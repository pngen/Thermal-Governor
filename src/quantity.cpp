// Thermal Governor — quantity rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/quantity.hpp"

#include "format.hpp"

namespace thermal_governor {

std::string DegreesCelsius::render() const { return detail::number(value_); }

}  // namespace thermal_governor
