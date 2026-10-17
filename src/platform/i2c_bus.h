#pragma once

/*
 * Owner of the one I2C bus (GPIO8/9): the AT24C256 on every node and
 * the BH1750 on node A (DECISIONS D2). Every bus transaction holds this
 * lock, and an EEPROM write keeps it until its write cycle finished
 * (ACK polling), so a sensor read can never start inside a write
 * cycle or between the halves of an EEPROM transaction. Today all
 * users run in the node task and the lock never waits; it makes the
 * rule hold if a driver ever moves to another task.
 */
namespace i2c_bus {

class Lock {
public:
    Lock();
    ~Lock();

    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

} // namespace i2c_bus
