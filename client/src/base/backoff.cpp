#include "base/backoff.h"

#include <algorithm>

namespace wb {

std::chrono::milliseconds Backoff::next() {
    const long long base = initial_.count();
    long long ceiling = base;
    for (int i = 0; i < attempt_ && ceiling < cap_.count(); ++i) ceiling *= 2;
    ceiling = std::min(ceiling, static_cast<long long>(cap_.count()));
    ++attempt_;
    const long long half = ceiling / 2;
    std::uniform_int_distribution<long long> dist(0, std::max(0LL, ceiling - half));
    return std::chrono::milliseconds(half + dist(rng_));
}

}  // namespace wb
