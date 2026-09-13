// Thermal Governor — coupling graph tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/coupling.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/provenance.hpp"

using thermal_governor::CouplingGeneration;
using thermal_governor::CouplingGraph;
using thermal_governor::CouplingId;
using thermal_governor::CouplingRelation;
using thermal_governor::CouplingType;
using thermal_governor::Provenance;
using thermal_governor::StrongId;
using thermal_governor::ThermalDomainId;

namespace {

[[nodiscard]] ThermalDomainId make_domain(std::uint64_t value) {
    return ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{value}};
}

[[nodiscard]] CouplingRelation make_edge(std::uint64_t id, std::uint64_t source,
                                         std::uint64_t destination) {
    CouplingRelation relation;
    relation.id = CouplingId{StrongId<thermal_governor::CouplingIdTag>{id}};
    relation.source = make_domain(source);
    relation.destination = make_domain(destination);
    relation.type = CouplingType::THERMALLY_COUPLED;
    relation.generation = CouplingGeneration{StrongId<thermal_governor::CouplingGenerationTag>{1}};
    relation.provenance = Provenance::REAL;
    relation.evidence_source = "fixture";
    relation.weight = 1.0;
    return relation;
}

/// Render edges as source>destination#id so ordering is directly comparable.
[[nodiscard]] std::string describe_edges(const CouplingGraph& graph) {
    std::string out;
    for (const auto& edge : graph.edges()) {
        out += std::to_string(edge.source.value());
        out += ">";
        out += std::to_string(edge.destination.value());
        out += "#";
        out += std::to_string(edge.id.value());
        out += " ";
    }
    return out;
}

/// Render reachable domains as domain:depth so ordering is directly comparable.
[[nodiscard]] std::string describe_reach(
    const std::vector<std::pair<ThermalDomainId, std::uint32_t>>& reached) {
    std::string out;
    for (const auto& entry : reached) {
        out += std::to_string(entry.first.value());
        out += ":";
        out += std::to_string(entry.second);
        out += " ";
    }
    return out;
}

}  // namespace

TG_CASE(coupling, add_keeps_edges_sorted) {
    CouplingGraph graph;
    graph.add(make_edge(1, 5, 6));
    graph.add(make_edge(2, 1, 4));
    graph.add(make_edge(3, 1, 2));
    graph.add(make_edge(0, 1, 2));

    TG_CHECK_EQ(graph.size(), std::size_t{4});
    TG_CHECK_EQ(describe_edges(graph), std::string("1>2#0 1>2#3 1>4#2 5>6#1 "));

    TG_PHASE("every prefix stays ordered");
    CouplingGraph incremental;
    const std::uint64_t sources[] = {9, 3, 7, 1, 5};
    for (std::size_t index = 0; index < 5; ++index) {
        incremental.add(make_edge(index + 1, sources[index], sources[index] + 1));
        const auto& edges = incremental.edges();
        for (std::size_t edge = 1; edge < edges.size(); ++edge) {
            TG_CHECK(edges[edge - 1].source <= edges[edge].source);
            if (edges[edge - 1].source == edges[edge].source) {
                TG_CHECK(edges[edge - 1].destination <= edges[edge].destination);
            }
        }
    }
    TG_CHECK_EQ(incremental.size(), std::size_t{5});
}

TG_CASE(coupling, add_replaces_on_duplicate_id) {
    CouplingGraph graph;
    graph.add(make_edge(1, 1, 2));
    graph.add(make_edge(2, 3, 4));
    TG_CHECK_EQ(graph.size(), std::size_t{2});

    CouplingRelation replacement = make_edge(1, 1, 2);
    replacement.weight = 0.25;
    replacement.type = CouplingType::SHARES_CHASSIS;
    replacement.evidence_source = "replaced";
    graph.add(replacement);

    TG_CHECK_EQ(graph.size(), std::size_t{2});
    const auto& edges = graph.edges();
    TG_CHECK_EQ(edges[0].id.value(), 1U);
    TG_CHECK_EQ(edges[0].weight, 0.25);
    TG_CHECK(edges[0].type == CouplingType::SHARES_CHASSIS);
    TG_CHECK_EQ(edges[0].evidence_source, std::string("replaced"));

    TG_PHASE("a replacement never grows the graph");
    graph.add(replacement);
    graph.add(replacement);
    TG_CHECK_EQ(graph.size(), std::size_t{2});
}

TG_CASE(coupling, add_restores_order_after_a_key_changing_replacement) {
    CouplingGraph graph;
    graph.add(make_edge(1, 1, 2));
    graph.add(make_edge(2, 3, 4));
    TG_CHECK_EQ(describe_edges(graph), std::string("1>2#1 3>4#2 "));

    graph.add(make_edge(1, 7, 8));
    TG_CHECK_EQ(graph.size(), std::size_t{2});
    TG_CHECK_EQ(describe_edges(graph), std::string("3>4#2 7>8#1 "));

    const auto& edges = graph.edges();
    TG_CHECK(edges[0].source < edges[1].source);
}

TG_CASE(coupling, remove_reports_accurately) {
    CouplingGraph graph;
    graph.add(make_edge(1, 1, 2));
    graph.add(make_edge(2, 2, 3));
    graph.add(make_edge(3, 3, 4));

    TG_CHECK(graph.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{2}}));
    TG_CHECK_EQ(graph.size(), std::size_t{2});
    TG_CHECK_EQ(describe_edges(graph), std::string("1>2#1 3>4#3 "));

    TG_PHASE("removing an unknown edge reports false and changes nothing");
    TG_CHECK(!graph.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{2}}));
    TG_CHECK(!graph.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{99}}));
    TG_CHECK_EQ(graph.size(), std::size_t{2});

    TG_CHECK(graph.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{1}}));
    TG_CHECK(graph.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{3}}));
    TG_CHECK_EQ(graph.size(), std::size_t{0});
    TG_CHECK(!graph.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{1}}));

    TG_PHASE("a removed id may be re-added");
    graph.add(make_edge(1, 1, 2));
    TG_CHECK_EQ(graph.size(), std::size_t{1});
}

TG_CASE(coupling, reachable_from_is_ordered_by_depth_then_id) {
    CouplingGraph graph;
    graph.add(make_edge(1, 1, 5));
    graph.add(make_edge(2, 1, 2));
    graph.add(make_edge(3, 2, 3));
    graph.add(make_edge(4, 5, 3));
    graph.add(make_edge(5, 3, 9));

    const auto reached = graph.reachable_from(make_domain(1), 3);
    TG_CHECK_EQ(describe_reach(reached), std::string("2:1 5:1 3:2 9:3 "));

    TG_PHASE("depth limits are respected exactly");
    TG_CHECK_EQ(describe_reach(graph.reachable_from(make_domain(1), 1)), std::string("2:1 5:1 "));
    TG_CHECK_EQ(describe_reach(graph.reachable_from(make_domain(1), 2)), std::string("2:1 5:1 3:2 "));
    TG_CHECK(graph.reachable_from(make_domain(1), 0).empty());

    TG_PHASE("the origin is never reported as its own neighbour");
    const auto from_three = graph.reachable_from(make_domain(3), 4);
    TG_CHECK_EQ(describe_reach(from_three), std::string("9:1 "));
    TG_CHECK(graph.reachable_from(make_domain(42), 4).empty());

    TG_PHASE("numbered domains sort by id, not by discovery order");
    CouplingGraph wide;
    wide.add(make_edge(1, 1, 30));
    wide.add(make_edge(2, 1, 20));
    wide.add(make_edge(3, 1, 10));
    TG_CHECK_EQ(describe_reach(wide.reachable_from(make_domain(1), 1)),
                std::string("10:1 20:1 30:1 "));
}

TG_CASE(coupling, reachable_from_is_independent_of_insertion_order) {
    CouplingGraph forward;
    forward.add(make_edge(1, 1, 2));
    forward.add(make_edge(2, 1, 3));
    forward.add(make_edge(3, 2, 4));
    forward.add(make_edge(4, 3, 4));
    forward.add(make_edge(5, 4, 5));

    CouplingGraph reversed;
    reversed.add(make_edge(5, 4, 5));
    reversed.add(make_edge(4, 3, 4));
    reversed.add(make_edge(3, 2, 4));
    reversed.add(make_edge(2, 1, 3));
    reversed.add(make_edge(1, 1, 2));

    const std::string expected = "2:1 3:1 4:2 5:3 ";
    TG_CHECK_EQ(describe_reach(forward.reachable_from(make_domain(1), 10)), expected);
    TG_CHECK_EQ(describe_reach(reversed.reachable_from(make_domain(1), 10)), expected);
    TG_CHECK_EQ(describe_edges(forward), describe_edges(reversed));
}

TG_CASE(coupling, reachable_from_terminates_on_cycles) {
    CouplingGraph graph;
    graph.add(make_edge(1, 1, 2));
    graph.add(make_edge(2, 2, 3));
    graph.add(make_edge(3, 3, 1));
    graph.add(make_edge(4, 3, 4));
    graph.add(make_edge(5, 4, 2));

    const auto reached = graph.reachable_from(make_domain(1), 16);
    TG_CHECK_EQ(describe_reach(reached), std::string("2:1 3:2 4:3 "));

    TG_PHASE("a self loop terminates too");
    CouplingGraph loop;
    loop.add(make_edge(1, 6, 6));
    loop.add(make_edge(2, 6, 7));
    TG_CHECK_EQ(describe_reach(loop.reachable_from(make_domain(6), 8)), std::string("7:1 "));
}

TG_CASE(coupling, has_cycle_detects_cycles_and_accepts_dags) {
    TG_PHASE("three domain cycle");
    CouplingGraph cycle;
    cycle.add(make_edge(1, 1, 2));
    cycle.add(make_edge(2, 2, 3));
    cycle.add(make_edge(3, 3, 1));
    TG_CHECK(cycle.has_cycle());

    TG_PHASE("diamond shaped DAG");
    CouplingGraph dag;
    dag.add(make_edge(1, 1, 2));
    dag.add(make_edge(2, 1, 3));
    dag.add(make_edge(3, 2, 4));
    dag.add(make_edge(4, 3, 4));
    TG_CHECK(!dag.has_cycle());

    TG_PHASE("self loop and empty graph");
    CouplingGraph loop;
    loop.add(make_edge(1, 5, 5));
    TG_CHECK(loop.has_cycle());

    const CouplingGraph empty;
    TG_CHECK(!empty.has_cycle());

    TG_PHASE("removing the closing edge removes the cycle");
    TG_CHECK(cycle.remove(CouplingId{StrongId<thermal_governor::CouplingIdTag>{3}}));
    TG_CHECK(!cycle.has_cycle());
}

TG_CASE(coupling, deep_chain_does_not_overflow_the_stack) {
    constexpr std::uint64_t kDepth = 5000;

    CouplingGraph chain;
    for (std::uint64_t index = 1; index <= kDepth; ++index) {
        chain.add(make_edge(index, index, index + 1));
    }
    TG_CHECK_EQ(chain.size(), static_cast<std::size_t>(kDepth));

    TG_PHASE("cycle detection is iterative");
    TG_CHECK(!chain.has_cycle());

    TG_PHASE("breadth first traversal reaches the far end");
    const auto reached = chain.reachable_from(make_domain(1), static_cast<std::uint32_t>(kDepth));
    TG_CHECK_EQ(reached.size(), static_cast<std::size_t>(kDepth));
    TG_CHECK_EQ(reached.front().first.value(), 2U);
    TG_CHECK_EQ(reached.front().second, 1U);
    TG_CHECK_EQ(reached.back().first.value(), kDepth + 1);
    TG_CHECK_EQ(reached.back().second, static_cast<std::uint32_t>(kDepth));

    TG_PHASE("the chain becomes cyclic when the far end is joined back");
    chain.add(make_edge(kDepth + 1, kDepth + 1, 1));
    TG_CHECK(chain.has_cycle());
}
