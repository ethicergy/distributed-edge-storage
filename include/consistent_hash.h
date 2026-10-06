#pragma once

#include <cstddef>
#include <cstdint>

namespace consistent_hash {

constexpr std::uint32_t FNV1A_OFFSET_BASIS =
    2166136261u;

constexpr std::uint32_t FNV1A_PRIME =
    16777619u;

std::uint32_t fnv1a(
    const std::uint8_t* data,
    std::size_t length
);

} // namespace consistent_hash
