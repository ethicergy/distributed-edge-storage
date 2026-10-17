#pragma once

#include <cstddef>
#include <cstdint>

#include "protocol.h"

/*
 * Serial framing between the Raspberry Pi and the bridge node
 * (frozen by the protocol):
 *
 *     AA 55 | LEN | ENVELOPE (LEN bytes) | CRC
 *
 * ENVELOPE  one protocol packet: 22-byte header + payload,
 *           22 <= LEN <= 250.
 * CRC       CRC-8, polynomial 0x07, init 0x00, no reflection, no
 *           final xor (CRC-8/SMBUS), computed over LEN and ENVELOPE.
 *           Check value: crc8("123456789") = 0xF4.
 *
 * The decoder resynchronises on garbage, bad lengths and bad CRCs by
 * rescanning the bytes after the failed frame's first sync byte.
 */
namespace uart {

constexpr std::uint8_t SYNC_0 = 0xAA;
constexpr std::uint8_t SYNC_1 = 0x55;

constexpr std::size_t MIN_ENVELOPE = protocol::HEADER_SIZE;
constexpr std::size_t MAX_ENVELOPE = protocol::MAX_PACKET_SIZE;

// sync (2) + len (1) + envelope + crc (1)
constexpr std::size_t MAX_FRAME = 2 + 1 + MAX_ENVELOPE + 1;

std::uint8_t crc8(const std::uint8_t* data, std::size_t length, std::uint8_t crc = 0x00);

bool encode(
    const std::uint8_t* envelope,
    std::size_t envelope_length,
    std::uint8_t* out,
    std::size_t out_capacity,
    std::size_t& out_length
);

struct DecoderStats {
    std::uint32_t frames;
    std::uint32_t crc_errors;
    std::uint32_t length_errors;
    std::uint32_t discarded_bytes;
};

/*
 * Byte-stream decoder. push() raw bytes as they arrive, then call
 * next() until it returns false; each true return makes one valid
 * envelope available through envelope()/envelope_length() until the
 * next push() or next().
 *
 * On a bad length or CRC only the first sync byte is discarded and
 * scanning restarts right after it, so a valid frame hidden behind
 * line noise (even inside a bogus frame) is still found.
 */
class Decoder {
public:
    void push(const std::uint8_t* data, std::size_t length);
    void push(std::uint8_t byte) { push(&byte, 1); }

    bool next();

    // Call when the line has been idle for a while with a partial
    // frame buffered (e.g. a noise byte announced a long frame): the
    // partial frame is abandoned and scanning restarts after its
    // first sync byte. Returns true if anything was buffered.
    bool abandon_partial();

    const std::uint8_t* envelope() const { return frame_ + 3; }
    std::size_t envelope_length() const { return frame_[2]; }

    const DecoderStats& stats() const { return stats_; }

    void reset();

private:
    void drop(std::size_t count, bool discarded);

    static constexpr std::size_t QUEUE_CAPACITY = 2 * MAX_FRAME;

    std::uint8_t queue_[QUEUE_CAPACITY]{};
    std::size_t count_ = 0;
    std::uint8_t frame_[MAX_FRAME]{};
    DecoderStats stats_{};
};

} // namespace uart
