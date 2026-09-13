// Thermal Governor — test framework with stable case identifiers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_TESTS_FRAMEWORK_HPP
#define THERMAL_GOVERNOR_TESTS_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tg {

/// Per-case execution context.
///
/// Progress is printed and flushed immediately so a hanging case is
/// visible rather than buffered away. No case is ever given a timeout:
/// a hang is a defect to diagnose, not to paper over.
class Context {
public:
    /// Announce a phase boundary. The line is printed and flushed
    /// immediately, so a case that blocks is never able to hide where it
    /// blocked behind buffered progress.
    void phase(const char* name);
    void fail(std::string reason);
    void note(std::string text);

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] const char* current_phase() const noexcept { return phase_; }
    void set_id(std::string id) { id_ = std::move(id); }
    [[nodiscard]] const std::string& id() const noexcept { return id_; }

private:
    bool failed_ = false;
    std::string reason_;
    std::string id_;
    const char* phase_ = "RUN";
};

struct TestCase {
    std::string id;
    void (*fn)(Context&) = nullptr;
};

class Registry {
public:
    static Registry& instance();
    void add(std::string id, void (*fn)(Context&));
    [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }

private:
    std::vector<TestCase> cases_;
};

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)(Context&));
};

/// Entry point shared by every suite executable.
///
/// Usage:
///   <exe>                     run every case
///   <exe> --list              print every stable case id
///   <exe> --case <suite::id>  run exactly one case
///   <exe> --filter <prefix>   run every case whose id starts with prefix
int run_all(int argc, char** argv);

}  // namespace tg

#define TG_CASE(suite, name)                                                        \
    static void tg_case_##suite##_##name(::tg::Context& tg_ctx);                    \
    static const ::tg::Registrar tg_registrar_##suite##_##name(                     \
        #suite, #name, &tg_case_##suite##_##name);                                  \
    static void tg_case_##suite##_##name(::tg::Context& tg_ctx)

#define TG_PHASE(name) tg_ctx.phase(name)

#define TG_FAIL(message)                     \
    do {                                     \
        tg_ctx.fail(std::string(message));   \
        return;                              \
    } while (false)

#define TG_CHECK(condition)                                                            \
    do {                                                                               \
        if (!(condition)) {                                                            \
            tg_ctx.fail(std::string("check failed: ") + #condition + " at " +          \
                        __FILE__ + ":" + std::to_string(__LINE__));                    \
            return;                                                                    \
        }                                                                              \
    } while (false)

#define TG_EXPECT(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            tg_ctx.fail(std::string("expect failed: ") + #condition + " at " +         \
                        __FILE__ + ":" + std::to_string(__LINE__));                    \
        }                                                                              \
    } while (false)

// The operands are copied rather than bound by reference. Binding a
// reference to the result of a function call does not extend the lifetime
// of the temporary that produced it, so a reference into it dangles the
// moment the statement ends.
#define TG_CHECK_EQ(actual, expected)                                                  \
    do {                                                                               \
        const auto tg_a = (actual);                                                   \
        const auto tg_b = (expected);                                                 \
        if (!(tg_a == tg_b)) {                                                         \
            tg_ctx.fail(std::string("equality failed: ") + #actual + " == " +          \
                        #expected + " at " + __FILE__ + ":" +                          \
                        std::to_string(__LINE__));                                     \
            return;                                                                    \
        }                                                                              \
    } while (false)

#define TG_OK(expression)                                                              \
    do {                                                                               \
        auto tg_result = (expression);                                                 \
        if (!tg_result.has_value()) {                                                  \
            tg_ctx.fail(std::string("unexpected error: ") + #expression + " -> " +     \
                        std::string(::thermal_governor::to_string(tg_result.error().code)) + " " +  \
                        tg_result.error().detail + " at " + __FILE__ + ":" +           \
                        std::to_string(__LINE__));                                     \
            return;                                                                    \
        }                                                                              \
    } while (false)

#define TG_STATUS_OK(expression)                                                       \
    do {                                                                               \
        auto tg_status = (expression);                                                 \
        if (!tg_status.ok()) {                                                         \
            tg_ctx.fail(std::string("unexpected status: ") + #expression + " -> " +    \
                        std::string(::thermal_governor::to_string(tg_status.code())) + " " +        \
                        tg_status.error().detail + " at " + __FILE__ + ":" +           \
                        std::to_string(__LINE__));                                     \
            return;                                                                    \
        }                                                                              \
    } while (false)

#define TG_ERROR_CODE(expression, expected_code)                                       \
    do {                                                                               \
        auto tg_result = (expression);                                                 \
        if (tg_result.has_value()) {                                                   \
            tg_ctx.fail(std::string("expected failure but succeeded: ") + #expression + \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));           \
            return;                                                                    \
        }                                                                              \
        if (tg_result.error().code != (expected_code)) {                               \
            tg_ctx.fail(std::string("wrong error code for ") + #expression + ": got " + \
                        std::string(::thermal_governor::to_string(tg_result.error().code)) +        \
                        " want " + std::string(::thermal_governor::to_string(expected_code)) +      \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));           \
            return;                                                                    \
        }                                                                              \
    } while (false)

#define TG_STATUS_ERROR_CODE(expression, expected_code)                                \
    do {                                                                               \
        auto tg_status = (expression);                                                 \
        if (tg_status.ok()) {                                                          \
            tg_ctx.fail(std::string("expected failure but succeeded: ") + #expression + \
                        " at " + __FILE__ + ":" + std::to_string(__LINE__));           \
            return;                                                                    \
        }                                                                              \
        if (tg_status.code() != (expected_code)) {                                     \
            tg_ctx.fail(std::string("wrong error code for ") + #expression + ": got " + \
                        std::string(::thermal_governor::to_string(tg_status.code())) + " want " +   \
                        std::string(::thermal_governor::to_string(expected_code)) + " at " +        \
                        __FILE__ + ":" + std::to_string(__LINE__));                    \
            return;                                                                    \
        }                                                                              \
    } while (false)

#endif  // THERMAL_GOVERNOR_TESTS_FRAMEWORK_HPP
