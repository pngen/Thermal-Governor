// Thermal Governor — test framework implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"

#include <cstring>
#include <string>

namespace tg {
namespace {

void emit(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

}  // namespace

void Context::phase(const char* name) {
    phase_ = name;
    if (!id_.empty()) {
        emit("PHASE " + id_ + " " + std::string(name));
    }
}

void Context::fail(std::string reason) {
    if (reason_.empty()) {
        reason_ = std::move(reason);
    }
    failed_ = true;
}

void Context::note(std::string text) { emit("NOTE " + std::move(text)); }

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::add(std::string id, void (*fn)(Context&)) {
    for (const auto& existing : cases_) {
        if (existing.id == id) {
            std::fprintf(stderr, "duplicate test case id: %s\n", id.c_str());
            std::fflush(stderr);
            std::abort();
        }
    }
    cases_.push_back(TestCase{std::move(id), fn});
}

Registrar::Registrar(const char* suite, const char* name, void (*fn)(Context&)) {
    Registry::instance().add(std::string(suite) + "::" + name, fn);
}

int run_all(int argc, char** argv) {
    std::string only_case;
    std::string filter;
    bool list = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--list") {
            list = true;
        } else if (argument == "--case" && i + 1 < argc) {
            only_case = argv[++i];
        } else if (argument == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            std::fflush(stderr);
            return 2;
        }
    }

    const auto& cases = Registry::instance().cases();

    if (list) {
        for (const auto& test : cases) {
            emit(test.id);
        }
        return 0;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t matched = 0;

    for (const auto& test : cases) {
        if (!only_case.empty() && test.id != only_case) {
            continue;
        }
        if (!filter.empty() && test.id.rfind(filter, 0) != 0) {
            continue;
        }
        ++matched;
        emit("BEGIN " + test.id);
        Context context;
        context.set_id(test.id);
        test.fn(context);
        if (context.failed()) {
            ++failed;
            emit("FAIL " + test.id + ": " + context.reason());
        } else {
            ++passed;
            emit("PASS " + test.id);
        }
    }

    if (matched == 0) {
        emit("NO CASES MATCHED");
        return 3;
    }

    emit("SUMMARY passed=" + std::to_string(passed) + " failed=" + std::to_string(failed));
    return failed == 0 ? 0 : 1;
}

}  // namespace tg

int main(int argc, char** argv) { return tg::run_all(argc, argv); }
