// Tiny dependency-free text logger. Writes timestamped lines to a log file
// next to the executable (and echoes them to the console), so the overlay can
// be diagnosed without keeping a terminal open.
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace logger {

inline std::string exeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
#endif
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

inline std::string defaultPath() {
    return exeDir() + "/steamvr_battery_indicator.log";
}

inline FILE *&stream() {
    static FILE *f = nullptr;
    return f;
}

inline bool &enabled() {
    static bool e = false;
    return e;
}

// Opens the log file in append mode. When path is empty the log is placed next
// to the executable. Returns false if the file could not be created; logging to
// the console still works in that case.
inline bool open(const std::string &path = std::string()) {
    const std::string p = path.empty() ? defaultPath() : path;
    stream() = std::fopen(p.c_str(), "a");
    enabled() = true;
    if (stream()) {
        std::fprintf(stream(), "\n");
        std::fflush(stream());
        return true;
    }
    return false;
}

inline void close() {
    if (stream()) {
        std::fclose(stream());
        stream() = nullptr;
    }
    enabled() = false;
}

inline std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                  tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

inline void write(const char *level, const std::string &msg) {
    if (!enabled()) return;
    const std::string line = timestamp() + " [" + level + "] " + msg;
    if (stream()) {
        std::fputs(line.c_str(), stream());
        std::fputc('\n', stream());
        std::fflush(stream());
    }
    std::FILE *out = (level[0] == 'I') ? stdout : stderr;
    std::fprintf(out, "%s\n", line.c_str());
}

inline void info(const std::string &m) { write("INFO", m); }
inline void warn(const std::string &m) { write("WARN", m); }
inline void error(const std::string &m) { write("ERROR", m); }

}  // namespace logger
