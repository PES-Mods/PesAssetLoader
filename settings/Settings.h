#pragma once

#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

class SettingsHelper {
public:
    template<typename CharT>
    static CharT ToLowerChar(CharT c) {
        if constexpr (sizeof(CharT) == 1)
            return (c >= CharT('A') && c <= CharT('Z')) ? static_cast<CharT>(c - CharT('A') + CharT('a')) : c;
        else
            return static_cast<CharT>(std::towlower(static_cast<std::wint_t>(c)));
    }

    template<typename CharT>
    static CharT ToUpperChar(CharT c) {
        if constexpr (sizeof(CharT) == 1)
            return (c >= CharT('a') && c <= CharT('z')) ? static_cast<CharT>(c - CharT('a') + CharT('A')) : c;
        else
            return static_cast<CharT>(std::towupper(static_cast<std::wint_t>(c)));
    }

    template<typename SrcT>
    static SrcT ToLower(SrcT const &str) {
        SrcT result = str;
        for (auto &c : result)
            c = ToLowerChar(c);
        return result;
    }

    template<typename SrcT>
    static SrcT ToUpper(SrcT const &str) {
        SrcT result = str;
        for (auto &c : result)
            c = ToUpperChar(c);
        return result;
    }

    template<typename SrcT>
    static void Trim(SrcT &str) {
        using CharT = typename SrcT::value_type;
        CharT const whitespace[] = { CharT(' '), CharT('\t'), CharT('\r'), CharT('\n'), CharT('\0') };
        size_t start = str.find_first_not_of(whitespace);
        if (start == SrcT::npos) {
            str.clear();
            return;
        }
        size_t end = str.find_last_not_of(whitespace);
        str = str.substr(start, end - start + 1);
    }

    template<typename SrcT>
    static std::vector<SrcT> Split(SrcT const &line, typename SrcT::value_type delim, bool trim = true, bool skipEmpty = false) {
        std::vector<SrcT> result;
        SrcT currStr;
        auto AddStr = [&, trim, skipEmpty]() {
            if (trim)
                Trim(currStr);
            if (!skipEmpty || !currStr.empty())
                result.push_back(currStr);
            currStr.clear();
        };
        for (size_t i = 0; i < line.length(); i++) {
            auto c = line[i];
            if (c == '\r' || (delim != '\n' && c == '\n'))
                break;
            if (c == delim)
                AddStr();
            else
                currStr += c;
        }
        AddStr();
        return result;
    }

    template<typename DstT, typename SrcT>
    static DstT StrToInt(SrcT const &str, bool isHex = false) {
        DstT result = 0;
        try {
            result = static_cast<DstT>(std::stoull(str, nullptr, isHex ? 16 : 10));
        }
        catch (...) {}
        return result;
    }

    template<typename SrcT>
    static float StrToFloat(SrcT const &str) {
        float result = 0.0f;
        try {
            result = std::stof(str);
        }
        catch (...) {}
        return result;
    }

    template<typename SrcT>
    static double StrToDouble(SrcT const &str) {
        double result = 0.0;
        try {
            result = std::stod(str);
        }
        catch (...) {}
        return result;
    }

    template<typename DstT, typename SrcT>
    static bool StrToInt(SrcT const &str, DstT &dst, bool isHex) {
        bool result = false;
        try {
            dst = static_cast<DstT>(std::stoull(str, nullptr, isHex ? 16 : 10));
            result = true;
        }
        catch (...) {}
        return result;
    }

    template<typename SrcT>
    static bool StrToFloat(SrcT const &str, float &dst) {
        bool result = false;
        try {
            dst = std::stof(str);
            result = true;
        }
        catch (...) {}
        return result;
    }

    template<typename SrcT>
    static bool StrToDouble(SrcT const &str, double &dst) {
        bool result = false;
        try {
            dst = std::stod(str);
            result = true;
        }
        catch (...) {}
        return result;
    }

    class FormatterUtilsUnicode {
    public:
        template<typename T> static T const &Arg(T const &arg) { return arg; }
        static wchar_t const *Arg(std::wstring const &arg) { return arg.c_str(); }
    };

    class FormatterUtilsAscii {
    public:
        template<typename T> static T const &Arg(T const &arg) { return arg; }
        static char const *Arg(std::string const &arg) { return arg.c_str(); }
    };

    template<typename ...ArgTypes>
    static std::wstring Format(std::wstring const &format, ArgTypes... args) {
        std::vector<wchar_t> buf(512);
        for (int attempt = 0; attempt < 12; attempt++) {
            int written = swprintf(buf.data(), buf.size(), format.c_str(), FormatterUtilsUnicode::Arg(args)...);
            if (written >= 0)
                return std::wstring(buf.data(), static_cast<size_t>(written));
            buf.resize(buf.size() * 2);
        }
        return std::wstring();
    }

    template<typename ...ArgTypes>
    static std::string Format(std::string const &format, ArgTypes... args) {
        std::vector<char> buf(512);
        for (;;) {
            int written = snprintf(buf.data(), buf.size(), format.c_str(), FormatterUtilsAscii::Arg(args)...);
            if (written < 0)
                return std::string();
            if (static_cast<size_t>(written) < buf.size())
                return std::string(buf.data(), static_cast<size_t>(written));
            buf.resize(static_cast<size_t>(written) + 1);
        }
    }

    static std::string ToUTF8(std::wstring const &wstr);
    static std::wstring ToUTF16(std::string const &str);

    static std::string MakeKey(std::string const &name);
    static std::string MakeKey(std::wstring const &name);
};

template<typename T, typename = void> struct SettingsParameterCallbacks;

template<typename T> struct SettingsParameterDefaultAssignCompare {
    static void Assign(T &dst, T const &src) {
        dst = src;
    }
    static bool Compare(T const &a, T const &b) {
        return a == b;
    }
};

template<typename T, bool IsEnum = std::is_enum_v<T>> struct SettingsUnderlyingType { using type = T; };
template<typename T> struct SettingsUnderlyingType<T, true> { using type = std::underlying_type_t<T>; };
template<typename T> using SettingsUnderlyingTypeT = typename SettingsUnderlyingType<T>::type;

template<typename T> struct SettingsParameterCallbacks<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>> : public SettingsParameterDefaultAssignCompare<T> {
    using UnderlyingT = SettingsUnderlyingTypeT<T>;
    static void FromString(T &dst, std::wstring const &src) {
        if (src.empty())
            return;
        size_t numberStart = (src[0] == L'+' || src[0] == L'-') ? 1 : 0;
        bool hexadecimal = src.size() >= numberStart + 2 && src[numberStart] == L'0' &&
            (src[numberStart + 1] == L'x' || src[numberStart + 1] == L'X');
        if (hexadecimal && src.size() < numberStart + 3)
            return;
        try {
            if constexpr (std::is_signed_v<UnderlyingT>)
                dst = static_cast<T>(std::stoll(src, nullptr, hexadecimal ? 16 : 10));
            else
                dst = static_cast<T>(std::stoull(src, nullptr, hexadecimal ? 16 : 10));
        }
        catch (...) {}
    }
    static void ToString(T const &src, std::wstring &dst) {
        if constexpr (std::is_signed_v<UnderlyingT>)
            dst = std::to_wstring(static_cast<long long>(src));
        else
            dst = std::to_wstring(static_cast<unsigned long long>(src));
    }
};

template<> struct SettingsParameterCallbacks<float> : public SettingsParameterDefaultAssignCompare<float> {
    static void FromString(float &dst, std::wstring const &src) {
        try { dst = std::stof(src); }
        catch (...) {}
    }
    static void ToString(float const &src, std::wstring &dst) {
        dst = std::to_wstring(src);
    }
};

template<> struct SettingsParameterCallbacks<double> : public SettingsParameterDefaultAssignCompare<double> {
    static void FromString(double &dst, std::wstring const &src) {
        try { dst = std::stod(src); }
        catch (...) {}
    }
    static void ToString(double const &src, std::wstring &dst) {
        dst = std::to_wstring(src);
    }
};

template<> struct SettingsParameterCallbacks<long double> : public SettingsParameterDefaultAssignCompare<long double> {
    static void FromString(long double &dst, std::wstring const &src) {
        try { dst = std::stold(src); }
        catch (...) {}
    }
    static void ToString(long double const &src, std::wstring &dst) {
        dst = std::to_wstring(src);
    }
};

template<> struct SettingsParameterCallbacks<bool> : public SettingsParameterDefaultAssignCompare<bool> {
    static void FromString(bool &dst, std::wstring const &src) {
        std::wstring l = SettingsHelper::ToLower(src);
        if (l == L"1" || l == L"true" || l == L"yes" || l == L"on")
            dst = true;
        else if (l == L"0" || l == L"false" || l == L"no" || l == L"off")
            dst = false;
    }
    static void ToString(bool const &src, std::wstring &dst) {
        dst = src ? L"TRUE" : L"FALSE";
    }
};

template<> struct SettingsParameterCallbacks<std::string> : public SettingsParameterDefaultAssignCompare<std::string> {
    static void FromString(std::string &dst, std::wstring const &src) {
        dst = SettingsHelper::ToUTF8(src);
    }
    static void ToString(std::string const &src, std::wstring &dst) {
        dst = SettingsHelper::ToUTF16(src);
    }
};

template<> struct SettingsParameterCallbacks<std::wstring> : public SettingsParameterDefaultAssignCompare<std::wstring> {
    static void FromString(std::wstring &dst, std::wstring const &src) {
        dst = src;
    }
    static void ToString(std::wstring const &src, std::wstring &dst) {
        dst = src;
    }
};

template<> struct SettingsParameterCallbacks<std::filesystem::path> : public SettingsParameterDefaultAssignCompare<std::filesystem::path> {
    static void FromString(std::filesystem::path &dst, std::wstring const &src) {
        dst = src;
    }
    static void ToString(std::filesystem::path const &src, std::wstring &dst) {
        dst = src.wstring();
    }
};

class SettingsAbstract abstract {
protected:
    class SettingsParameter {
    public:
        std::string mName;
        std::string mKey;
        std::ptrdiff_t mOffset = 0;
        std::shared_ptr<void> mDefaultValue;
        std::function<void(void *, void const *)> mAssign;
        std::function<bool(void const *, void const *)> mCompare;
        std::function<void(void *, std::wstring const &)> mFromString;
        std::function<void(void const *, std::wstring &)> mToString;
    };

    std::vector<SettingsParameter> mParameters;
    std::unordered_map<std::string, std::size_t> mParametersMap;

    void *MemberPtr(SettingsParameter const &parameter);
    void const *MemberPtr(SettingsParameter const &parameter) const;

    template<typename T>
    T SettingsInitializer(SettingsAbstract *base, T *member, char const *name, T defaultVal,
        void(*customRead)(T &, std::wstring const &) = nullptr,
        void(*customWrite)(T const &, std::wstring &) = nullptr)
    {
        SettingsParameter parameter;
        parameter.mName = name ? name : "";
        parameter.mKey = SettingsHelper::MakeKey(parameter.mName);
        assert(!parameter.mKey.empty() && "settings parameter must have a name");
        parameter.mOffset = reinterpret_cast<char const *>(member) - reinterpret_cast<char const *>(base);
        parameter.mDefaultValue = std::static_pointer_cast<void>(std::make_shared<T>(defaultVal));

        parameter.mAssign = [](void *dst, void const *src) {
            SettingsParameterCallbacks<T>::Assign(*static_cast<T *>(dst), *static_cast<T const *>(src));
        };
        parameter.mCompare = [](void const *a, void const *b) {
            return SettingsParameterCallbacks<T>::Compare(*static_cast<T const *>(a), *static_cast<T const *>(b));
        };
        if (customRead)
            parameter.mFromString = [customRead](void *dst, std::wstring const &src) { customRead(*static_cast<T *>(dst), src); };
        else
            parameter.mFromString = [](void *dst, std::wstring const &src) { SettingsParameterCallbacks<T>::FromString(*static_cast<T *>(dst), src); };
        if (customWrite)
            parameter.mToString = [customWrite](void const *src, std::wstring &dst) { customWrite(*static_cast<T const *>(src), dst); };
        else
            parameter.mToString = [](void const *src, std::wstring &dst) { SettingsParameterCallbacks<T>::ToString(*static_cast<T const *>(src), dst); };

        if (!parameter.mKey.empty()) {
            auto it = base->mParametersMap.find(parameter.mKey);
            if (it == base->mParametersMap.end()) {
                base->mParametersMap[parameter.mKey] = base->mParameters.size();
                base->mParameters.push_back(std::move(parameter));
            }
            else {
                assert(false && "duplicate settings parameter name");
            }
        }
        return defaultVal;
    }

public:
    bool Read(std::filesystem::path const &filename);
    bool Write(std::filesystem::path const &filename, bool onlyNonDefault = false) const;
    void Reset();
    SettingsAbstract() = default;
    virtual ~SettingsAbstract() = default;
    SettingsAbstract(std::filesystem::path const &filename) = delete;
};

#define SETTINGS_PARAMETER(type, variable, name, defaultValue) \
    type variable { SettingsInitializer<type>(this, &variable, name, defaultValue) }
#define SETTINGS_PARAMETER_CUSTOM_READ_WRITE(type, variable, name, defaultValue, customRead, customWrite) \
    type variable { SettingsInitializer<type>(this, &variable, name, defaultValue, customRead, customWrite) }
