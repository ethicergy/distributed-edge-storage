#include "uart_frame.h"

#include <cstring>

namespace uart {

std::uint8_t crc8(const std::uint8_t* data, std::size_t length, std::uint8_t crc)
{
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80u)
                ? static_cast<std::uint8_t>((crc << 1) ^ 0x07u)
                : static_cast<std::uint8_t>(crc << 1);
        }
    }

    return crc;
}

bool encode(
    const std::uint8_t* envelope,
    std::size_t envelope_length,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& out_length
)
{
    out_length = 0;

    if (envelope == nullptr ||
        out == nullptr ||
        envelope_length < MIN_ENVELOPE ||
        envelope_length > MAX_ENVELOPE ||
        out_capacity < envelope_length + 4) {
        return false;
    }

    out[0] = SYNC_0;
    out[1] = SYNC_1;
    out[2] = static_cast<std::uint8_t>(envelope_length);
    std::memcpy(out + 3, envelope, envelope_length);
    out[3 + envelope_length] = crc8(out + 2, envelope_length + 1);

    out_length = envelope_length + 4;
    return true;
}

void Decoder::reset()
{
    count_ = 0;
}

void Decoder::drop(std::size_t count, bool discarded)
{
    if (count > count_) {
        count = count_;
    }

    std::memmove(queue_, queue_ + count, count_ - count);
    count_ -= count;

    if (discarded) {
        stats_.discarded_bytes += static_cast<std::uint32_t>(count);
    }
}

void Decoder::push(const std::uint8_t* data, std::size_t length)
{
    if (data == nullptr) {
        return;
    }

    for (std::size_t i = 0; i < length; ++i) {
        if (count_ == QUEUE_CAPACITY) {
            // Nobody is calling next(); keep the newest bytes.
            drop(1, true);
        }

        queue_[count_++] = data[i];
    }
}

bool Decoder::abandon_partial()
{
    if (count_ == 0) {
        return false;
    }

    drop(1, true);
    return true;
}

bool Decoder::next()
{
    for (;;) {
        // Find AA 55.
        std::size_t start = 0;

        while (start < count_ &&
               !(queue_[start] == SYNC_0 &&
                 (start + 1 == count_ || queue_[start + 1] == SYNC_1))) {
            ++start;
        }

        if (start > 0) {
            drop(start, true);
        }

        if (count_ < 3) {
            return false;
        }

        const std::size_t envelope_length = queue_[2];

        if (envelope_length < MIN_ENVELOPE || envelope_length > MAX_ENVELOPE) {
            ++stats_.length_errors;
            drop(1, true);
            continue;
        }

        if (count_ < envelope_length + 4) {
            return false;
        }

        const std::uint8_t expected = crc8(queue_ + 2, envelope_length + 1);

        if (expected != queue_[3 + envelope_length]) {
            ++stats_.crc_errors;
            drop(1, true);
            continue;
        }

        std::memcpy(frame_, queue_, envelope_length + 4);
        drop(envelope_length + 4, false);

        ++stats_.frames;
        return true;
    }
}

} // namespace uart
