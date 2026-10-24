/*
 * Real sensor drivers (D1), one per node, behind config::REAL_SENSOR_NODES.
 *
 *   A  BH1750 (GY-302)   I2C 0x23 on the EEPROM bus    continuous high-res
 *   B  AM312 PIR         OUT  -> config::PIR_PIN       polled by MotionSource
 *   C  LM393 sound (AO)  AO   -> config::SOUND_AO_PIN  own task, ADC1 at 1 kHz
 *   D  DHT11 module      DATA -> config::DHT11_PIN     own task, 1 read / 2 s
 *
 * The bridge node A gets the BH1750: its driver never disables
 * interrupts, so the Pi link is never held up by a sensor.
 *
 * The logic (windows, settling, events, keep-last-good, fallback) is in
 * src/sensor_sources.cpp and tested on the host; this file only touches
 * the hardware. Nothing here runs unless the node's bit is set.
 */

#include "sensors.h"

#include <Arduino.h>
#include <DHT.h>
#include <Wire.h>
#include <esp_adc_cal.h>

#include <cmath>

#include "config.h"
#include "i2c_bus.h"
#include "platform_time.h"
#include "sensor_sources.h"

namespace platform_sensors {

namespace {

// ------------------------------------------------------------------
// DHT11 (node D): read in its own low-priority task, so the node task
// (radio, storage, console) never waits for it. One read takes about
// 25 ms: an 18 ms start pulse (the task sleeps) and ~5 ms with
// interrupts off while the 40 bits arrive.
// ------------------------------------------------------------------

constexpr std::uint32_t DHT_TASK_STACK_BYTES = 3072;
constexpr UBaseType_t DHT_TASK_PRIORITY = 1; // below the node task (3)
constexpr std::uint32_t DHT_POWER_UP_MS = 1500;

class Dht11Input final : public sensor::ValueInput {
public:
    bool begin() override
    {
        dht_.begin();

        task_ = xTaskCreateStatic(
            task_entry,
            "dht11",
            DHT_TASK_STACK_BYTES / sizeof(StackType_t),
            this,
            DHT_TASK_PRIORITY,
            stack_,
            &task_control_
        );

        return task_ != nullptr;
    }

    sensor::ValueAttempts take() override
    {
        portENTER_CRITICAL(&lock_);
        const sensor::ValueAttempts out = attempts_;
        attempts_ = sensor::ValueAttempts{};
        portEXIT_CRITICAL(&lock_);
        return out;
    }

private:
    static void task_entry(void* context)
    {
        static_cast<Dht11Input*>(context)->run();
    }

    [[noreturn]] void run()
    {
        vTaskDelay(pdMS_TO_TICKS(DHT_POWER_UP_MS));

        for (;;) {
            float celsius = NAN;

            {
                // The read turns interrupts off (~5 ms good, far longer
                // when the sensor does not answer). Holding the bus lock
                // keeps it out of every EEPROM transaction, write cycle
                // included: an EEPROM op that blocks (I2C interrupt,
                // delay(1) in the write-cycle poll) would otherwise let
                // this task run and overrun the op's deadline
                // (BRINGUP_LOG problem 15).
                i2c_bus::Lock bus;

                // force = true: the library's own 2 s cache is not needed,
                // this loop already keeps to DHT11_READ_INTERVAL_MS.
                celsius = dht_.readTemperature(false, true);
            }

            portENTER_CRITICAL(&lock_);

            if (std::isnan(celsius)) {
                ++attempts_.failed;
            } else {
                ++attempts_.ok;
                attempts_.last_good_milli = static_cast<std::int32_t>(std::lround(celsius * 1000.0f));
                attempts_.last_good_ms = platform_time::now_ms();
            }

            portEXIT_CRITICAL(&lock_);

            vTaskDelay(pdMS_TO_TICKS(config::DHT11_READ_INTERVAL_MS));
        }
    }

    DHT dht_{config::DHT11_PIN, DHT11};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    sensor::ValueAttempts attempts_{};

    TaskHandle_t task_ = nullptr;
    StaticTask_t task_control_{};
    StackType_t stack_[DHT_TASK_STACK_BYTES / sizeof(StackType_t)]{};
};

// ------------------------------------------------------------------
// AM312 (node B): push-pull OUT at 3.3 V; the pull-down keeps an
// unplugged sensor at "no motion".
// ------------------------------------------------------------------

class PirInput final : public sensor::DigitalInput {
public:
    bool begin() override
    {
        pinMode(config::PIR_PIN, INPUT_PULLDOWN);
        return true;
    }

    bool level() override { return digitalRead(config::PIR_PIN) == HIGH; }
};

// ------------------------------------------------------------------
// LM393 AO (node C): a task one priority above the node task takes one
// ADC1 conversion per tick (config::SOUND_SAMPLE_INTERVAL_MS) and hands
// each block of config::SOUND_BLOCK_SAMPLES to the node task over a
// small ring. A sample is a single conversion plus a linear mV
// conversion (`status` shows the measured us/sample), so the node task
// keeps nearly all of the CPU. No interrupts are disabled and no bus
// lock is taken: the ADC is not on I2C.
// ------------------------------------------------------------------

constexpr std::uint32_t SOUND_TASK_STACK_BYTES = 2048;
constexpr UBaseType_t SOUND_TASK_PRIORITY = 4; // above the node task (3): steady 1 kHz
constexpr std::size_t SOUND_RING_BLOCKS = 64;  // 640 ms of node-task delay
constexpr std::uint32_t ADC_DEFAULT_VREF_MV = 1100; // used only without eFuse calibration
constexpr std::uint32_t ADC_RAW_MAX = 4095;

static_assert(
    static_cast<int>(ADC_11db) == 3 && static_cast<int>(ADC_ATTEN_DB_12) == 3,
    "config::SOUND_ADC_ATTENUATION uses the adc_attenuation_t / adc_atten_t numbering"
);

class SoundAoInput final : public sensor::SoundBlockInput {
public:
    bool begin() override
    {
        analogReadResolution(12);
        analogSetPinAttenuation(
            config::SOUND_AO_PIN,
            static_cast<adc_attenuation_t>(config::SOUND_ADC_ATTENUATION)
        );

        // Once, not per sample: analogReadMilliVolts() re-characterises
        // (eFuse reads) on every call.
        esp_adc_cal_characterize(
            ADC_UNIT_1,
            static_cast<adc_atten_t>(config::SOUND_ADC_ATTENUATION),
            ADC_WIDTH_BIT_12,
            ADC_DEFAULT_VREF_MV,
            &chars_
        );
        clip_low_mv_ = static_cast<std::uint16_t>(esp_adc_cal_raw_to_voltage(0, &chars_));
        clip_high_mv_ = static_cast<std::uint16_t>(esp_adc_cal_raw_to_voltage(ADC_RAW_MAX, &chars_));

        task_ = xTaskCreateStatic(
            task_entry,
            "sound",
            SOUND_TASK_STACK_BYTES / sizeof(StackType_t),
            this,
            SOUND_TASK_PRIORITY,
            stack_,
            &task_control_
        );

        return task_ != nullptr;
    }

    std::size_t take(sensor::SoundBlock* out, std::size_t capacity) override
    {
        portENTER_CRITICAL(&lock_);

        std::size_t n = 0;

        while (n < capacity && count_ > 0) {
            out[n++] = ring_[tail_];
            tail_ = (tail_ + 1) % SOUND_RING_BLOCKS;
            --count_;
        }

        portEXIT_CRITICAL(&lock_);
        return n;
    }

    std::uint32_t dropped() const override { return dropped_; }

    std::uint32_t sample_cost_us() const override
    {
        portENTER_CRITICAL(&lock_);
        const std::uint32_t cost = cost_samples_ == 0 ? 0 : static_cast<std::uint32_t>(cost_us_ / cost_samples_);
        portEXIT_CRITICAL(&lock_);
        return cost;
    }

private:
    static void task_entry(void* context)
    {
        static_cast<SoundAoInput*>(context)->run();
    }

    [[noreturn]] void run()
    {
        std::uint16_t samples[config::SOUND_BLOCK_SAMPLES];
        std::size_t n = 0;
        std::uint32_t block_cost_us = 0;
        TickType_t wake = xTaskGetTickCount();

        for (;;) {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(config::SOUND_SAMPLE_INTERVAL_MS));

            const std::uint32_t start_us = micros();
            const std::uint16_t raw = analogReadRaw(config::SOUND_AO_PIN);
            samples[n++] = static_cast<std::uint16_t>(esp_adc_cal_raw_to_voltage(raw, &chars_));
            block_cost_us += micros() - start_us;

            if (n < config::SOUND_BLOCK_SAMPLES) {
                continue;
            }

            const sensor::SoundBlock block = sensor::summarize_sound_block(
                samples, n, clip_low_mv_, clip_high_mv_, platform_time::now_ms()
            );

            portENTER_CRITICAL(&lock_);

            if (count_ == SOUND_RING_BLOCKS) {
                ++dropped_;
            } else {
                ring_[(tail_ + count_) % SOUND_RING_BLOCKS] = block;
                ++count_;
            }

            cost_us_ += block_cost_us;
            cost_samples_ += n;

            portEXIT_CRITICAL(&lock_);

            n = 0;
            block_cost_us = 0;
        }
    }

    esp_adc_cal_characteristics_t chars_{};
    std::uint16_t clip_low_mv_ = 0;
    std::uint16_t clip_high_mv_ = 0;

    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    sensor::SoundBlock ring_[SOUND_RING_BLOCKS]{};
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    volatile std::uint32_t dropped_ = 0;
    std::uint64_t cost_us_ = 0;
    std::uint64_t cost_samples_ = 0;

    TaskHandle_t task_ = nullptr;
    StaticTask_t task_control_{};
    StackType_t stack_[SOUND_TASK_STACK_BYTES / sizeof(StackType_t)]{};
};

// ------------------------------------------------------------------
// BH1750 (node A) on the EEPROM bus, continuous high-resolution mode:
// a read is one 2-byte transfer (~0.1 ms at 400 kHz), under the bus
// lock (i2c_bus.h).
// ------------------------------------------------------------------

constexpr std::uint8_t BH1750_POWER_ON = 0x01;
constexpr std::uint8_t BH1750_CONTINUOUS_HIGH_RES = 0x10;

class Bh1750Source final : public sensor::Source {
public:
    bool begin() override
    {
        i2c_bus::Lock lock;
        return command(BH1750_POWER_ON) && command(BH1750_CONTINUOUS_HIGH_RES);
    }

    sensor::Kind kind() const override { return sensor::Kind::LIGHT; }
    bool simulated() const override { return false; }

    bool read(std::uint64_t, sensor::Sample& sample) override
    {
        std::uint16_t raw = 0;

        {
            i2c_bus::Lock lock;

            if (Wire.requestFrom(config::BH1750_I2C_ADDRESS, static_cast<std::uint8_t>(2)) != 2) {
                ++failures_;
                return false;
            }

            raw = static_cast<std::uint16_t>(Wire.read() << 8);
            raw = static_cast<std::uint16_t>(raw | Wire.read());
        }

        // lux = raw / 1.2 (datasheet, MTreg default).
        sample = sensor::Sample{};
        sample.value_milli = static_cast<std::int32_t>(static_cast<std::uint32_t>(raw) * 10000u / 12u);
        return true;
    }

    std::uint32_t failures() const override { return failures_; }

private:
    static bool command(std::uint8_t opcode)
    {
        Wire.beginTransmission(config::BH1750_I2C_ADDRESS);
        Wire.write(opcode);
        return Wire.endTransmission(true) == 0;
    }

    std::uint32_t failures_ = 0;
};

} // namespace

sensor::Source* real_source(node::Id id)
{
    // Constructed on first use only: a node whose bit is clear never
    // touches these pins.
    switch (id) {
        case node::A: {
            static Bh1750Source source;
            return &source;
        }
        case node::B: {
            static PirInput pir;
            static sensor::MotionSource source(pir);
            return &source;
        }
        case node::C: {
            static SoundAoInput mic;
            static sensor::SoundSource source(mic);
            return &source;
        }
        case node::D: {
            static Dht11Input dht;
            static sensor::LatestValueSource source(sensor::Kind::TEMPERATURE, dht);
            return &source;
        }
        default:
            return nullptr;
    }
}

} // namespace platform_sensors
