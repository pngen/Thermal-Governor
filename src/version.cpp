// Thermal Governor — build identity.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/version.hpp"

namespace thermal_governor {

std::string build_identity() {
    std::string out = "thermal-governor/";
    out += version_string();
    return out;
}

}  // namespace thermal_governor
