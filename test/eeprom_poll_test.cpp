/*
 * eeprom::wait_for_write_cycle with a fake clock (BRINGUP_LOG
 * problem 15): the waiting task can be held off the CPU for longer
 * than the whole timeout (on node D, by the DHT11 read with interrupts
 * off), and the chip has finished by the time it runs again.
 */

#include <cstdint>
#include <iostream>

#include "eeprom_poll.h"
#include "test_support.h"

namespace {

struct FakeChip {
    std::uint32_t clock_ms = 0;
    std::uint32_t ready_at_ms = 0;
    int polls = 0;

    bool ready()
    {
        ++polls;
        return clock_ms >= ready_at_ms;
    }
};

eeprom::Result wait(FakeChip& chip, std::uint32_t pause_ms)
{
    return eeprom::wait_for_write_cycle(
        [&] { return chip.clock_ms; },
        [&] { return chip.ready(); },
        [&] { chip.clock_ms += pause_ms; }
    );
}

void test_normal_write_cycle()
{
    FakeChip chip;
    chip.ready_at_ms = 5; // typical AT24C256 page write

    REQUIRE(wait(chip, 1) == eeprom::Result::OK);
    REQUIRE(chip.polls == 6);
}

void test_task_resumes_late_chip_ready()
{
    // Busy at the first poll, then the task is off the CPU for 30 ms;
    // the chip finished after 5 ms.
    FakeChip chip;
    chip.ready_at_ms = 5;

    REQUIRE(wait(chip, 30) == eeprom::Result::OK);
}

void test_chip_never_ready_times_out()
{
    FakeChip chip;
    chip.ready_at_ms = 1000000;

    REQUIRE(wait(chip, 1) == eeprom::Result::WRITE_TIMEOUT);
    REQUIRE(chip.clock_ms >= eeprom::WRITE_CYCLE_TIMEOUT_MS);
}

void test_clock_wraps()
{
    FakeChip chip;
    chip.clock_ms = 0xFFFFFFFEu;
    chip.ready_at_ms = 0xFFFFFFFFu; // ready 1 ms later, before the wrap

    REQUIRE(wait(chip, 1) == eeprom::Result::OK);
}

} // namespace

int main()
{
    test_normal_write_cycle();
    test_task_resumes_late_chip_ready();
    test_chip_never_ready_times_out();
    test_clock_wraps();

    std::cout << "EEPROM poll tests: PASS\n";
    return 0;
}
