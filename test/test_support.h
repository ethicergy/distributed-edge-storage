#pragma once

/*
 * Minimal check macro for the standalone host tests.
 * Unlike assert() it is not disabled by NDEBUG, and it says where
 * and what failed.
 */

#include <cstdio>
#include <cstdlib>

#define REQUIRE(condition)                                         \
    do {                                                           \
        if (!(condition)) {                                        \
            std::fprintf(                                          \
                stderr,                                            \
                "%s:%d: REQUIRE(%s) failed\n",                     \
                __FILE__,                                          \
                __LINE__,                                          \
                #condition                                         \
            );                                                     \
            std::exit(1);                                          \
        }                                                          \
    } while (0)
