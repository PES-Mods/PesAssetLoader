#pragma once
#include <string>
#include <vector>
#include <type_traits>
#include <cstdint>
#include <cstddef>
#include "Other.h"

#define AUTHOR_INFO(str) \
struct _Plugin_AuthorInfo { \
    virtual const char *GetName() { \
        return str; \
    } \
} info

namespace obf_impl {

using key_type = std::uint64_t;
using size_type = std::size_t;

template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename CHAR_T>
constexpr CHAR_T make_mask_from_byte(std::uint8_t b) {
    CHAR_T mask = 0;
    for (std::size_t i = 0; i < sizeof(CHAR_T); ++i) {
        mask = CHAR_T(mask | (CHAR_T(static_cast<std::uint64_t>(b) << (i * 8))));
    }
    return mask;
}

template <typename CHAR_T, size_type N, key_type KEY>
struct obf_literal {
    CHAR_T data[N];

    constexpr obf_literal(const CHAR_T(&s)[N]) : data{} {
        for (size_type i = 0; i < N; ++i) {
            std::uint8_t b = static_cast<std::uint8_t>((KEY >> ((i % 8) * 8)) & 0xFFu);
            CHAR_T mask = make_mask_from_byte<CHAR_T>(b);
            data[i] = CHAR_T(s[i] ^ mask);
        }
    }

    constexpr const CHAR_T *ptr() const noexcept { return data; }
    constexpr size_type size() const noexcept { return N; }
};

}

#ifndef OBFUSCATE_USE_LINE_INSTEAD_OF_COUNTER
#define OBFUSCATE_KEY_FROM_COUNTER() ( (obf_impl::key_type)(__COUNTER__) * 0xff51afd7ed558ccdULL | 0x0101010101010101ULL )
#else
#define OBFUSCATE_KEY_FROM_COUNTER() ( (obf_impl::key_type)(__LINE__) * 0xff51afd7ed558ccdULL | 0x0101010101010101ULL )
#endif

#define Obfuscate(literal) \
    ([&]() -> std::basic_string< obf_impl::remove_cvref_t<decltype(*(literal))> > { \
        using CharT = obf_impl::remove_cvref_t<decltype(*(literal))>; \
        constexpr std::size_t N = sizeof(literal) / sizeof(literal[0]); \
        constexpr obf_impl::key_type KEY = OBFUSCATE_KEY_FROM_COUNTER(); \
        constexpr obf_impl::obf_literal<CharT, N, KEY> lit{ literal }; \
        CharT tmp[N]; \
        for (std::size_t i = 0; i < N; ++i) tmp[i] = lit.ptr()[i]; \
        for (std::size_t i = 0; i + 1 < N; ++i) { \
            std::uint8_t b = static_cast<std::uint8_t>((KEY >> ((i % 8) * 8)) & 0xFFu); \
            CharT mask = obf_impl::make_mask_from_byte<CharT>(b); \
            tmp[i] = CharT(tmp[i] ^ mask); \
        } \
        std::basic_string<CharT> out(tmp, tmp + (N - 1)); \
        for (std::size_t i = 0; i < N; ++i) tmp[i] = CharT(0); \
        return out; \
    }())

#define Obf(s) Obfuscate(s)
#define OBF(s) Obfuscate(s)
#define OBFUSCATE(s) Obfuscate(s)

namespace PluginLicenseCheckNamespace {

inline void DummyFunc() {}

inline std::wstring GetPluginNameW() {
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

inline std::string GetPluginNameA() {
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&DummyFunc), &hModule)) {
        char filepath[MAX_PATH];
        char outname[MAX_PATH];
        char outext[MAX_PATH];
        DWORD len = GetModuleFileNameA(hModule, filepath, MAX_PATH);
        if (len > 0) {
            _splitpath(filepath, NULL, NULL, outname, outext);
            return std::string(outname) + outext;
        }
    }
    return std::string();
}

}

namespace plugin {

template<unsigned int ...chars>
inline std::wstring Magic() {
    std::wstring out;
    (out.push_back(static_cast<wchar_t>(chars)),...);
    return out;
}

inline bool CheckPluginName(std::wstring const &pluginName) {
    auto name = PluginLicenseCheckNamespace::GetPluginNameW();
    if (plugin::ToLower(name) != plugin::ToLower(pluginName)) {
        MessageBoxW(NULL, Obfuscate(L"Plugin installation is wrong.\nPlease reinstall the plugin.").c_str(), name.c_str(), MB_ICONERROR);
        return false;
    }
    return true;
}

inline bool CheckPluginName(std::string const &pluginName) {
    auto name = PluginLicenseCheckNamespace::GetPluginNameA();
    if (plugin::ToLower(name) != plugin::ToLower(pluginName)) {
        MessageBoxA(NULL, Obfuscate("Plugin installation is wrong.\nPlease reinstall the plugin.").c_str(), name.c_str(), MB_ICONERROR);
        return false;
    }
    return true;
}

#ifdef TARGET_NAME_A
inline bool CheckPluginName() {
    return CheckPluginName(Obfuscate(TARGET_NAME_A));
}
#endif

};
