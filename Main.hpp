#pragma once
#include "plugin.h"
#include <map>
#include <string>
#include <filesystem>
#include <Windows.h>
#include "log/Log.hpp"
#include "Config.hpp"

using namespace std;
using namespace std::filesystem;
using namespace plugin;

Config &config();

enum LogLevel { LOG_LEVEL_1 = 1, LOG_LEVEL_2 = 2, LOG_LEVEL_3 = 3 };

template<typename ...Args>
void LOG(LogLevel level, std::string const &format, Args... args) {
    if (level >= config().LOG_LEVEL) {
        char buf[4096];
        sprintf_s(buf, sizeof(buf), format.c_str(), args...);
        Log::Write(buf);
    }
}

void InitFileLoader(PES::Version v);
void InitMemoryLimits(PES::Version v);
