#include "Logger.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <mutex>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_RESET   "\033[0m"

namespace {
    std::string to_utf8(std::wstring_view wstr) {
        if (wstr.empty()) return {};
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        std::string result(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size_needed, nullptr, nullptr);
        return result;
    }

    std::string current_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm = {};
        localtime_s(&tm, &tt);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M:%S") << '.'
            << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    std::string get_desktop_path() {
        char* profile = nullptr;
        size_t len = 0;
        if (_dupenv_s(&profile, &len, "USERPROFILE") != 0 || !profile) {
            // Fallback: try HOMEDRIVE + HOMEPATH
            char* drive = nullptr, * path = nullptr;
            size_t dlen = 0, plen = 0;
            if (_dupenv_s(&drive, &dlen, "HOMEDRIVE") == 0 && drive &&
                _dupenv_s(&path, &plen, "HOMEPATH") == 0 && path) {
                std::string result = std::string(drive) + std::string(path) + "\\Desktop";
                free(drive); free(path); free(profile);
                return result;
            }
            free(profile);
            return "";
        }
        std::string desktop = std::string(profile) + "\\Desktop";
        free(profile);
        return desktop;
    }
}

void Logger::InitFileLog(const std::string& filename) {
    std::lock_guard<std::mutex> lock(s_logMutex);

    if (s_fileLogReady) return; // already initialized

    std::string desktop = get_desktop_path();
    if (desktop.empty()) {
        std::cerr << "Logger: Could not determine desktop path for log file" << std::endl;
        return;
    }

    // Ensure Desktop exists
    try {
        if (!fs::exists(desktop)) {
            fs::create_directories(desktop);
        }
    }
    catch (...) {
        std::cerr << "Logger: Failed to create desktop directory" << std::endl;
        return;
    }

    std::string filepath = desktop + "\\" + filename;

    s_logFile.open(filepath, std::ios::out | std::ios::trunc);
    if (!s_logFile.is_open()) {
        std::cerr << "Logger: Failed to open log file: " << filepath << std::endl;
        return;
    }

    s_fileLogReady = true;

    // Write header
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm = {};
    localtime_s(&tm, &tt);
    s_logFile << "=== Angelfraud Scraper Log — "
        << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << " ===" << std::endl;
    s_logFile << "=== Log file: " << filepath << " ===" << std::endl;
    s_logFile << std::endl;
    s_logFile.flush();

    // Also print to console so user knows where the log is
    std::cout << "[*] Logging to " << filepath << std::endl;
}

void Logger::WriteToFile(const std::string& prefix, std::string_view message) {
    if (!s_fileLogReady) return;

    std::lock_guard<std::mutex> lock(s_logMutex);
    s_logFile << current_timestamp() << " " << prefix << " " << message << std::endl;
    s_logFile.flush();
}

void Logger::Flush() {
    if (!s_fileLogReady) return;
    std::lock_guard<std::mutex> lock(s_logMutex);
    s_logFile.flush();
}

// Narrow string implementations
void Logger::logWarning(std::string_view message) {
    std::cerr << std::format("{}[!] {}{}\n", COLOR_RED, message, COLOR_RESET);
    WriteToFile("[!]", message);
}

void Logger::logInfo(std::string_view message) {
    std::cout << std::format("{}[*] {}{}\n", COLOR_WHITE, message, COLOR_RESET);
    WriteToFile("[*]", message);
}

void Logger::logSuccess(std::string_view message) {
    std::cout << std::format("{}[+] {}{}\n", COLOR_GREEN, message, COLOR_RESET);
    WriteToFile("[+]", message);
}

// Wide string implementations (convert to UTF-8 and call narrow overloads)
void Logger::logWarning(std::wstring_view message) {
    std::string utf8 = to_utf8(message);
    logWarning(std::string_view(utf8));
}

void Logger::logInfo(std::wstring_view message) {
    std::string utf8 = to_utf8(message);
    logInfo(std::string_view(utf8));
}

void Logger::logSuccess(std::wstring_view message) {
    std::string utf8 = to_utf8(message);
    logSuccess(std::string_view(utf8));
}

void Logger::EnableANSIColors() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return;

    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
}
