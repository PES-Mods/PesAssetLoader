#pragma once
#ifdef _USING_V110_SDK71_
#ifndef _WIN_OBJBASE_UNKNOWN_DEFINED
typedef struct IUnknown IUnknown;
#define _WIN_OBJBASE_UNKNOWN_DEFINED
#endif
#endif
#include <Windows.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <algorithm>
#include "Error.h"
#include "Other.h"
#include "Patch.h"
#include "LicenseCheck.h"

enum ePluginApp {
    APP_UNKNOWN = 0,
    APP_GAME = 1,
    APP_SETTINGS = 2,
    APP_COUNT
};

#define VERSION_UNKNOWN 0

#define PES_VERSION(game, v1, v2, v3, v4, prot, app) \
    ( ((unsigned int)(game) << 24) | \
      ((unsigned int)(v1)   << 20) | \
      ((unsigned int)(v2)   << 16) | \
      ((unsigned int)(v3)   << 12)  | \
      ((unsigned int)(v4)   << 8)  | \
      ((unsigned int)(prot) << 4)  | \
      ((unsigned int)(app)) )

#define ID_PES6          PES_VERSION(6, 1,0,0,1, 1, APP_GAME)
#define ID_PES5          PES_VERSION(5, 1,0,0,1, 1, APP_GAME)
#define ID_SETTINGS_PES6 PES_VERSION(6, 1,0,0,1, 1, APP_SETTINGS)
#define ID_SETTINGS_PES5 PES_VERSION(5, 1,0,0,1, 1, APP_SETTINGS)
#define ID_SETTINGS_WE9  PES_VERSION(9, 1,0,0,1, 1, APP_SETTINGS)

namespace PES {

class Version {
    union {
        struct {
            unsigned int mApp : 4;
            unsigned int mProtection : 4;
            unsigned int mVersion4 : 4;
            unsigned int mVersion3 : 4;
            unsigned int mVersion2 : 4;
            unsigned int mVersion1 : 4;
            unsigned int mGame : 8;
        };
        unsigned int mId;
    };
public:
    inline Version() { mId = 0; }
    inline Version(unsigned int _id) { mId = _id; }
    inline unsigned char app() { return mApp; }
    inline unsigned int id() { return mId; }
    inline unsigned char game() { return mGame; }
    inline unsigned char version1() { return mVersion1; }
    inline unsigned char version2() { return mVersion2; }
    inline unsigned char version3() { return mVersion3; }
    inline unsigned char version4() { return mVersion4; }
    inline unsigned char protection() { return mProtection; }
};

inline uintptr_t DetectAppEntryPoint() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) return 0;
    const PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const PIMAGE_NT_HEADERS64 nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const uintptr_t entry = base + static_cast<uintptr_t>(nt->OptionalHeader.AddressOfEntryPoint);
    return entry;
}

inline uintptr_t GetEntryPoint() {
    static const uintptr_t ep = DetectAppEntryPoint();
    return ep;
}

inline Version DetectAppVersion() {
    switch (GetEntryPoint()) {
    case 0x89E231: // PES5, WE9
    case 0xACDFE0: // PES5, WE9 Kitserver
        return ID_PES5;
    //case 0x899F29: // WE9LE
    //    return ID_WE9LE;
    case 0x434866: // settings PES5, WE9
        return (plugin::patch::GetUChar(0x489240) == 'W') ? ID_SETTINGS_WE9 : ID_SETTINGS_PES5;
    //case 0x434856: // settings WE9LE
    //    return ID_SETTINGS_WE9LE;
    case 0x41A3CE: // PES 6
    case 0xB7CEE0: // PES 6 Kitserver
        return ID_PES6;
    case 0x4176CB: // settings PES 6
        return ID_SETTINGS_PES6;
    }
    return VERSION_UNKNOWN;
}

inline bool IsWE9() {
    return plugin::patch::GetUChar(0xACE4B8) == 'W';
}

inline bool IsKitserverInstalled() {
    return GetEntryPoint() == 0xACDFE0 || GetEntryPoint() == 0xB7CEE0;
}

inline Version GetAppVersion() {
    static PES::Version v = DetectAppVersion();
    return v;
}

inline std::wstring GetProcessName(bool withExtension = true) {
    std::wstring result;
    wchar_t filepath[MAX_PATH + 1];
    wchar_t outname[MAX_PATH + 1];
    wchar_t ext[MAX_PATH + 1];
    DWORD length = GetModuleFileNameW(NULL, filepath, MAX_PATH);
    if (length > 0) {
        _wsplitpath(filepath, NULL, NULL, outname, ext);
        result = outname;
        if (withExtension)
            result += ext;
    }
    return result;
}

inline std::wstring GetProcessDir() {
    wchar_t filepath[MAX_PATH + 1];
    wchar_t outname[MAX_PATH + 1];
    DWORD length = GetModuleFileNameW(NULL, filepath, MAX_PATH);
    if (length > 0) {
        _wsplitpath(filepath, NULL, outname, NULL, NULL);
        return outname;
    }
    return std::wstring();
}

inline std::wstring GetModulePath(HMODULE hmodule) {
    static constexpr auto INITIAL_BUFFER_SIZE = MAX_PATH;
    static constexpr auto MAX_ITERATIONS = 7;
    std::wstring ret;
    auto bufferSize = INITIAL_BUFFER_SIZE;
    for (size_t iterations = 0; iterations < MAX_ITERATIONS; ++iterations) {
        ret.resize(bufferSize);
        auto charsReturned = GetModuleFileNameW(hmodule, &ret[0], bufferSize);
        if (charsReturned < ret.length()) {
            ret.resize(charsReturned);
            return ret;
        }
        else
            bufferSize *= 2;
    }
    return std::wstring();
}

inline std::wstring GetGameDir() {
    std::wstring modulePath = GetModulePath(NULL);
    if (!modulePath.empty()) {
        auto lastSlash = modulePath.find_last_of(L"/\\");
        if (lastSlash != std::wstring::npos) {
            modulePath.resize(lastSlash + 1);
            return modulePath;
        }
    }
    return std::wstring();
}

inline std::wstring GameDirPath(std::wstring const &subPath) {
    auto gameDir = GetGameDir();
    if (!gameDir.empty())
        return gameDir + subPath;
    return subPath;
}

inline std::wstring GameDirPath(std::wstring const &subPath, std::wstring extension) {
    return GameDirPath(subPath) + extension;
}

inline std::wstring GetPluginName() {
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&GetGameDir), &hModule)) {
        wchar_t filepath[MAX_PATH];
        wchar_t outname[MAX_PATH];
        wchar_t outext[MAX_PATH];
        DWORD len = GetModuleFileNameW(hModule, filepath, MAX_PATH);
        if (len > 0) {
            _wsplitpath(filepath, NULL, NULL, outname, outext);
            return std::wstring(outname) + outext;
        }
    }
    return std::wstring();
}

inline bool UnknownVersionMessage() {
    return plugin::Error(L"Your game version is not supported by this plugin.\nPlugin: %s\nGame process: %s\nEntryPoint: %llX",
        GetPluginName().c_str(), GetProcessName().c_str(), GetEntryPoint());
}

}
