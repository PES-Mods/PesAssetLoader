#include "Log.hpp"

bool Log::enabled = false;

std::filesystem::path &LogPath() {
    static std::filesystem::path safeLogPath;
    return safeLogPath;
}

CriticalSection &Log::cs() {
    static CriticalSection criticalSection;
    return criticalSection;
}

std::map<std::filesystem::path, Log::LogFile> &Log::logFiles() {
    static std::map<std::filesystem::path, Log::LogFile> lf;
    return lf;
}

void Log::Init(std::filesystem::path const &fileName, bool enable, bool clear) {
    LogPath() = fileName;
    if (clear)
        Clear();
    enabled = enable;
}

void Log::Clear() {
    if (LogPath().empty())
        return;
    CriticalSectionLock lock(cs());
    std::error_code ec;
    if (std::filesystem::exists(LogPath(), ec))
        std::filesystem::remove(LogPath(), ec);
}

void Log::Write(const std::string &msg) {
    if (!enabled)
        return;
    CriticalSectionLock lock(cs());
    FILE *file = _wfopen(LogPath().c_str(), L"at");
    if (file) {
        fputs(msg.c_str(), file);
        fputs("\n", file);
        fclose(file);
    }
}

void Log::WriteToFile(std::filesystem::path const& fileName, std::string const& msg, std::string const& header) {
    if (!enabled)
        return;
    auto &lf = logFiles()[fileName];
    CriticalSectionLock lock(lf.cs);
    FILE *file = nullptr;
    if (!lf.created) {
        file = _wfopen(fileName.c_str(), L"wt");
        if (file) {
            if (!header.empty()) {
                fputs(header.c_str(), file);
                fputs("\n", file);
            }
            lf.created = true;
        }
    }
    else
        file = _wfopen(fileName.c_str(), L"at");
    if (file) {
        fputs(msg.c_str(), file);
        fputs("\n", file);
        fclose(file);
    }
}
