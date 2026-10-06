#include <cassert>
#include <cstdint>

#include "consistent_hash.h"

int main()
{
    {
        const std::uint8_t data[] = {};
        assert(
            consistent_hash::fnv1a(
                data,
                0
            ) == 0x811c9dc5u
        );
    }

    {
        const std::uint8_t data[] = {
            'a'
        };

        assert(
            consistent_hash::fnv1a(
                data,
                sizeof(data)
            ) == 0xe40c292cu
        );
    }

    {
        const std::uint8_t data[] = {
            'f','o','o','b','a','r'
        };

        assert(
            consistent_hash::fnv1a(
                data,
                sizeof(data)
            ) == 0xbf9cf968u
        );
    }

    {
        const std::uint8_t a[] = {
            't','e','m','p'
        };

        const std::uint8_t b[] = {
            't','e','m','p','e'
        };

        assert(
            consistent_hash::fnv1a(
                a,
                sizeof(a)
            ) !=
            consistent_hash::fnv1a(
                b,
                sizeof(b)
            )
        );
    }

    {
        assert(
            consistent_hash::fnv1a(
                nullptr,
                1
            ) == 0
        );
    }

    return 0;
}
