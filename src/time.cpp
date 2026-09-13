// Thermal Governor — clock implementations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/time.hpp"

namespace thermal_governor {

std::shared_ptr<const IClock> SystemClock::shared() {
    static const std::shared_ptr<const IClock> instance = std::make_shared<const SystemClock>();
    return instance;
}

}  // namespace thermal_governor
