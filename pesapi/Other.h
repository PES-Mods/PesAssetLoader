#pragma once
#include <Windows.h>
#include <string>
#include <filesystem>

namespace plugin {

inline unsigned int Random(unsigned int min, unsigned int max) {
    return min + (rand() % (max - min + 1));
}

inline bool KeyPressed(unsigned int keyCode) {
    return (GetKeyState(keyCode) & 0x8000) != 0;
}

inline bool IsPluginInstalled(const char *pluginName) {
    return GetModuleHandleA(pluginName) ? true : false;
}

inline bool IsPluginInstalled(const wchar_t *pluginName) {
    return GetModuleHandleW(pluginName) ? true : false;
}

inline std::wstring AtoW(std::string const &str) {
	std::wstring result;
	result.resize(str.size());
	for (unsigned int i = 0; i < str.size(); i++)
		result[i] = static_cast<wchar_t>(static_cast<unsigned char>(str[i]));
	return result;
}

inline std::string WtoA(std::wstring const &str) {
	std::string result;
	result.resize(str.size());
	for (unsigned int i = 0; i < str.size(); i++)
		result[i] = static_cast<char>(static_cast<unsigned char>(str[i]));
	return result;
}

inline std::string ToUpper(std::string const &str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++)
        result += toupper(static_cast<unsigned char>(str[i]));
    return result;
}

inline std::wstring ToUpper(std::wstring const &str) {
    std::wstring result;
    for (size_t i = 0; i < str.length(); i++)
        result += toupper(static_cast<wchar_t>(str[i]));
    return result;
}

inline std::string ToLower(std::string const &str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++)
        result += tolower(static_cast<unsigned char>(str[i]));
    return result;
}

inline std::wstring ToLower(std::wstring const &str) {
    std::wstring result;
    for (size_t i = 0; i < str.length(); i++)
        result += towlower(static_cast<wchar_t>(str[i]));
    return result;
}

inline bool StartsWith(std::string const &str, std::string const &with) {
    return str.substr(0, with.length()) == with;
}

inline bool StartsWith(std::wstring const &str, std::wstring const &with) {
    return str.substr(0, with.length()) == with;
}

inline bool EndsWith(std::string const &str, std::string const &with) {
    auto strLen = str.length();
    auto whatLen = with.length();
    if (whatLen > strLen)
        return false;
    return str.substr(strLen - whatLen) == with;
}

inline bool EndsWith(std::wstring const &str, std::wstring const &with) {
    auto strLen = str.length();
    auto whatLen = with.length();
    if (whatLen > strLen)
        return false;
    return str.substr(strLen - whatLen) == with;
}

namespace FormatterUtils {

template <typename T>
decltype(auto) PrepareAscii(T &&arg) {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, std::filesystem::path>)
        return arg.string();
    else
        return std::forward<T>(arg);
}

template <typename T>
decltype(auto) ExtractAscii(T &&arg) {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, std::string>)
        return arg.c_str();
    else
        return std::forward<T>(arg);
}

template <typename T>
decltype(auto) PrepareUnicode(T &&arg) {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, std::filesystem::path>)
        return arg.wstring();
    else
        return std::forward<T>(arg);
}

template <typename T>
decltype(auto) ExtractUnicode(T &&arg) {
    using Decayed = std::decay_t<T>;
    if constexpr (std::is_same_v<Decayed, std::wstring>)
        return arg.c_str();
    else
        return std::forward<T>(arg);
}

}

template<typename ...ArgTypes>
std::string Format(const std::string &format, ArgTypes&&... args) {
    std::tuple<decltype(FormatterUtils::PrepareAscii(std::forward<ArgTypes>(args)))...>
        safe_args(FormatterUtils::PrepareAscii(std::forward<ArgTypes>(args))...);
    char buf[4096] = {};
    std::apply([&](auto&&... unpacked_args) {
        snprintf(buf, std::size(buf), format.c_str(),
            FormatterUtils::ExtractAscii(std::forward<decltype(unpacked_args)>(unpacked_args))...);
    }, safe_args);
    return buf;
}

template<typename ...ArgTypes>
std::wstring Format(const std::wstring &format, ArgTypes&&... args) {
    std::tuple<decltype(FormatterUtils::PrepareUnicode(std::forward<ArgTypes>(args)))...>
        safe_args(FormatterUtils::PrepareUnicode(std::forward<ArgTypes>(args))...);
    wchar_t buf[4096] = {};
    std::apply([&](auto&&... unpacked_args) {
        swprintf(buf, std::size(buf), format.c_str(),
            FormatterUtils::ExtractUnicode(std::forward<decltype(unpacked_args)>(unpacked_args))...);
    }, safe_args);
    return buf;
}

inline bool FileExistsW(std::wstring const &filePath) {
    auto dwAttrib = GetFileAttributesW(filePath.c_str());
    return (dwAttrib != INVALID_FILE_ATTRIBUTES &&
        !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}

inline std::vector<std::wstring> FileToLinesW(std::wstring const &filePath, std::wstring const &commentLineBegin = std::wstring()) {
    std::vector<std::wstring> lines;
    FILE *file = _wfopen(filePath.c_str(), L"rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        long fileSizeWithBom = ftell(file);
        fseek(file, 0, SEEK_SET);

        enum class encoding { ascii, utf8, utf16le, utf16be } enc = encoding::ascii;

        long numBytesToSkip = 0;
        if (fileSizeWithBom >= 2) {
            unsigned char bom[3];
            bom[0] = 0;
            fread(&bom, 1, 2, file);
            fseek(file, 0, SEEK_SET);
            if (bom[0] == 0xFE && bom[1] == 0xFF) {
                enc = encoding::utf16be;
                numBytesToSkip = 2;
            }
            else if (bom[0] == 0xFF && bom[1] == 0xFE) {
                enc = encoding::utf16le;
                numBytesToSkip = 2;
            }
            else if (fileSizeWithBom >= 3) {
                bom[0] = 0;
                fread(&bom, 1, 3, file);
                fseek(file, 0, SEEK_SET);
                if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
                    enc = encoding::utf8;
                    numBytesToSkip = 3;
                }
            }
        }
        long totalSize = fileSizeWithBom - numBytesToSkip;
        char *fileData = new char[totalSize];
        fseek(file, numBytesToSkip, SEEK_SET);
        fread(fileData, 1, totalSize, file);
        fclose(file);
        long numWideChars = 0;
        switch (enc) {
        case encoding::ascii:
            numWideChars = totalSize;
            break;
        case encoding::utf8:
            numWideChars = MultiByteToWideChar(CP_UTF8, 0, fileData, totalSize, 0, 0);
            break;
        case encoding::utf16le:
        case encoding::utf16be:
            numWideChars = totalSize / 2;
            break;
        }

        wchar_t *data = new wchar_t[numWideChars];
        memset(data, 0, numWideChars * sizeof(wchar_t));

        switch (enc) {
        case encoding::ascii:
            MultiByteToWideChar(1252, 0, fileData, totalSize, data, numWideChars);
            break;
        case encoding::utf8:
            MultiByteToWideChar(CP_UTF8, 0, fileData, totalSize, data, numWideChars);
            break;
        case encoding::utf16le:
        case encoding::utf16be:
            memcpy(data, fileData, totalSize);
            break;
        }
        delete[] fileData;

        if (enc == encoding::utf16be) {
            for (long i = 0; i < numWideChars; i++)
                data[i] = (data[i] >> 8) | (data[i] << 8);
        }

        std::wstring currentLine;
        bool inComment = false;
        for (long i = 0; i < numWideChars; i++) {
            if (data[i] == L'\n') {
                if (inComment)
                    inComment = false;
                else {
                    lines.push_back(currentLine);
                    currentLine.clear();
                }
            }
            else if (data[i] == L'\r') {
                if ((i + 1) < numWideChars && data[i + 1] == L'\n')
                    i++;
                if (inComment)
                    inComment = false;
                else {
                    lines.push_back(currentLine);
                    currentLine.clear();
                }
            }
            else if (!inComment) {
                if (!commentLineBegin.empty() && !wcsncmp(&data[i], commentLineBegin.c_str(), commentLineBegin.size())) {
                    if (!currentLine.empty()) {
                        lines.push_back(currentLine);
                        currentLine.clear();
                    }
                    inComment = true;
                }
                else
                    currentLine += data[i];
            }
        }
        if (!inComment)
            lines.push_back(currentLine);
        delete[] data;
    }
    return lines;
}

inline std::vector<std::string> FileToLinesA(std::wstring const &filePath, std::string const &commentLineBegin = std::string()) {
    std::vector<std::string> lines;
    FILE *file = _wfopen(filePath.c_str(), L"rb");
    if (file) {
        fseek(file, 0, SEEK_END);
        long fileSizeWithBom = ftell(file);
        fseek(file, 0, SEEK_SET);

        enum class encoding { ascii, utf8, utf16le, utf16be } enc = encoding::ascii;

        long numBytesToSkip = 0;
        if (fileSizeWithBom >= 2) {
            unsigned char bom[3];
            bom[0] = 0;
            fread(&bom, 1, 2, file);
            fseek(file, 0, SEEK_SET);
            if (bom[0] == 0xFE && bom[1] == 0xFF) {
                enc = encoding::utf16be;
                numBytesToSkip = 2;
            }
            else if (bom[0] == 0xFF && bom[1] == 0xFE) {
                enc = encoding::utf16le;
                numBytesToSkip = 2;
            }
            else if (fileSizeWithBom >= 3) {
                bom[0] = 0;
                fread(&bom, 1, 3, file);
                fseek(file, 0, SEEK_SET);
                if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
                    enc = encoding::utf8;
                    numBytesToSkip = 3;
                }
            }
        }
        long totalSize = fileSizeWithBom - numBytesToSkip;
        char *fileData = new char[totalSize];
        fseek(file, numBytesToSkip, SEEK_SET);
        fread(fileData, 1, totalSize, file);
        fclose(file);
        if (enc == encoding::utf16be) {
            wchar_t *fileDataW = (wchar_t *)fileData;
            long numCharsW = totalSize / 2;
            for (long i = 0; i < numCharsW; i++)
                fileDataW[i] = (fileDataW[i] >> 8) | (fileDataW[i] << 8);
        }
        long numChars = 0;
        switch (enc) {
        case encoding::ascii:
        case encoding::utf8:
            numChars = totalSize;
            break;
        case encoding::utf16le:
        case encoding::utf16be:
            numChars = WideCharToMultiByte(CP_UTF8, 0, (wchar_t *)fileData, totalSize, NULL, 0, NULL, NULL);
            break;
        }

        char *data = new char[numChars];
        memset(data, 0, numChars);

        switch (enc) {
        case encoding::ascii:
        case encoding::utf8:
            memcpy(data, fileData, totalSize);
            break;
        case encoding::utf16le:
        case encoding::utf16be:
            WideCharToMultiByte(CP_UTF8, 0, (wchar_t *)fileData, totalSize, data, numChars, NULL, NULL);
            break;
        }
        delete[] fileData;

        std::string currentLine;
        bool inComment = false;
        for (long i = 0; i < numChars; i++) {
            if (data[i] == '\n') {
                if (inComment)
                    inComment = false;
                else {
                    lines.push_back(currentLine);
                    currentLine.clear();
                }
            }
            else if (data[i] == '\r') {
                if ((i + 1) < numChars && data[i + 1] == '\n')
                    i++;
                if (inComment)
                    inComment = false;
                else {
                    lines.push_back(currentLine);
                    currentLine.clear();
                }
            }
            else if (!inComment) {
                if (!commentLineBegin.empty() && !strncmp(&data[i], commentLineBegin.c_str(), commentLineBegin.size())) {
                    if (!currentLine.empty()) {
                        lines.push_back(currentLine);
                        currentLine.clear();
                    }
                    inComment = true;
                }
                else
                    currentLine += data[i];
            }
        }
        if (!inComment)
            lines.push_back(currentLine);
        delete[] data;
    }
    return lines;
}

inline std::vector<std::wstring> FileToLines(std::wstring const &filePath, std::wstring const &commentLineBegin = std::wstring()) {
    return FileToLinesW(filePath, commentLineBegin);
}

inline void Replace(std::string &str, const std::string &from, const std::string &to) {
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

inline void Replace(std::wstring &str, const std::wstring &from, const std::wstring &to) {
    if (from.empty())
        return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::wstring::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

inline void Trim(std::string &str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start != std::string::npos)
        str = str.substr(start);
    size_t end = str.find_last_not_of(" \t\r\n");
    if (end != std::string::npos)
        str = str.substr(0, end + 1);
}

inline void Trim(std::wstring &str) {
    size_t start = str.find_first_not_of(L" \t\r\n");
    if (start != std::wstring::npos)
        str = str.substr(start);
    size_t end = str.find_last_not_of(L" \t\r\n");
    if (end != std::wstring::npos)
        str = str.substr(0, end + 1);
}

inline std::vector<std::string> Split(std::string const &line, char delim, bool trim, bool skipEmpty, bool quotesHavePriority) {
    std::vector<std::string> result;
    std::string currStr;
    auto AddStr = [&, trim, skipEmpty]() {
        if (trim)
            Trim(currStr);
        if (!skipEmpty || !currStr.empty())
            result.push_back(currStr);
        currStr.clear();
    };
    bool inQuotes = false;
    for (size_t i = 0; i < line.length(); i++) {
        auto c = line[i];
        if (c == '\r' || (delim != '\n' && c == '\n'))
            break;
        if (!inQuotes) {
            if (quotesHavePriority && c == '"')
                inQuotes = true;
            else if (c == delim)
                AddStr();
            else
                currStr += c;
        }
        else {
            if (c == '"')
                inQuotes = false;
            else
                currStr += c;
        }
    }
    AddStr();
    return result;
}

inline std::vector<std::wstring> Split(std::wstring const &line, wchar_t delim, bool trim, bool skipEmpty, bool quotesHavePriority) {
    std::vector<std::wstring> result;
    std::wstring currStr;
    auto AddStr = [&, trim, skipEmpty]() {
        if (trim)
            Trim(currStr);
        if (!skipEmpty || !currStr.empty())
            result.push_back(currStr);
        currStr.clear();
    };
    bool inQuotes = false;
    for (size_t i = 0; i < line.length(); i++) {
        auto c = line[i];
        if (c == L'\r' || (delim != L'\n' && c == L'\n'))
            break;
        if (!inQuotes) {
            if (quotesHavePriority && c == L'"')
                inQuotes = true;
            else if (c == delim)
                AddStr();
            else
                currStr += c;
        }
        else {
            if (c == L'"')
                inQuotes = false;
            else
                currStr += c;
        }
    }
    AddStr();
    return result;
}

inline std::wstring Join(std::vector<std::wstring> const &strList, wchar_t delim) {
    std::wstring result;
    bool first = true;
    for (std::wstring const &str : strList) {
        if (first)
            first = false;
        else
            result += delim;
        result += str;
    }
    return result;
}

inline std::string Join(std::vector<std::string> const &strList, char delim) {
    std::string result;
    bool first = true;
    for (std::string const &str : strList) {
        if (first)
            first = false;
        else
            result += delim;
        result += str;
    }
    return result;
}

inline std::wstring Join(std::vector<std::wstring> const &strList, std::wstring const &delim) {
    std::wstring result;
    bool first = true;
    for (std::wstring const &str : strList) {
        if (first)
            first = false;
        else
            result += delim;
        result += str;
    }
    return result;
}

inline std::string Join(std::vector<std::string> const &strList, std::string const &delim) {
    std::string result;
    bool first = true;
    for (std::string const &str : strList) {
        if (first)
            first = false;
        else
            result += delim;
        result += str;
    }
    return result;
}

inline std::string ToUTF8(std::wstring const &wstr) {
    if (wstr.empty())
        return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

inline std::wstring ToUTF16(std::string const &str) {
    if (str.empty())
        return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring strTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &strTo[0], size_needed);
    return strTo;
}

}
