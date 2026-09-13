// Thermal Governor — injectable clock abstraction.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_TIME_HPP
#define THERMAL_GOVERNOR_TIME_HPP

#include <chrono>
#include <cstdint>
#include <memory>

namespace thermal_governor {

using SteadyTimePoint = std::chrono::steady_clock::time_point;
using WallTimePoint = std::chrono::system_clock::time_point;
using Milliseconds = std::chrono::milliseconds;

/// Clock source used for evidence freshness, hysteresis spans and
/// duration-based stability. Injectable so that every duration-dependent
/// policy path is deterministically testable without sleeps.
class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual SteadyTimePoint now() const noexcept = 0;
    [[nodiscard]] virtual WallTimePoint wall_now() const noexcept = 0;
};

/// Production clock backed by std::chrono steady/system clocks.
class SystemClock final : public IClock {
public:
    [[nodiscard]] SteadyTimePoint now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] static std::shared_ptr<const IClock> shared();
};

/// Manually advanced clock for deterministic tests.
class ManualClock final : public IClock {
public:
    explicit ManualClock(SteadyTimePoint start = SteadyTimePoint{}) : now_(start) {}

    [[nodiscard]] SteadyTimePoint now() const noexcept override { return now_; }
    [[nodiscard]] WallTimePoint wall_now() const noexcept override { return wall_; }

    void advance(Milliseconds delta) noexcept {
        now_ += delta;
        wall_ += delta;
    }

    void set(SteadyTimePoint value) noexcept { now_ = value; }

private:
    SteadyTimePoint now_{};
    WallTimePoint wall_{};
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_TIME_HPP
