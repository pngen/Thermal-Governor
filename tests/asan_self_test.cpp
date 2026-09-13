// Thermal Governor — deliberate AddressSanitizer self-test.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This executable must produce an AddressSanitizer report when built with
// THERMAL_GOVERNOR_ENABLE_ASAN. It exists to prove that sanitizer
// instrumentation is genuinely active in the configuration under test, so it
// is intentionally NOT registered as a passing test case.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/// Opaque sink so the compiler cannot remove the offending access.
volatile int g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    const bool expect_report = argc > 1 && std::strcmp(argv[1], "--expect-report") == 0;
    std::printf("ASAN SELF-TEST BEGIN expect_report=%s\n", expect_report ? "true" : "false");
#if defined(__SANITIZE_ADDRESS__)
    std::printf("ASAN SELF-TEST INSTRUMENTED __SANITIZE_ADDRESS__=%d\n", __SANITIZE_ADDRESS__);
#else
    std::printf("ASAN SELF-TEST NOT-INSTRUMENTED\n");
#endif
    std::fflush(stdout);

    // Deliberate heap buffer overflow. Under AddressSanitizer this aborts
    // with a report; the harness never runs it without instrumentation.
    int* buffer = new int[4];
    for (int i = 0; i < 4; ++i) {
        buffer[i] = i;
    }
    buffer[7] = 42;
    g_sink = buffer[7];
    delete[] buffer;

    std::printf("ASAN SELF-TEST NO REPORT (uninstrumented build)\n");
    std::fflush(stdout);
    return expect_report ? 1 : 0;
}
