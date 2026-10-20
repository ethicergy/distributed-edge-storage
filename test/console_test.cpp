/*
 * Operator console driven against the simulated cluster.
 */

#include <cstdio>
#include <iostream>
#include <string>

#include "cluster.h"
#include "console.h"
#include "test_support.h"

namespace {

std::string g_output;

void capture(void*, const char* text)
{
    g_output += text;
}

bool contains(const std::string& text)
{
    return g_output.find(text) != std::string::npos;
}

void run(sim::Cluster& cluster, console::Console& console, const char* line, std::uint64_t settle_ms = 300)
{
    console.execute(line, cluster.now());
    cluster.run_for(settle_ms);
}

} // namespace

int main()
{
    sim::Cluster cluster(41);
    cluster.enable_sensors(1000);
    REQUIRE(cluster.start_all());

    console::Console console(cluster.runtime(node::B), cluster.node(node::B).sampler.get(), capture, nullptr);

    cluster.on_node_tick = [&](node::Id id, std::uint64_t now) {
        if (id == node::B) {
            console.tick(now);
        }
    };

    run(cluster, console, "help");
    REQUIRE(contains("set <key> <value>"));
    REQUIRE(contains("fault <kind>"));
    // The middle and end of the text arrive too (it once overflowed).
    REQUIRE(contains("reboot                restart this node"));
    REQUIRE(contains("stale <key> <node>"));

    run(cluster, console, "status");
    REQUIRE(contains("node B  state ACTIVE  view ABCD"));

    run(cluster, console, "members");
    REQUIRE(contains("A  alive"));
    REQUIRE(contains("B  self"));

    g_output.clear();
    run(cluster, console, "set greeting hello distributed world");
    REQUIRE(contains("SET greeting: OK"));

    g_output.clear();
    run(cluster, console, "get greeting");
    REQUIRE(contains("greeting = \"hello distributed world\""));

    run(cluster, console, "ring greeting");
    REQUIRE(contains("primary"));

    g_output.clear();
    run(cluster, console, "del greeting");
    REQUIRE(contains("DEL greeting: OK"));

    g_output.clear();
    run(cluster, console, "get greeting");
    REQUIRE(contains("GET greeting: NOT_FOUND"));

    // Typed character by character with a backspace.
    g_output.clear();
    for (char c : std::string("set typed vx\balue\r")) {
        console.on_char(c, cluster.now());
    }
    cluster.run_for(300);
    REQUIRE(contains("SET typed: OK"));

    // Input hold (problem 5): echoed text is discarded, not executed,
    // including a partial line typed before the hold started.
    g_output.clear();
    for (char c : std::string("sta")) {
        console.on_char(c, cluster.now());
    }
    console.hold_input(cluster.now(), 1000);
    for (char c : std::string("set echoed junk\r")) {
        console.on_char(c, cluster.now());
    }
    cluster.run_for(500);
    REQUIRE(!contains("SET echoed"));
    REQUIRE(!contains("console: ignored"));  // reported only once the hold ends
    cluster.run_for(600);
    REQUIRE(contains("console: ignored 19 character(s)"));
    REQUIRE(!contains("SET echoed"));

    g_output.clear();
    cluster.run_for(100);
    REQUIRE(!contains("console: ignored"));  // reported once
    for (char c : std::string("tus\r")) {
        console.on_char(c, cluster.now());
    }
    cluster.run_for(300);
    REQUIRE(contains("unknown command 'tus'"));  // "sta" did not survive the hold
    run(cluster, console, "get echoed");
    REQUIRE(contains("GET echoed: NOT_FOUND"));

    // InputGuard, driven like the firmware's node task: hold at boot and
    // when USB comes back; never because the output paused mid-session
    // (the drain trigger that did that dropped real commands, problem 5).
    console::InputGuard guard(console);
    auto type_line = [&](const std::string& text) {
        for (char c : text) {
            guard.poll(true, cluster.now());
            console.on_char(c, cluster.now());
        }
    };
    auto pause = [&](std::uint64_t ms) {
        for (std::uint64_t t = 0; t < ms; t += 10) {
            guard.poll(true, cluster.now());
            cluster.run_for(10);
        }
    };

    g_output.clear();
    guard.begin(cluster.now());
    type_line("set boot early\r");
    pause(1100);
    REQUIRE(contains("console: ignored 15 character(s)"));
    REQUIRE(!contains("SET boot"));

    // Mid-session: output, then a 3 s pause with no output, then commands.
    g_output.clear();
    type_line("set mid before\r");
    pause(300);
    REQUIRE(contains("SET mid: OK"));
    pause(3000);
    g_output.clear();
    type_line("set mid after-pause\r");
    pause(300);
    type_line("get mid\r");
    pause(300);
    REQUIRE(!contains("console: ignored"));
    REQUIRE(contains("SET mid: OK"));
    REQUIRE(contains("mid = \"after-pause\""));

    // USB unplugged and plugged in again while powered: held for 1 s.
    g_output.clear();
    guard.poll(false, cluster.now());
    cluster.run_for(50);
    guard.poll(true, cluster.now());
    type_line("set replug early\r");
    pause(1100);
    REQUIRE(contains("console: ignored"));
    REQUIRE(!contains("SET replug"));
    g_output.clear();
    type_line("set replug later\r");
    pause(300);
    REQUIRE(contains("SET replug: OK"));

    g_output.clear();
    cluster.run_for(5000);
    run(cluster, console, "sensors", 1000);
    REQUIRE(contains("s/A/light = "));
    REQUIRE(contains("lux"));
    REQUIRE(contains("s/D/temp = "));
    REQUIRE(contains("degC"));

    g_output.clear();
    run(cluster, console, "dump");
    REQUIRE(contains("key(s); log"));

    g_output.clear();
    run(cluster, console, "stats");
    REQUIRE(contains("repl    started"));
    // The whole block arrives (it once overflowed printf's buffer).
    REQUIRE(contains("resync tx "));
    REQUIRE(contains("gc      orphans "));
    REQUIRE(contains("client  ok "));
    REQUIRE(contains("storage errors "));

    g_output.clear();
    run(cluster, console, "bench 20", 5000);
    REQUIRE(contains("bench: 20 ok, 0 failed"));

    g_output.clear();
    run(cluster, console, "fault drop-ack 1");
    REQUIRE(contains("fault armed"));

    g_output.clear();
    run(cluster, console, "fault loss 10");
    REQUIRE(contains("frame loss 10 % on every frame this node sends"));
    REQUIRE(cluster.runtime(node::B).faults().tx_loss_percent == 10);
    run(cluster, console, "fault loss 101");
    REQUIRE(contains("usage: fault"));
    run(cluster, console, "fault clear");
    REQUIRE(cluster.runtime(node::B).faults().tx_loss_percent == 0);

    run(cluster, console, "frobnicate");
    REQUIRE(contains("unknown command 'frobnicate'"));

    run(cluster, console, "format");
    REQUIRE(contains("format yes"));

    std::cout << "Console tests: PASS\n";
    return 0;
}
