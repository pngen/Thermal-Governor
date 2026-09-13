// Thermal Governor — internal deterministic formatting helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "format.hpp"

#include <cmath>
#include <cstdio>

namespace thermal_governor::detail {
namespace {

/// Round-half-away-from-zero to a fixed number of decimals, then trim.
std::string fixed(double value, int decimals) {
    if (!std::isfinite(value)) {
        return value != value ? "nan" : (value > 0 ? "inf" : "-inf");
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text(buffer);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (text == "-0") {
        text = "0";
    }
    return text;
}

}  // namespace

std::string number(double value) {
    if (!std::isfinite(value)) {
        return fixed(value, 0);
    }
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) < 1e-9 && std::fabs(rounded) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.0f", rounded);
        return std::string(buffer);
    }
    return fixed(value, 3);
}

std::string temperature(double value) { return number(value); }

std::string signed_number(double value) {
    const std::string magnitude = number(value);
    if (!magnitude.empty() && magnitude[0] != '-') {
        return "+" + magnitude;
    }
    return magnitude;
}

std::string percent(double value) { return number(value); }

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out.append(separator);
        }
        out.append(parts[i]);
    }
    return out;
}

std::string unsigned_decimal(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return std::string(buffer);
}

std::string millis(std::int64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    return std::string(buffer);
}

}  // namespace thermal_governor::detail
