// Thermal Governor — capability set operations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/capability.hpp"

namespace thermal_governor {

std::string CapabilitySet::render() const {
    std::string out;
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const auto capability = static_cast<ThermalCapability>(i);
        if (!out.empty()) {
            out += "\n";
        }
        out += capability_name(capability);
        out += ": ";
        out += to_string(states_[i]);
        if (!details_[i].empty()) {
            out += " (";
            out += details_[i];
            out += ")";
        }
    }
    return out;
}

}  // namespace thermal_governor
