#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace launcher {

class JobControl {
public:
    void SetTotal(std::uint64_t total) { total_ = total; }
    void Advance(std::uint64_t amount) { done_ += amount; }
    void Cancel() { cancelled_ = true; }

    std::uint64_t Total() const { return total_; }
    std::uint64_t Done() const { return done_; }
    bool Cancelled() const { return cancelled_; }

private:
    std::atomic<std::uint64_t> total_ = 0;
    std::atomic<std::uint64_t> done_ = 0;
    std::atomic<bool> cancelled_ = false;
};

class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("cancelled") {}
};

inline void ThrowIfCancelled(const JobControl& control) {
    if (control.Cancelled()) throw Cancelled();
}

}
