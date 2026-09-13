// Thermal Governor — vendor-neutral C++20 thermal governance runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_VERSION_HPP
#define THERMAL_GOVERNOR_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace thermal_governor {

/// Semantic version of the Thermal Governor runtime.
struct Version {
    std::uint32_t major = 1;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    friend constexpr bool operator==(const Version&, const Version&) noexcept = default;
    friend constexpr auto operator<=>(const Version&, const Version&) noexcept = default;
};

inline constexpr Version kVersion{1, 0, 0};

/// On-disk / on-wire format version for the persistence container.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

/// Protocol version for the framed coordinator/worker transport.
inline constexpr std::uint16_t kProtocolVersion = 1;

[[nodiscard]] constexpr std::string_view version_string() noexcept {
    return "1.0.0";
}

/// Human-readable build identity string, e.g. "thermal-governor/1.0.0".
[[nodiscard]] std::string build_identity();

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_VERSION_HPP
