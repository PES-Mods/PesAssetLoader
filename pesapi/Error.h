#pragma once
#include <Windows.h>
#include <stdio.h>
#include <string>
#include "Other.h"

namespace PluginErrorNamespace {

inline void DummyFunc() {}

inline std::wstring GetPluginName_() {
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&DummyFunc), &hModule)) {
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

inline std::wstring GetPluginNameW() {
    static std::wstring name = GetPluginName_();
    return name;
}

inline std::string WtoA(std::wstring const &str) {
    std::string result;
    result.resize(str.size());
    for (unsigned int i = 0; i < str.size(); i++)
        result[i] = static_cast<char>(static_cast<unsigned char>(str[i]));
    return result;
}

inline std::string GetPluginNameA() {
    static std::string name = WtoA(GetPluginNameW());
    return name;
}

}

namespace plugin {

struct message_gen {
    template <typename... ArgTypes>
    static bool msga(std::string const &title, int iconType, std::string const &format, ArgTypes... args) {
        MessageBoxA(GetActiveWindow(), Format(format, args...).c_str(), title.c_str(), iconType);
        return false;
    }

    template <typename... ArgTypes>
    static bool msgw(std::wstring const &title, int iconType, std::wstring const &format, ArgTypes... args) {
        MessageBoxW(GetActiveWindow(), Format(format, args...).c_str(), title.c_str(), iconType);
        return false;
    }
};

template <typename... ArgTypes> bool Message(std::string const &format, ArgTypes... Args) {
    return message_gen::msga(PluginErrorNamespace::GetPluginNameA(), MB_ICONINFORMATION, format, Args...);
}

template <typename... ArgTypes> bool Warning(std::string const &format, ArgTypes... Args) {
    return message_gen::msga(PluginErrorNamespace::GetPluginNameA(), MB_ICONWARNING, format, Args...);
}

template <typename... ArgTypes> bool Error(std::string const &format, ArgTypes... Args) {
    return message_gen::msga(PluginErrorNamespace::GetPluginNameA(), MB_ICONERROR, format, Args...);
}

template <typename... ArgTypes> bool Message(std::wstring const &format, ArgTypes... Args) {
    return message_gen::msgw(PluginErrorNamespace::GetPluginNameW(), MB_ICONINFORMATION, format, Args...);
}

template <typename... ArgTypes> bool Warning(std::wstring const &format, ArgTypes... Args) {
    return message_gen::msgw(PluginErrorNamespace::GetPluginNameW(), MB_ICONWARNING, format, Args...);
}

template <typename... ArgTypes> bool Error(std::wstring const &format, ArgTypes... Args) {
    return message_gen::msgw(PluginErrorNamespace::GetPluginNameW(), MB_ICONERROR, format, Args...);
}

template <typename... ArgTypes> bool InternalMessage(std::string const &format, ArgTypes... Args) {
    return message_gen::msga("PES Plugin Loader Message", MB_ICONINFORMATION, format, Args...);
}

template <typename... ArgTypes> bool InternalWarning(std::string const &format, ArgTypes... Args) {
    return message_gen::msga("PES Plugin Loader Warning", MB_ICONWARNING, format, Args...);
}

template <typename... ArgTypes> bool InternalError(std::string const &format, ArgTypes... Args) {
    return message_gen::msga("PES Plugin Loader Error", MB_ICONERROR, format, Args...);
}

template <typename... ArgTypes> bool InternalMessage(std::wstring const &format, ArgTypes... Args) {
    return message_gen::msgw(L"PES Plugin Loader Message", MB_ICONINFORMATION, format, Args...);
}

template <typename... ArgTypes> bool InternalWarning(std::wstring const &format, ArgTypes... Args) {
    return message_gen::msgw(L"PES Plugin Loader Warning", MB_ICONWARNING, format, Args...);
}

template <typename... ArgTypes> bool InternalError(std::wstring const &format, ArgTypes... Args) {
    return message_gen::msgw(L"PES Plugin Loader Error", MB_ICONERROR, format, Args...);
}

}
