#pragma once

#include <string>
#include <iostream>
#include <format>
#include <fstream>
#include <mutex>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

class Logger {
public:
    // Initialize file logging to desktop
    static void InitFileLog(const std::string& filename = "erscripts_log.txt");

    // Enable ANSI color support
    static void EnableANSIColors();

    // Overloads for narrow strings (std::string)
    static void logWarning(std::string_view message);
    static void logInfo(std::string_view message);
    static void logSuccess(std::string_view message);

    // Overloads for wide strings (std::wstring)
    static void logWarning(std::wstring_view message);
    static void logInfo(std::wstring_view message);
    static void logSuccess(std::wstring_view message);

    // Flush the log file (call before potential crash points)
    static void Flush();

private:
    static void WriteToFile(const std::string& prefix, std::string_view message);

    inline static std::ofstream s_logFile;
    inline static std::mutex s_logMutex;
    inline static bool s_fileLogReady = false;
};