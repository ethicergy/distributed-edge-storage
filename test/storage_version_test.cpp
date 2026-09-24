#include <cassert>
#include <cstdint>
#include <iostream>

#include "node_id.h"
#include "storage.h"

using namespace storage;

int main() {
    // Higher HLC must win regardless of origin ID.
    Version older_hlc{
        .hlc = 100,
        .origin = node::D
    };

    Version newer_hlc{
        .hlc = 200,
        .origin = node::A
    };

    assert(compare_versions(older_hlc, newer_hlc) < 0);
    assert(compare_versions(newer_hlc, older_hlc) > 0);

    // When HLCs are equal, higher origin ID must win.
    Version lower_origin{
        .hlc = 500,
        .origin = node::A
    };

    Version higher_origin{
        .hlc = 500,
        .origin = node::D
    };

    assert(compare_versions(lower_origin, higher_origin) < 0);
    assert(compare_versions(higher_origin, lower_origin) > 0);

    // Exactly equal versions must compare equal.
    Version equal_left{
        .hlc = 1000,
        .origin = node::B
    };

    Version equal_right{
        .hlc = 1000,
        .origin = node::B
    };

    assert(compare_versions(equal_left, equal_right) == 0);
    assert(compare_versions(equal_right, equal_left) == 0);

    // Test the smallest valid node ID in a tie.
    Version origin_a{
        .hlc = 2000,
        .origin = node::A
    };

    Version origin_b{
        .hlc = 2000,
        .origin = node::B
    };

    assert(compare_versions(origin_a, origin_b) < 0);

    // Test HLC equality with the largest valid node ID.
    Version origin_c{
        .hlc = 3000,
        .origin = node::C
    };

    Version origin_d{
        .hlc = 3000,
        .origin = node::D
    };

    assert(compare_versions(origin_c, origin_d) < 0);

    // Sanity: comparison must be antisymmetric for these cases.
    assert(
        compare_versions(older_hlc, newer_hlc) ==
        -compare_versions(newer_hlc, older_hlc)
    );

    assert(
        compare_versions(lower_origin, higher_origin) ==
        -compare_versions(higher_origin, lower_origin)
    );

    std::cout << "Storage version comparison tests: PASS\n";

    return 0;
}