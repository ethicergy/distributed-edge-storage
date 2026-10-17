/*
 * edge_sim: the 4-node cluster on a PC, in real time.
 *
 * Runs the real firmware logic (runtime, storage on fake EEPROMs,
 * sensors, console, Pi bridge) over a simulated radio. Useful for
 * demos, for trying console commands before touching hardware, and
 * for running the Raspberry Pi software against a bridge without
 * boards: the bridge of node A is exposed on a pseudo-terminal.
 *
 *   build/host/edge_sim [--speed N] [--seed S] [--quiet]
 *
 * Input lines:
 *   A: <console command>     run a console command on node A (B, C, D)
 *   <console command>        on the currently selected node
 *   use B                    select node B
 *   off C | on C | reboot C  power control
 *   cut A C | heal           radio partition between two nodes / undo
 *   loss 0.2                 random frame loss on every link
 *   wipe C                   erase C's EEPROM (while it is off)
 *   quit
 */

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "bridge.h"
#include "cluster.h"
#include "console.h"

namespace {

int g_pty_master = -1;
bool g_quiet = false;

void console_write(void*, const char* text)
{
    std::fputs(text, stdout);
    std::fflush(stdout);
}

void bridge_write(void*, const std::uint8_t* data, std::size_t length)
{
    if (g_pty_master >= 0) {
        const ssize_t written = ::write(g_pty_master, data, length);
        (void)written;
    }
}

node::Id parse_node(const std::string& text)
{
    if (text.empty()) return node::INVALID;
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    return (c >= 'A' && c <= 'D') ? static_cast<node::Id>(node::MIN + (c - 'A')) : node::INVALID;
}

std::string trim(const std::string& text)
{
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

} // namespace

int main(int argc, char** argv)
{
    double speed = 1.0;
    std::uint32_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            speed = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--quiet") == 0) {
            g_quiet = true;
        } else {
            std::fprintf(stderr, "usage: %s [--speed N] [--seed S] [--quiet]\n", argv[0]);
            return 2;
        }
    }

    if (speed <= 0.0) speed = 1.0;

    // Pseudo-terminal for the Pi side.
    int slave = -1;
    char slave_name[128] = {};

    if (openpty(&g_pty_master, &slave, slave_name, nullptr, nullptr) == 0) {
        termios attributes{};
        tcgetattr(slave, &attributes);
        cfmakeraw(&attributes);
        tcsetattr(slave, TCSANOW, &attributes);
        fcntl(g_pty_master, F_SETFL, fcntl(g_pty_master, F_GETFL) | O_NONBLOCK);
    } else {
        g_pty_master = -1;
    }

    sim::Cluster cluster(seed, !g_quiet);
    cluster.enable_sensors(config::SENSOR_PERIOD_MS);

    for (node::Id id = node::MIN; id <= node::MAX; ++id) {
        cluster.power_on(id);
        cluster.runtime(id).set_log_level(g_quiet ? edge::LogLevel::OFF : edge::LogLevel::INFO);
    }

    std::unique_ptr<console::Console> consoles[sim::NODE_COUNT];
    std::unique_ptr<bridge::Bridge> pi_bridge;

    // Consoles and the bridge follow their node across reboots.
    edge::Runtime* bound[sim::NODE_COUNT] = {};

    auto rebind = [&] {
        for (node::Id id = node::MIN; id <= node::MAX; ++id) {
            const std::size_t i = id - node::MIN;
            edge::Runtime* runtime = cluster.powered(id) ? &cluster.runtime(id) : nullptr;

            if (runtime == bound[i]) {
                continue;
            }

            bound[i] = runtime;
            consoles[i].reset();

            if (runtime != nullptr) {
                runtime->set_log_level(g_quiet ? edge::LogLevel::OFF : edge::LogLevel::INFO);
                consoles[i] = std::make_unique<console::Console>(
                    *runtime, cluster.node(id).sampler.get(), console_write, nullptr);
            }

            if (id == config::BRIDGE_NODE_ID) {
                pi_bridge.reset();
                if (runtime != nullptr) {
                    pi_bridge = std::make_unique<bridge::Bridge>(*runtime, bridge_write, nullptr);
                }
            }
        }
    };

    rebind();

    cluster.on_node_tick = [&](node::Id id, std::uint64_t now) {
        const std::size_t i = id - node::MIN;

        if (consoles[i] && bound[i] == &cluster.runtime(id)) {
            consoles[i]->tick(now);
        }

        if (id == config::BRIDGE_NODE_ID && pi_bridge && bound[i] == &cluster.runtime(id)) {
            pi_bridge->tick(now);
        }
    };

    std::printf("edge_sim: 4 nodes, seed %u, speed x%.1f\n", seed, speed);

    if (g_pty_master >= 0) {
        std::printf("Pi bridge (node %c) on pseudo-terminal: %s\n",
                    static_cast<char>('A' + config::BRIDGE_NODE_ID - 1), slave_name);
        std::printf("  try: python3 -m edgekv --port %s sensors   (from the pi/ directory)\n", slave_name);
    }

    std::printf("commands: 'A: <console cmd>', 'use B', off/on/reboot <n>, cut <a> <b>, heal, loss <p>, wipe <n>, quit\n");
    std::fflush(stdout);

    node::Id selected = node::A;
    std::string pending_input;
    bool running = true;

    const auto start = std::chrono::steady_clock::now();
    const std::uint64_t virtual_start = cluster.now();

    while (running) {
        // Pace virtual time against the wall clock.
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const std::uint64_t target = virtual_start + static_cast<std::uint64_t>(elapsed_ms * speed);

        while (cluster.now() < target) {
            cluster.step(1);
            rebind();
        }

        // Pi bytes.
        if (g_pty_master >= 0) {
            std::uint8_t buffer[256];
            const ssize_t got = ::read(g_pty_master, buffer, sizeof(buffer));

            if (got > 0 && pi_bridge) {
                pi_bridge->on_bytes(buffer, static_cast<std::size_t>(got), cluster.now());
            }
        }

        // Operator input.
        pollfd input{STDIN_FILENO, POLLIN, 0};

        if (poll(&input, 1, 1) > 0 && (input.revents & (POLLIN | POLLHUP))) {
            char chunk[512];
            const ssize_t got = ::read(STDIN_FILENO, chunk, sizeof(chunk));

            if (got <= 0) {
                running = false;
                continue;
            }

            pending_input.append(chunk, static_cast<std::size_t>(got));
        }

        std::size_t newline;

        while ((newline = pending_input.find('\n')) != std::string::npos) {
            std::string line = trim(pending_input.substr(0, newline));
            pending_input.erase(0, newline + 1);

            if (line.empty()) continue;

            node::Id target_node = selected;

            if (line.size() >= 2 && line[1] == ':' && parse_node(line.substr(0, 1)) != node::INVALID) {
                target_node = parse_node(line.substr(0, 1));
                line = trim(line.substr(2));
            }

            char word[32] = {};
            char arg1[32] = {};
            char arg2[32] = {};
            std::sscanf(line.c_str(), "%31s %31s %31s", word, arg1, arg2);
            const std::string command = word;

            if (command == "quit" || command == "exit") {
                running = false;
            } else if (command == "use" && parse_node(arg1) != node::INVALID) {
                selected = parse_node(arg1);
                std::printf("console -> node %c\n", 'A' + selected - 1);
            } else if (command == "off" && parse_node(arg1) != node::INVALID) {
                cluster.power_off(parse_node(arg1));
            } else if (command == "on" && parse_node(arg1) != node::INVALID) {
                cluster.power_on(parse_node(arg1));
            } else if (command == "reboot" && line.find(' ') != std::string::npos && parse_node(arg1) != node::INVALID) {
                cluster.reboot(parse_node(arg1));
            } else if (command == "cut" && parse_node(arg1) != node::INVALID && parse_node(arg2) != node::INVALID) {
                cluster.network().set_link(parse_node(arg1), parse_node(arg2), false);
                std::printf("link %s-%s cut\n", arg1, arg2);
            } else if (command == "heal") {
                for (node::Id a = node::MIN; a <= node::MAX; ++a)
                    for (node::Id b = node::MIN; b <= node::MAX; ++b)
                        if (a != b) cluster.network().set_link(a, b, true);
                std::printf("all links restored\n");
            } else if (command == "loss") {
                cluster.network().set_loss(std::atof(arg1));
                std::printf("frame loss %.2f\n", std::atof(arg1));
            } else if (command == "wipe" && parse_node(arg1) != node::INVALID) {
                if (cluster.powered(parse_node(arg1))) {
                    std::printf("power the node off first\n");
                } else {
                    cluster.node(parse_node(arg1)).eeprom.erase();
                    std::printf("EEPROM of %s erased\n", arg1);
                }
            } else {
                rebind();
                console::Console* console = consoles[target_node - node::MIN].get();

                if (console == nullptr) {
                    std::printf("node %c is powered off\n", 'A' + target_node - 1);
                } else {
                    console->execute(line.c_str(), cluster.now());
                }
            }

            rebind();
            std::fflush(stdout);
        }
    }

    if (g_pty_master >= 0) {
        close(g_pty_master);
    }

    return 0;
}
