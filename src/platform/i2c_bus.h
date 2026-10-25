#pragma once

/*
 * Owner of the one I2C bus (GPIO8/9): the AT24C256 on every node and
 * the BH1750 on node A (DECISIONS D2). Every bus transaction holds this
 * lock, and an EEPROM write keeps it until its write cycle finished
 * (ACK polling), so a sensor read can never start inside a write
 * cycle or between the halves of an EEPROM transaction. The DHT11 task
 * on node D takes it too, although the DHT11 is not on the bus: its
 * read disables interrupts and must not land inside an EEPROM
 * transaction (BRINGUP_LOG problem 15). The node task can therefore
 * wait up to one DHT11 read (~25 ms) for the bus on D; the mutex's
 * priority inheritance lifts the DHT task while it holds it.
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
