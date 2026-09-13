// Thermal Governor — coupling graph.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/coupling.hpp"

#include <algorithm>
#include <map>
#include <queue>
#include <set>

namespace thermal_governor {

void CouplingGraph::add(CouplingRelation relation) {
    bool replaced = false;
    for (auto& existing : edges_) {
        if (existing.id == relation.id) {
            existing = std::move(relation);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        edges_.push_back(std::move(relation));
    }
    // Ordering is re-established after every insertion and replacement, so a
    // replacement that changes the source or destination cannot break the
    // (source, destination, id) invariant.
    std::sort(edges_.begin(), edges_.end(), [](const CouplingRelation& a, const CouplingRelation& b) {
        if (a.source != b.source) {
            return a.source < b.source;
        }
        if (a.destination != b.destination) {
            return a.destination < b.destination;
        }
        return a.id < b.id;
    });
}

bool CouplingGraph::remove(CouplingId id) {
    const auto it = std::find_if(edges_.begin(), edges_.end(),
                                 [id](const CouplingRelation& e) { return e.id == id; });
    if (it == edges_.end()) {
        return false;
    }
    edges_.erase(it);
    return true;
}

std::vector<std::pair<ThermalDomainId, std::uint32_t>> CouplingGraph::reachable_from(
    ThermalDomainId origin, std::uint32_t max_depth) const {
    std::vector<std::pair<ThermalDomainId, std::uint32_t>> out;
    if (max_depth == 0) {
        return out;
    }

    std::map<ThermalDomainId, std::uint32_t> best;
    std::queue<std::pair<ThermalDomainId, std::uint32_t>> frontier;
    frontier.emplace(origin, 0);
    best.emplace(origin, 0);

    while (!frontier.empty()) {
        const auto [current, depth] = frontier.front();
        frontier.pop();
        if (depth >= max_depth) {
            continue;
        }
        for (const auto& edge : edges_) {
            if (!(edge.source == current)) {
                continue;
            }
            const std::uint32_t next_depth = depth + 1;
            const auto it = best.find(edge.destination);
            if (it != best.end() && it->second <= next_depth) {
                continue;
            }
            best[edge.destination] = next_depth;
            frontier.emplace(edge.destination, next_depth);
        }
    }

    for (const auto& [domain, depth] : best) {
        if (domain == origin) {
            continue;
        }
        out.emplace_back(domain, depth);
    }
    // Ordered by (depth, domain id) so results do not depend on insertion
    // order or on traversal order.
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second < b.second;
        }
        return a.first < b.first;
    });
    return out;
}

std::vector<std::pair<ThermalDomainId, std::uint32_t>> CouplingGraph::incoming_reachable(
    ThermalDomainId origin, std::uint32_t max_depth) const {
    std::vector<std::pair<ThermalDomainId, std::uint32_t>> out;
    if (max_depth == 0) {
        return out;
    }

    std::map<ThermalDomainId, std::uint32_t> best;
    std::queue<std::pair<ThermalDomainId, std::uint32_t>> frontier;
    frontier.emplace(origin, 0);
    best.emplace(origin, 0);

    while (!frontier.empty()) {
        const auto [current, depth] = frontier.front();
        frontier.pop();
        if (depth >= max_depth) {
            continue;
        }
        for (const auto& edge : edges_) {
            if (!(edge.destination == current)) {
                continue;
            }
            const std::uint32_t next_depth = depth + 1;
            const auto it = best.find(edge.source);
            if (it != best.end() && it->second <= next_depth) {
                continue;
            }
            best[edge.source] = next_depth;
            frontier.emplace(edge.source, next_depth);
        }
    }

    for (const auto& [domain, depth] : best) {
        if (domain == origin) {
            continue;
        }
        out.emplace_back(domain, depth);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second < b.second;
        }
        return a.first < b.first;
    });
    return out;
}

bool CouplingGraph::has_cycle() const {
    std::map<ThermalDomainId, std::vector<ThermalDomainId>> adjacency;
    std::set<ThermalDomainId> nodes;
    for (const auto& edge : edges_) {
        adjacency[edge.source].push_back(edge.destination);
        nodes.insert(edge.source);
        nodes.insert(edge.destination);
    }

    enum class Mark { White, Grey, Black };
    std::map<ThermalDomainId, Mark> marks;
    for (const auto& node : nodes) {
        marks[node] = Mark::White;
    }

    // Iterative depth-first search: no recursion, so a deep coupling graph
    // cannot overflow the stack.
    for (const auto& start : nodes) {
        if (marks[start] != Mark::White) {
            continue;
        }
        std::vector<std::pair<ThermalDomainId, std::size_t>> stack;
        stack.emplace_back(start, 0);
        marks[start] = Mark::Grey;
        while (!stack.empty()) {
            auto& [node, index] = stack.back();
            const auto it = adjacency.find(node);
            if (it == adjacency.end() || index >= it->second.size()) {
                marks[node] = Mark::Black;
                stack.pop_back();
                continue;
            }
            const ThermalDomainId next = it->second[index];
            ++index;
            const Mark mark = marks[next];
            if (mark == Mark::Grey) {
                return true;
            }
            if (mark == Mark::White) {
                marks[next] = Mark::Grey;
                stack.emplace_back(next, 0);
            }
        }
    }
    return false;
}

}  // namespace thermal_governor
