#pragma once

#include <sstream>
#include <stdexcept>

#define DS_CHECK(cond)                                                            \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::ostringstream oss;                                               \
            oss << "CHECK failed: " #cond << " at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str());                                  \
        }                                                                          \
    } while (0)

#define DS_CHECK_EQ(a, b)                                                                 \
    do {                                                                                  \
        auto va = (a);                                                                    \
        auto vb = (b);                                                                    \
        if (!(va == vb)) {                                                                \
            std::ostringstream oss;                                                       \
            oss << "CHECK_EQ failed: " #a " == " #b << " at " << __FILE__ << ":" << __LINE__; \
            throw std::runtime_error(oss.str());                                          \
        }                                                                                  \
    } while (0)
