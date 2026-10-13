#pragma once
#include "settings/Settings.h"

using namespace std;

class Config : public SettingsAbstract {
public:
    SETTINGS_PARAMETER(int,    LOG_LEVEL,  "MAIN/LOG_LEVEL",         0);
    SETTINGS_PARAMETER(int,    MEMORY_MONITOR, "MAIN/MEMORY_MONITOR", 0);
    SETTINGS_PARAMETER(string, MODE,       "FILE_LOADER/MODE",       "");
    SETTINGS_PARAMETER(string, PATH1,      "FILE_LOADER/PATH1",      "");
    SETTINGS_PARAMETER(string, PATH2,      "FILE_LOADER/PATH2",      "");
    SETTINGS_PARAMETER(string, PATH3,      "FILE_LOADER/PATH3",      "");
    SETTINGS_PARAMETER(string, PATH4,      "FILE_LOADER/PATH4",      "");
    SETTINGS_PARAMETER(string, PATH5,      "FILE_LOADER/PATH5",      "");
    SETTINGS_PARAMETER(string, PATH6,      "FILE_LOADER/PATH6",      "");
    SETTINGS_PARAMETER(string, PATH7,      "FILE_LOADER/PATH7",      "");
    SETTINGS_PARAMETER(string, PATH8,      "FILE_LOADER/PATH8",      "");
    SETTINGS_PARAMETER(string, PATH9,      "FILE_LOADER/PATH9",      "");
    SETTINGS_PARAMETER(string, PATH10,     "FILE_LOADER/PATH10",     "");
    SETTINGS_PARAMETER(int,    LOD_MAIN_TEXTURE, "TEXTURE_LIMITS/LOD_MAIN_TEXTURE", 1);
    SETTINGS_PARAMETER(string, MIPMAP_LOD_BIAS,  "TEXTURE_LIMITS/MIPMAP_LOD_BIAS",  "");
    SETTINGS_PARAMETER(string, ARENA_MP,   "MEMORY_LIMITS/ARENA_MP", "");
    SETTINGS_PARAMETER(string, ARENA_1,    "MEMORY_LIMITS/ARENA_1",  "");
    SETTINGS_PARAMETER(string, ARENA_2,    "MEMORY_LIMITS/ARENA_2",  "");
    SETTINGS_PARAMETER(string, ARENA_3,    "MEMORY_LIMITS/ARENA_3",  "");
    SETTINGS_PARAMETER(string, ARENA_4,    "MEMORY_LIMITS/ARENA_4",  "");
    SETTINGS_PARAMETER(string, ARENA_5,    "MEMORY_LIMITS/ARENA_5",  "");
    SETTINGS_PARAMETER(string, ARENA_6,    "MEMORY_LIMITS/ARENA_6",  "");
    SETTINGS_PARAMETER(string, ARENA_7,    "MEMORY_LIMITS/ARENA_7",  "");
    SETTINGS_PARAMETER(string, ARENA_8,    "MEMORY_LIMITS/ARENA_8",  "");
    SETTINGS_PARAMETER(string, ARENA_9,    "MEMORY_LIMITS/ARENA_9",  "");
    SETTINGS_PARAMETER(string, ARENA_10,   "MEMORY_LIMITS/ARENA_10", "");
    SETTINGS_PARAMETER(string, ARENA_11,   "MEMORY_LIMITS/ARENA_11", "");
    SETTINGS_PARAMETER(string, ARENA_12,   "MEMORY_LIMITS/ARENA_12", "");
    SETTINGS_PARAMETER(string, ARENA_13,   "MEMORY_LIMITS/ARENA_13", "");
    SETTINGS_PARAMETER(string, ARENA_15,   "MEMORY_LIMITS/ARENA_15", "");
    SETTINGS_PARAMETER(string, ARENA_17,   "MEMORY_LIMITS/ARENA_17", "");
    SETTINGS_PARAMETER(string, ARENA_18,   "MEMORY_LIMITS/ARENA_18", "");
    SETTINGS_PARAMETER(string, ARENA_19,   "MEMORY_LIMITS/ARENA_19", "");
    SETTINGS_PARAMETER(string, ARENA_20,   "MEMORY_LIMITS/ARENA_20", "");
    SETTINGS_PARAMETER(string, ARENA_21,   "MEMORY_LIMITS/ARENA_21", "");
    SETTINGS_PARAMETER(string, ARENA_23,   "MEMORY_LIMITS/ARENA_23", "");
    // game textures from DDS/PNG files named like the textures inside the .txs files (FOLDER\<name>.dds|.png,
    // FOLDER\<language letter>\<name>.dds|.png first); LOG lists named textures drawn without a file (once per name, LOG_LEVEL >= 1)
    SETTINGS_PARAMETER(bool,    TEXTURES,        "TEXTURES/ENABLED",  true);
    SETTINGS_PARAMETER(wstring, TEXTURES_FOLDER, "TEXTURES/FOLDER",   L"dat\\textures");
    SETTINGS_PARAMETER(bool,    TEXTURES_LOG,    "TEXTURES/LOG",      false);
    // game texts from .tr files in plugins and dat\translation (and their subfolders)
    SETTINGS_PARAMETER(bool,    TEXT_LOADER,     "TEXT_LOADER/ENABLED", true);
};
