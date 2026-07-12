#include "utils/debug_log.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace twinx::debug {
namespace {

std::mutex logMutex;
std::atomic_uint64_t sequence{0};
std::chrono::steady_clock::time_point startedAt =
    std::chrono::steady_clock::now();
FILE* logFile = nullptr;
bool initialized = false;

void writeLineLocked(const char* line) {
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);

    if (logFile) {
        std::fputs(line, logFile);
        std::fputc('\n', logFile);
        std::fflush(logFile);
    }
}

}  // namespace

void init(const std::string& path) {
    std::lock_guard<std::mutex> lock(logMutex);

    if (logFile) {
        std::fclose(logFile);
        logFile = nullptr;
    }

    startedAt = std::chrono::steady_clock::now();
    sequence.store(0);
    initialized = true;

    const std::string previousPath = path + ".previous";
    std::remove(previousPath.c_str());
    std::rename(path.c_str(), previousPath.c_str());

    logFile = std::fopen(path.c_str(), "w");
    if (logFile)
        std::setvbuf(logFile, nullptr, _IONBF, 0);

    char line[768];
    std::snprintf(
        line,
        sizeof(line),
        "[TWINX-DBG][000000][+00000000ms][BOOT] logger initialized; "
        "sd_log=%s path=%s",
        logFile ? "yes" : "no",
        path.c_str());
    writeLineLocked(line);
}

void shutdown() {
    std::lock_guard<std::mutex> lock(logMutex);

    if (!initialized)
        return;

    writeLineLocked(
        "[TWINX-DBG][999999][SHUTDOWN] logger closing cleanly");

    if (logFile) {
        std::fclose(logFile);
        logFile = nullptr;
    }
    initialized = false;
}

bool active() {
    return initialized;
}

void log(const char* category, const char* format, ...) {
    char message[1536];

    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    // MPV log messages often include trailing newlines. Keep one record per
    // physical line so the final pre-crash sequence remains easy to inspect.
    for (char* cursor = message; *cursor; ++cursor) {
        if (*cursor == '\r' || *cursor == '\n')
            *cursor = ' ';
    }

    const uint64_t id = sequence.fetch_add(1) + 1;
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt)
            .count();
    const size_t threadId =
        std::hash<std::thread::id>{}(std::this_thread::get_id());

    char line[2048];
    std::snprintf(
        line,
        sizeof(line),
        "[TWINX-DBG][%06llu][+%08lldms][T%08zx][%s] %s",
        static_cast<unsigned long long>(id),
        static_cast<long long>(elapsed),
        threadId & 0xffffffffu,
        category ? category : "GEN",
        message);

    std::lock_guard<std::mutex> lock(logMutex);
    writeLineLocked(line);
}

uint64_t hashText(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

const char* playerEventName(int event) {
    switch (event) {
    case 0: return "MPV_LOADED";
    case 1: return "MPV_PAUSE";
    case 2: return "MPV_RESUME";
    case 3: return "MPV_STOP";
    case 4: return "LOADING_START";
    case 5: return "LOADING_END";
    case 6: return "UPDATE_DURATION";
    case 7: return "UPDATE_PROGRESS";
    case 8: return "START_FILE";
    case 9: return "END_OF_FILE";
    case 10: return "CACHE_SPEED_CHANGE";
    case 11: return "VIDEO_SPEED_CHANGE";
    case 12: return "VIDEO_VOLUME_CHANGE";
    case 13: return "VIDEO_MUTE";
    case 14: return "VIDEO_UNMUTE";
    case 15: return "MPV_FILE_ERROR";
    case 16: return "VIDEO_RECONFIG";
    case 17: return "RESET";
    default: return "UNKNOWN";
    }
}

}  // namespace twinx::debug
