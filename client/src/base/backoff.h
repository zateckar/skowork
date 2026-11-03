#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace wb {

// Jittered exponential backoff ("equal jitter": half fixed, half random), capped
// (client/control-channel Push Connection: capped at 5 minutes; client/model-access Retries With Backoff).
class Backoff {
public:
    Backoff(std::chrono::milliseconds initial, std::chrono::milliseconds cap, std::uint64_t seed = std::random_device{}())
        : initial_(initial), cap_(cap), rng_(seed) {}

    std::chrono::milliseconds next();
    void reset() { attempt_ = 0; }
    [[nodiscard]] int attempts() const { return attempt_; }

private:
    std::chrono::milliseconds initial_, cap_;
    std::mt19937_64 rng_;
    int attempt_ = 0;
};

}  // namespace wb
