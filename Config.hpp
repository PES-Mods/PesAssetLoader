#pragma once
#include "settings/Settings.h"

using namespace std;

class Config : public SettingsAbstract {
public:
    SETTINGS_PARAMETER(int,    LOG_LEVEL,  "MAIN/LOG_LEVEL",         0);
    SETTINGS_PARAMETER(int,    MEMORY_MONITOR, "MAIN/MEMORY_MONITOR", 0);
    SETTINGS_PARAMETER(int,    UI_INSPECTOR,   "MAIN/UI_INSPECTOR",   0);
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
    SETTINGS_PARAMETER(int,    KIT_TEXT_MIPMAPS, "TEXTURE_LIMITS/KIT_TEXT_MIPMAPS", 1);
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
    SETTINGS_PARAMETER(wstring, TEXTURES_FOLDER, "TEXTURES/FOLDER",   L"dat\\assets\\textures");
    SETTINGS_PARAMETER(bool,    TEXTURES_LOG,    "TEXTURES/LOG",      false);
    // country flags, club badges and patterns from PNG/DDS files: FOLDER\folder_0|folder_1|folder_2\<flag id>.png|.dds
    // (<flag id>@<w>x<h>.png|.dds: only for the flags of that size); SCALE: times the game's size (power of two);
    // ASPECT: width factor of the flags and badges (1: as they are; the game's own pictures are about 0.87 times as
    // wide as they look, so 0.87 for pictures that are not squeezed yet)
    SETTINGS_PARAMETER(bool,    FLAGS,           "FLAGS/ENABLED",     true);
    SETTINGS_PARAMETER(wstring, FLAGS_FOLDER,    "FLAGS/FOLDER",      L"dat\\assets\\flags");
    SETTINGS_PARAMETER(int,     FLAGS_SCALE,     "FLAGS/SCALE",       4);
    SETTINGS_PARAMETER(string,  FLAGS_ASPECT,    "FLAGS/ASPECT",      "1.0");
    SETTINGS_PARAMETER(bool,    FLAGS_MIPMAPS,   "FLAGS/MIPMAPS",     true);
    SETTINGS_PARAMETER(string,  FLAGS_FILTER,    "FLAGS/FILTER",      "lanczos");
    SETTINGS_PARAMETER(int,     FLAGS_CACHE_MB,  "FLAGS/CACHE_MB",    96);
    // player photos on some menu panels: FOLDER\<player id>.png|.dds, background.png|.dds behind every photo,
    // no_photo.png|.dds for players without a photo; BORDER: of the background around the photo (GS lines); per place:
    // "height, x, y" (GS lines; x, y: moves the photo); EDIT_HEAD_MODEL_Y: moves the 3D head of Edit player (GS
    // lines, negative: up)
    SETTINGS_PARAMETER(bool,    PLAYER_PHOTOS,    "PLAYER_PHOTOS/ENABLED",   true);
    SETTINGS_PARAMETER(wstring, PHOTOS_FOLDER,    "PLAYER_PHOTOS/FOLDER",    L"dat\\assets\\photos");
    SETTINGS_PARAMETER(int,     PHOTOS_BORDER,    "PLAYER_PHOTOS/BORDER",    1);
    SETTINGS_PARAMETER(string,  PHOTOS_EDIT_LIST, "PLAYER_PHOTOS/EDIT_LIST", "68, 0, 0");
    SETTINGS_PARAMETER(string,  PHOTOS_EDIT_MENU, "PLAYER_PHOTOS/EDIT_MENU", "68, 0, 0");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_LIST,   "PLAYER_PHOTOS/ML_LIST",   "50, -4, -3");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_POPUP,  "PLAYER_PHOTOS/ML_POPUP",  "62, -2, 4");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_NO_PHOTO, "PLAYER_PHOTOS/ML_NO_PHOTO", "");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_POPUP_NO_PHOTO, "PLAYER_PHOTOS/ML_POPUP_NO_PHOTO", "3d");   // older name
    SETTINGS_PARAMETER(string,  PHOTOS_ML_SEASON,  "PLAYER_PHOTOS/ML_SEASON",  "68, 0, 0");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_RANKING, "PLAYER_PHOTOS/ML_RANKING", "34, -152, 0");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_POINTS,  "PLAYER_PHOTOS/ML_POINTS",  "37, -152, 0");
    SETTINGS_PARAMETER(string,  PHOTOS_LEAGUE_SEASON, "PLAYER_PHOTOS/LEAGUE_SEASON", "68, 0, 0");
    SETTINGS_PARAMETER(string,  PHOTOS_ML_POPUP_3D_SCALE, "PLAYER_PHOTOS/ML_POPUP_3D_SCALE", "0.8");
    SETTINGS_PARAMETER(int,     PHOTOS_EDIT_HEAD_MODEL_Y, "PLAYER_PHOTOS/EDIT_HEAD_MODEL_Y", -30);
    SETTINGS_PARAMETER(bool,    PHOTOS_EDIT_LIST_FULL_3D, "PLAYER_PHOTOS/EDIT_LIST_FULL_3D", true);
    // game texts from .tr files in plugins and dat\assets\translation (and their subfolders)
    SETTINGS_PARAMETER(bool,    TEXT_LOADER,     "TEXT_LOADER/ENABLED", true);
};
