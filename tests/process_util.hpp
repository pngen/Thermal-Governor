// Thermal Governor — Windows process helpers for real multiprocess proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_TESTS_PROCESS_UTIL_HPP
#define THERMAL_GOVERNOR_TESTS_PROCESS_UTIL_HPP

#include <cstdint>
#include <string>

namespace tg {

/// A child process launched without inheriting a visible console window.
///
/// Handle ownership is explicit: the process handle is owned by this object
/// and closed exactly once, and the child is always reaped.
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Launch an executable with arguments. The child inherits no console
    /// window (CREATE_NO_WINDOW) unless show_console is requested.
    [[nodiscard]] static bool launch(const std::string& executable,
                                     const std::string& arguments,
                                     ChildProcess& out,
                                     bool show_console = false,
                                     const std::string& working_directory = std::string());

    /// Terminate the process abruptly, emulating a crash or a kill -9.
    [[nodiscard]] bool terminate_now();

    /// Wait for exit and return the exit code. Honours no time limit: a
    /// child that never exits is a defect to diagnose.
    [[nodiscard]] bool wait(std::uint32_t& exit_code);

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::uint32_t process_id() const noexcept { return process_id_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    void close();

private:
    void* handle_ = nullptr;
    std::uint32_t process_id_ = 0;
};

/// Directory of the currently running executable.
[[nodiscard]] std::string executable_directory();

/// Absolute path of a sibling executable built next to the running one.
[[nodiscard]] std::string sibling_executable(const std::string& name);

/// Unique temporary file path that this process owns.
[[nodiscard]] std::string unique_temp_path(const std::string& stem);

/// Create a unique temporary directory and return its path.
[[nodiscard]] std::string unique_temp_directory(const std::string& stem);

/// Remove a file, ignoring absence.
void remove_file(const std::string& path);

/// Remove a directory tree, ignoring absence.
void remove_directory(const std::string& path);

/// Read an entire file into a string. Returns false when unreadable.
[[nodiscard]] bool read_file(const std::string& path, std::string& out);

/// Write an entire file. Returns false on failure.
[[nodiscard]] bool write_file(const std::string& path, const std::string& content);

}  // namespace tg

#endif  // THERMAL_GOVERNOR_TESTS_PROCESS_UTIL_HPP
