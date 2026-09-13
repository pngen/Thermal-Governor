// Thermal Governor — internal deterministic formatting helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_SRC_FORMAT_HPP
#define THERMAL_GOVERNOR_SRC_FORMAT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace thermal_governor::detail {

/// Render a double deterministically: integers without a decimal point,
/// otherwise a trimmed fixed-point representation with at most three
/// fractional digits. Locale independent.
[[nodiscard]] std::string number(double value);

/// Render a temperature in degrees Celsius without the unit suffix.
[[nodiscard]] std::string temperature(double value);

/// Render a signed temperature delta, always carrying an explicit sign for
/// positive values.
[[nodiscard]] std::string signed_number(double value);

/// Render a percentage.
[[nodiscard]] std::string percent(double value);

/// Join strings with a separator.
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);

/// Render a generation or identity value as an unsigned decimal.
[[nodiscard]] std::string unsigned_decimal(std::uint64_t value);

/// Render a duration in milliseconds.
[[nodiscard]] std::string millis(std::int64_t value);

}  // namespace thermal_governor::detail

#endif  // THERMAL_GOVERNOR_SRC_FORMAT_HPP
