// Thermal Governor — explicit thermal quantity types.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_QUANTITY_HPP
#define THERMAL_GOVERNOR_QUANTITY_HPP

#include <cmath>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

#include "thermal_governor/error.hpp"

namespace thermal_governor {

/// A signed temperature difference. Distinct from an absolute temperature so
/// that "add two absolute temperatures" is a compile error.
class TemperatureDelta {
public:
    constexpr TemperatureDelta() noexcept = default;
    constexpr explicit TemperatureDelta(double kelvin_equivalent) noexcept
        : value_(kelvin_equivalent) {}

    [[nodiscard]] static std::optional<TemperatureDelta> try_from(double value) noexcept {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
        if (value < -2000.0 || value > 2000.0) {
            return std::nullopt;
        }
        return TemperatureDelta{value};
    }

    [[nodiscard]] constexpr double value() const noexcept { return value_; }
    [[nodiscard]] constexpr TemperatureDelta operator-() const noexcept {
        return TemperatureDelta{-value_};
    }
    [[nodiscard]] constexpr TemperatureDelta operator+(TemperatureDelta o) const noexcept {
        return TemperatureDelta{value_ + o.value_};
    }

    friend constexpr bool operator==(TemperatureDelta, TemperatureDelta) noexcept = default;
    friend constexpr std::partial_ordering operator<=>(
        TemperatureDelta, TemperatureDelta) noexcept = default;

private:
    double value_ = 0.0;
};

/// An absolute temperature expressed in degrees Celsius.
///
/// Construction is validated: NaN, infinities and physically implausible
/// magnitudes are rejected rather than propagated.
class DegreesCelsius {
public:
    /// Absolute zero, inclusive lower bound of representable readings.
    static constexpr double kAbsoluteZero = -273.15;
    /// Upper plausibility bound for a thermal sensor reading.
    static constexpr double kMaxPlausible = 1500.0;

    constexpr DegreesCelsius() noexcept = default;

    /// Construct without validation. Intended for policy literals and for
    /// values that have already been validated. Untrusted input must go
    /// through make() or try_from().
    constexpr explicit DegreesCelsius(double value) noexcept : value_(value) {}

    [[nodiscard]] static std::optional<DegreesCelsius> try_from(double value) noexcept {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
        if (value < kAbsoluteZero || value > kMaxPlausible) {
            return std::nullopt;
        }
        return DegreesCelsius{value};
    }

    [[nodiscard]] static Result<DegreesCelsius> make(double value) {
        auto parsed = try_from(value);
        if (!parsed.has_value()) {
            return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                                "temperature outside representable range"};
        }
        return *parsed;
    }

    [[nodiscard]] constexpr double value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value_ >= kAbsoluteZero && value_ <= kMaxPlausible;
    }

    [[nodiscard]] constexpr DegreesCelsius operator+(TemperatureDelta d) const noexcept {
        return DegreesCelsius{value_ + d.value()};
    }
    [[nodiscard]] constexpr DegreesCelsius operator-(TemperatureDelta d) const noexcept {
        return DegreesCelsius{value_ - d.value()};
    }
    [[nodiscard]] constexpr TemperatureDelta operator-(DegreesCelsius o) const noexcept {
        return TemperatureDelta{value_ - o.value()};
    }

    friend constexpr bool operator==(DegreesCelsius, DegreesCelsius) noexcept = default;
    friend constexpr std::partial_ordering operator<=>(
        DegreesCelsius, DegreesCelsius) noexcept = default;

    /// Deterministic fixed-point rendering used by explanations and CLI.
    [[nodiscard]] std::string render() const;

private:
    double value_ = 0.0;
};

/// A percentage in the closed interval [0, 100] used for concurrency and
/// admission ceilings.
class Percent {
public:
    constexpr Percent() noexcept = default;
    constexpr explicit Percent(double value) noexcept : value_(value) {}

    [[nodiscard]] static std::optional<Percent> try_from(double value) noexcept {
        if (!std::isfinite(value) || value < 0.0 || value > 100.0) {
            return std::nullopt;
        }
        return Percent{value};
    }

    [[nodiscard]] constexpr double value() const noexcept { return value_; }

    friend constexpr bool operator==(Percent, Percent) noexcept = default;
    friend constexpr std::partial_ordering operator<=>(Percent, Percent) noexcept = default;

private:
    double value_ = 0.0;
};

/// A clock frequency in megahertz.
class MegaHertz {
public:
    constexpr MegaHertz() noexcept = default;
    constexpr explicit MegaHertz(std::uint32_t value) noexcept : value_(value) {}

    [[nodiscard]] static std::optional<MegaHertz> try_from(std::uint64_t value) noexcept {
        if (value > 1'000'000ULL) {
            return std::nullopt;
        }
        return MegaHertz{static_cast<std::uint32_t>(value)};
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

    friend constexpr bool operator==(MegaHertz, MegaHertz) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(MegaHertz, MegaHertz) noexcept = default;

private:
    std::uint32_t value_ = 0;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_QUANTITY_HPP
