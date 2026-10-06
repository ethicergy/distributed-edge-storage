#include "consistent_hash.h"

namespace consistent_hash {

std::uint32_t fnv1a(
    const std::uint8_t* data,
    std::size_t length
)
{
    if (data == nullptr && length != 0) {
        return 0;
    }

    std::uint32_t hash =
        FNV1A_OFFSET_BASIS;

    for (std::size_t i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= FNV1A_PRIME;
    }

    return hash;
}

} // namespace consistent_hash
