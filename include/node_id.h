#pragma once

#include <cstdint>

namespace node {

using Id = std::uint8_t;

constexpr Id INVALID = 0x00;

constexpr Id A = 0x01;
constexpr Id B = 0x02;
constexpr Id C = 0x03;
constexpr Id D = 0x04;

constexpr Id MIN = A;
constexpr Id MAX = D;

constexpr Id BROADCAST = 0xFF;

} // namespace node