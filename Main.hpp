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
    if (config().LOG_LEVEL >= level) {
        char buf[4096];
        sprintf_s(buf, sizeof(buf), format.c_str(), args...);
        Log::Write(buf);
    }
}

void InitFileLoader(PES::Version v);
void InitMemoryLimits(PES::Version v);
void InitTextureLimits(PES::Version v);
void InitMemoryMonitor(PES::Version v);
void InitTextureReplacement(PES::Version v);
void InitFlagReplacer(PES::Version v);
void InitTextLoader(PES::Version v);
void InitUiInspector(PES::Version v);
void InitPlayerPhotos(PES::Version v);

// TextLoader: the game's TeamName(team, kind, defaults) without the names of the .tr files (0: not hooked)
uintptr_t GameTeamNameFunction();

uintptr_t MakeTrampoline(uintptr_t addr, char const *prologue);
uintptr_t MakeTrampolineWithCall(uintptr_t addr, char const *prologue);

// Memory allocated by the plugin itself (shown by the memory monitor)
struct MemoryStats {
    size_t modelBuffersSize = 0;    // own buffers of model slots (never freed)
    unsigned int modelBuffers = 0;
    size_t textureCopiesSize = 0;   // copies of file textures in use
    unsigned int textureCopies = 0;
};

MemoryStats &memoryStats();
