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

    g_output.clear();
    cluster.run_for(5000);
    run(cluster, console, "sensors", 1000);
    REQUIRE(contains("s/A/temp = "));
    REQUIRE(contains("degC"));
    REQUIRE(contains("s/D/light = "));

    g_output.clear();
    run(cluster, console, "dump");
    REQUIRE(contains("key(s); log"));

    run(cluster, console, "stats");
    REQUIRE(contains("repl    started"));

    g_output.clear();
    run(cluster, console, "bench 20", 5000);
    REQUIRE(contains("bench: 20 ok, 0 failed"));

    g_output.clear();
    run(cluster, console, "fault drop-ack 1");
    REQUIRE(contains("fault armed"));

    run(cluster, console, "frobnicate");
    REQUIRE(contains("unknown command 'frobnicate'"));

    run(cluster, console, "format");
    REQUIRE(contains("format yes"));

    std::cout << "Console tests: PASS\n";
    return 0;
}
