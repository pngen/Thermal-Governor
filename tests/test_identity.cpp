// Thermal Governor — strongly typed identity tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/identity.hpp"

using thermal_governor::CoordinatorEpoch;
using thermal_governor::CoordinatorId;
using thermal_governor::DeviceGeneration;
using thermal_governor::DeviceId;
using thermal_governor::GenerationCounter;
using thermal_governor::Label;
using thermal_governor::StrongId;
using thermal_governor::ThermalDomainId;
using thermal_governor::ThermalDomainGeneration;
using thermal_governor::ThermalPolicyId;
using thermal_governor::WorkerId;

// Compile-time proof that distinct identity families never interconvert.
static_assert(!std::is_convertible_v<DeviceId, ThermalDomainId>);
static_assert(!std::is_convertible_v<ThermalDomainId, DeviceId>);
static_assert(!std::is_convertible_v<DeviceId, WorkerId>);
static_assert(!std::is_convertible_v<WorkerId, CoordinatorId>);
static_assert(!std::is_convertible_v<CoordinatorId, CoordinatorEpoch>);
static_assert(!std::is_convertible_v<DeviceId, DeviceGeneration>);
static_assert(!std::is_convertible_v<ThermalDomainId, ThermalDomainGeneration>);
static_assert(!std::is_convertible_v<ThermalPolicyId, ThermalDomainId>);
static_assert(!std::is_convertible_v<CoordinatorEpoch, ThermalDomainGeneration>);
static_assert(!std::is_convertible_v<std::uint64_t, DeviceId>);
static_assert(!std::is_convertible_v<DeviceId, std::uint64_t>);
static_assert(!std::is_constructible_v<DeviceId, ThermalDomainId>);
static_assert(!std::is_assignable_v<DeviceId&, ThermalDomainId>);
static_assert(!std::is_assignable_v<ThermalDomainId&, DeviceId>);
static_assert(std::is_convertible_v<DeviceId, DeviceId>);
static_assert(std::is_trivially_copyable_v<DeviceId>);

namespace {

[[nodiscard]] DeviceId make_device(std::uint64_t value) {
    return DeviceId{StrongId<thermal_governor::DeviceIdTag>{value}};
}

[[nodiscard]] ThermalDomainId make_domain(std::uint64_t value) {
    return ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{value}};
}

[[nodiscard]] DeviceGeneration make_device_generation(std::uint64_t value) {
    return DeviceGeneration{StrongId<thermal_governor::DeviceGenerationTag>{value}};
}

[[nodiscard]] CoordinatorEpoch make_epoch(std::uint64_t value) {
    return CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{value}};
}

}  // namespace

TG_CASE(identity, default_construction_is_invalid) {
    const DeviceId device;
    TG_CHECK(!device.is_valid());
    TG_CHECK_EQ(device.value(), 0U);

    const ThermalDomainId domain;
    TG_CHECK(!domain.is_valid());
    TG_CHECK(device == DeviceId{});

    TG_CHECK(make_device(1).is_valid());
    TG_CHECK(make_domain(1).is_valid());
}

TG_CASE(identity, explicit_construction_carries_the_value) {
    const DeviceId device = make_device(42);
    TG_CHECK_EQ(device.value(), 42U);
    TG_CHECK(device.is_valid());
    TG_CHECK(device == make_device(42));
    TG_CHECK(device != make_device(43));

    const DeviceGeneration generation = make_device_generation(7);
    TG_CHECK_EQ(generation.value(), 7U);
    TG_CHECK(generation.is_valid());
    TG_CHECK(!make_device_generation(0).is_valid());

    using WideId = StrongId<thermal_governor::NodeIdTag, std::uint32_t>;
    const WideId narrow{99U};
    TG_CHECK_EQ(narrow.value(), 99U);
}

TG_CASE(identity, ordering_is_strong_and_total) {
    TG_CHECK(make_device(1) < make_device(2));
    TG_CHECK(make_device(2) > make_device(1));
    TG_CHECK(make_device(3) >= make_device(3));
    TG_CHECK(make_device(3) <= make_device(3));
    TG_CHECK((make_device(1) <=> make_device(2)) < 0);

    std::vector<DeviceId> devices{make_device(9), make_device(2), make_device(5), make_device(2)};
    std::sort(devices.begin(), devices.end());
    TG_CHECK_EQ(devices.front().value(), 2U);
    TG_CHECK_EQ(devices.back().value(), 9U);
    for (std::size_t i = 1; i < devices.size(); ++i) {
        TG_CHECK(devices[i - 1] <= devices[i]);
    }

    std::vector<ThermalDomainId> domains{make_domain(4), make_domain(1)};
    std::sort(domains.begin(), domains.end());
    TG_CHECK(domains.front() == make_domain(1));
    TG_CHECK(domains.back() == make_domain(4));
}

TG_CASE(identity, hashing_supports_unordered_containers) {
    std::unordered_map<DeviceId, std::string> names;
    names.emplace(make_device(1), "one");
    names.emplace(make_device(2), "two");
    names.emplace(make_device(3), "three");
    TG_CHECK_EQ(names.size(), static_cast<std::size_t>(3));
    TG_CHECK_EQ(names.at(make_device(2)), std::string("two"));
    TG_CHECK(names.find(make_device(9)) == names.end());
    names[make_device(1)] = "uno";
    TG_CHECK_EQ(names.at(make_device(1)), std::string("uno"));
    TG_CHECK_EQ(names.size(), static_cast<std::size_t>(3));

    std::unordered_set<ThermalDomainId> domains;
    domains.insert(make_domain(7));
    domains.insert(make_domain(7));
    domains.insert(make_domain(8));
    TG_CHECK_EQ(domains.size(), static_cast<std::size_t>(2));
    TG_CHECK(domains.contains(make_domain(7)));
    TG_CHECK(!domains.contains(make_domain(9)));

    TG_CHECK_EQ(std::hash<DeviceId>{}(make_device(11)), std::hash<std::uint64_t>{}(11));
    TG_CHECK_EQ(std::hash<ThermalDomainId>{}(make_domain(11)), std::hash<std::uint64_t>{}(11));
}

TG_CASE(identity, generation_counter_advance_is_monotonic) {
    GenerationCounter<DeviceGeneration> counter{make_device_generation(5)};
    TG_CHECK_EQ(counter.current().value(), 5U);

    DeviceGeneration previous = counter.current();
    for (std::uint64_t step = 1; step <= 4; ++step) {
        const DeviceGeneration next = counter.advance();
        TG_CHECK_EQ(next.value(), 5U + step);
        TG_CHECK_EQ(counter.current().value(), 5U + step);
        TG_CHECK(next > previous);
        previous = next;
    }

    GenerationCounter<CoordinatorEpoch> epochs;
    TG_CHECK(!epochs.current().is_valid());
    TG_CHECK_EQ(epochs.advance().value(), 1U);
    TG_CHECK_EQ(epochs.advance().value(), 2U);
}

TG_CASE(identity, generation_counter_observe_only_moves_forward) {
    GenerationCounter<CoordinatorEpoch> counter{make_epoch(10)};

    TG_PHASE("equal and older observations are refused");
    TG_CHECK(!counter.observe(make_epoch(10)));
    TG_CHECK(!counter.observe(make_epoch(3)));
    TG_CHECK(!counter.observe(CoordinatorEpoch{}));
    TG_CHECK_EQ(counter.current().value(), 10U);

    TG_PHASE("strictly newer observations are adopted");
    TG_CHECK(counter.observe(make_epoch(11)));
    TG_CHECK_EQ(counter.current().value(), 11U);
    TG_CHECK(counter.observe(make_epoch(4096)));
    TG_CHECK_EQ(counter.current().value(), 4096U);

    TG_PHASE("an adopted generation is never lost");
    TG_CHECK(!counter.observe(make_epoch(4095)));
    TG_CHECK(!counter.observe(make_epoch(11)));
    TG_CHECK_EQ(counter.current().value(), 4096U);
}

TG_CASE(identity, label_comparison_is_stable) {
    const Label empty;
    TG_CHECK(empty.empty());
    TG_CHECK(empty.view().empty());
    TG_CHECK_EQ(empty.text(), std::string());

    const Label first{"gpu-0"};
    const Label second{"gpu-1"};
    const Label copy{"gpu-0"};
    TG_CHECK(!first.empty());
    TG_CHECK_EQ(first.text(), std::string("gpu-0"));
    TG_CHECK_EQ(first.view(), std::string_view("gpu-0"));
    TG_CHECK(first == copy);
    TG_CHECK(first != second);
    TG_CHECK(first < second);
    TG_CHECK(second > first);

    std::vector<Label> labels{second, empty, first};
    std::sort(labels.begin(), labels.end());
    TG_CHECK(labels.front() == empty);
    TG_CHECK(labels.back() == second);
    TG_CHECK(labels[1] == first);
}
