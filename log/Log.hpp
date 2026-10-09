#pragma once
#include <string>
#include <map>
#include <filesystem>
#include <Windows.h>

class CriticalSection {
    CRITICAL_SECTION cs;
public:
    CriticalSection() { InitializeCriticalSection(&cs); }
    ~CriticalSection() { DeleteCriticalSection(&cs); }

    void Enter() { EnterCriticalSection(&cs); }
    void Leave() { LeaveCriticalSection(&cs); }
};

class CriticalSectionLock {
    CriticalSection &c;
public:
    CriticalSectionLock(CriticalSection &c_) : c(c_) { c.Enter(); }
    ~CriticalSectionLock() { c.Leave(); }
};

class Log {
public:
    struct LogFile {
        bool created = false;
        CriticalSection cs;
    };
private:
    static bool enabled;
    static CriticalSection &cs();
    static std::map<std::filesystem::path, LogFile> &logFiles();
public:
    static void Init(std::filesystem::path const &fileName, bool enable, bool clear);
    static void Clear();
    static void Write(std::string const& msg);
    static void WriteToFile(std::filesystem::path const& fileName, std::string const& msg, std::string const& header = std::string());
};
