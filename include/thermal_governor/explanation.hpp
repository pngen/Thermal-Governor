// Thermal Governor — deterministic structured explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_EXPLANATION_HPP
#define THERMAL_GOVERNOR_EXPLANATION_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "thermal_governor/admission.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/envelope.hpp"
#include "thermal_governor/evaluation.hpp"
#include "thermal_governor/recovery.hpp"

namespace thermal_governor {

/// A single key/value line inside an explanation section.
struct ExplanationLine {
    std::string key;
    std::string value;

    friend bool operator==(const ExplanationLine&, const ExplanationLine&) = default;
};

/// A titled section of an explanation.
struct ExplanationSection {
    std::string title;
    std::vector<ExplanationLine> lines;
    /// Free-form stable bullets, used by the "Rejected" section.
    std::vector<std::string> bullets;

    friend bool operator==(const ExplanationSection&, const ExplanationSection&) = default;
};

/// A structured, canonically-ordered explanation.
///
/// Ordering is fixed by construction: sections are appended in a documented
/// order and lines within a section are appended deterministically. Two
/// evaluations over equal inputs therefore render byte-identical text.
class Explanation {
public:
    static constexpr std::size_t kMaxRenderedBytes = 256 * 1024;

    void add_section(ExplanationSection section) {
        sections_.push_back(std::move(section));
    }

    [[nodiscard]] const std::vector<ExplanationSection>& sections() const noexcept {
        return sections_;
    }

    [[nodiscard]] std::string render() const;

    friend bool operator==(const Explanation&, const Explanation&) = default;

private:
    std::vector<ExplanationSection> sections_;
};

/// Build the canonical explanation for a thermal evaluation.
[[nodiscard]] Explanation build_explanation(const ThermalEvaluation& evaluation,
                                            const ThermalEnvelope& envelope,
                                            const ThermalPolicy& policy);

/// Build the canonical explanation for a thermal admission result.
[[nodiscard]] Explanation build_admission_explanation(const ThermalAdmissionResult& result,
                                                      const ThermalPolicy& policy);

/// Build the canonical explanation for a recovery assessment.
[[nodiscard]] Explanation build_recovery_explanation(const RecoveryAssessment& assessment,
                                                      const ThermalPolicy& policy);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_EXPLANATION_HPP
