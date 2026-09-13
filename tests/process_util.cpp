// Thermal Governor — Windows process helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "process_util.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tg {
namespace {

std::atomic<std::uint64_t> g_counter{0};

#ifdef _WIN32
[[nodiscard]] std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                          size);
    return out;
}

[[nodiscard]] std::string from_wide(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                          size, nullptr, nullptr);
    return out;
}
#endif

}  // namespace

ChildProcess::~ChildProcess() { close(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), process_id_(other.process_id_) {
    other.handle_ = nullptr;
    other.process_id_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        process_id_ = other.process_id_;
        other.handle_ = nullptr;
        other.process_id_ = 0;
    }
    return *this;
}

void ChildProcess::close() {
#ifdef _WIN32
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
#endif
    handle_ = nullptr;
    process_id_ = 0;
}

bool ChildProcess::launch(const std::string& executable, const std::string& arguments,
                          ChildProcess& out, bool show_console,
                          const std::string& working_directory) {
#ifdef _WIN32
    std::string command = "\"" + executable + "\"";
    if (!arguments.empty()) {
        command += " ";
        command += arguments;
    }

    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};

    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (!show_console) {
        // Helper processes must never flash a console window.
        flags |= CREATE_NO_WINDOW;
    }

    const std::wstring wide_working = working_directory.empty()
                                          ? std::wstring{}
                                          : to_wide(working_directory);
    const char* working = working_directory.empty() ? nullptr : working_directory.c_str();

    // The child inherits the parent standard handles so that a helper
    // process writes its diagnostics into the driver output stream. A
    // visible console is still suppressed by CREATE_NO_WINDOW, so no
    // unwanted window ever appears.
    const BOOL created = ::CreateProcessA(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE, flags, nullptr,
        working, &startup, &information);
    if (created == FALSE) {
        return false;
    }
    ::CloseHandle(information.hThread);
    out.close();
    out.handle_ = information.hProcess;
    out.process_id_ = information.dwProcessId;
    (void)wide_working;
    return true;
#else
    (void)executable;
    (void)arguments;
    (void)out;
    (void)show_console;
    (void)working_directory;
    return false;
#endif
}

bool ChildProcess::terminate_now() {
#ifdef _WIN32
    if (handle_ == nullptr) {
        return false;
    }
    return ::TerminateProcess(static_cast<HANDLE>(handle_), 0xDEADU) != FALSE;
#else
    return false;
#endif
}

bool ChildProcess::wait(std::uint32_t& exit_code) {
#ifdef _WIN32
    if (handle_ == nullptr) {
        return false;
    }
    // No timeout: the wait is unbounded on purpose. A child that never
    // exits is a defect to diagnose, never something to paper over.
    const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    if (result != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == FALSE) {
        return false;
    }
    exit_code = static_cast<std::uint32_t>(code);
    return true;
#else
    (void)exit_code;
    return false;
#endif
}

bool ChildProcess::running() const {
#ifdef _WIN32
    if (handle_ == nullptr) {
        return false;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == FALSE) {
        return false;
    }
    return code == STILL_ACTIVE;
#else
    return false;
#endif
}

std::string executable_directory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    std::filesystem::path path(from_wide(buffer));
    return path.parent_path().string();
#else
    return {};
#endif
}

std::string sibling_executable(const std::string& name) {
    std::filesystem::path path(executable_directory());
    path /= name;
    return path.string();
}

std::string unique_temp_path(const std::string& stem) {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    buffer.resize(length == 0 ? 0 : length);
    std::filesystem::path base(from_wide(buffer));
#else
    std::filesystem::path base(std::filesystem::temp_directory_path());
#endif
    const std::uint64_t counter = ++g_counter;
    std::ostringstream name;
    name << stem << "-" << counter << "-" << reinterpret_cast<std::uintptr_t>(&g_counter)
         << ".tmp";
    return (base / name.str()).string();
}

std::string unique_temp_directory(const std::string& stem) {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    buffer.resize(length == 0 ? 0 : length);
    std::filesystem::path base(from_wide(buffer));
#else
    std::filesystem::path base(std::filesystem::temp_directory_path());
#endif
    const std::uint64_t counter = ++g_counter;
    std::ostringstream name;
    name << stem << "-" << counter << "-" << reinterpret_cast<std::uintptr_t>(&g_counter);
    std::filesystem::path path = base / name.str();
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return path.string();
}

void remove_file(const std::string& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

void remove_directory(const std::string& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

bool write_file(const std::string& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return stream.good();
}

}  // namespace tg
