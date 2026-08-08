#pragma once

// MSVC doesn't define __BYTE_ORDER__; Windows is always little-endian
#if defined(_MSC_VER) && !defined(__BYTE_ORDER__)
#define __BYTE_ORDER__            1234
#define __ORDER_LITTLE_ENDIAN__   1234
#define __ORDER_BIG_ENDIAN__      4321
#endif

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LOW_HIGH_UNION(name, low, high) \
    union { \
        struct { \
            uint8_t low; \
            uint8_t high; \
        }; \
        uint16_t name; \
    }
#define ENDIAN_STRUCT_MEMBERS(low, high) \
    low; \
    high
#else
#define LOW_HIGH_UNION(name, low, high) \
    union { \
        struct { \
            uint8_t high; \
            uint8_t low; \
        }; \
        uint16_t name; \
    }
#define ENDIAN_STRUCT_MEMBERS(low, high) \
    high; \
    low
#endif
