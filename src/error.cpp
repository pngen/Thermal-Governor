// Thermal Governor — error rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/error.hpp"

namespace thermal_governor {

std::string ThermalError::render() const {
    std::string out(to_string(code));
    if (!detail.empty()) {
        out += ": ";
        out += detail;
    }
    return out;
}

}  // namespace thermal_governor
