#include <Windows.h>
#include "Settings.h"
#include <climits>

namespace {

    bool NeedsQuotes(std::wstring const &value) {
        if (value.empty())
            return true;
        if (value.front() == L' ' || value.front() == L'\t' || value.back() == L' ' || value.back() == L'\t')
            return true;
        if (value.front() == L'"' || value.back() == L'"')
            return true;
        if (value.find(L';') != std::wstring::npos)
            return true;
        return false;
    }

    bool ReadFileAsWide(std::filesystem::path const &filename, std::wstring &out) {
        out.clear();
        FILE *file = _wfopen(filename.c_str(), L"rb");
        if (!file)
            return false;
        std::vector<unsigned char> raw;
        if (fseek(file, 0, SEEK_END) == 0) {
            long fileSize = ftell(file);
            if (fileSize > 0 && fseek(file, 0, SEEK_SET) == 0) {
                raw.resize(static_cast<size_t>(fileSize));
                size_t bytesRead = fread(raw.data(), 1, raw.size(), file);
                raw.resize(bytesRead);
            }
        }
        fclose(file);
        if (raw.empty())
            return true;

        enum class Encoding { utf8, utf16le, utf16be } encoding = Encoding::utf8;
        size_t bytesToSkip = 0;
        if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
            encoding = Encoding::utf16le;
            bytesToSkip = 2;
        }
        else if (raw.size() >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
            encoding = Encoding::utf16be;
            bytesToSkip = 2;
        }
        else if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
            bytesToSkip = 3;

        unsigned char const *data = raw.data() + bytesToSkip;
        size_t size = raw.size() - bytesToSkip;
        if (size == 0)
            return true;

        if (encoding == Encoding::utf8) {
            if (size > static_cast<size_t>(INT_MAX))
                return false;
            char const *text = reinterpret_cast<char const *>(data);
            int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(size), nullptr, 0);
            UINT codePage = CP_UTF8;
            if (length <= 0) {
                codePage = CP_ACP;
                length = MultiByteToWideChar(CP_ACP, 0, text, static_cast<int>(size), nullptr, 0);
                if (length <= 0)
                    return false;
            }
            out.resize(static_cast<size_t>(length));
            MultiByteToWideChar(codePage, 0, text, static_cast<int>(size), &out[0], length);
        }
        else {
            size_t count = size / 2;
            out.resize(count);
            for (size_t i = 0; i < count; i++) {
                unsigned int first = data[i * 2];
                unsigned int second = data[i * 2 + 1];
                out[i] = static_cast<wchar_t>(encoding == Encoding::utf16le ? (first | (second << 8)) : (second | (first << 8)));
            }
        }
        return true;
    }

    bool WriteFileFromUtf8(std::filesystem::path const &filename, std::string const &text) {
        FILE *file = _wfopen(filename.c_str(), L"wb");
        if (!file)
            return false;
        unsigned char const bom[3] = { 0xEF, 0xBB, 0xBF };
        bool result = fwrite(bom, 1, sizeof(bom), file) == sizeof(bom);
        if (result && !text.empty())
            result = fwrite(text.data(), 1, text.size(), file) == text.size();
        fclose(file);
        return result;
    }
}

std::string SettingsHelper::ToUTF8(std::wstring const &wstr) {
    if (wstr.empty() || wstr.size() > static_cast<size_t>(INT_MAX))
        return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
        return std::string();
    std::string strTo(static_cast<size_t>(sizeNeeded), 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], sizeNeeded, nullptr, nullptr);
    return strTo;
}

std::wstring SettingsHelper::ToUTF16(std::string const &str) {
    if (str.empty() || str.size() > static_cast<size_t>(INT_MAX))
        return std::wstring();
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (sizeNeeded <= 0)
        return std::wstring();
    std::wstring strTo(static_cast<size_t>(sizeNeeded), 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &strTo[0], sizeNeeded);
    return strTo;
}

std::string SettingsHelper::MakeKey(std::string const &name) {
    std::string key;
    key.reserve(name.size());
    for (char c : name)
        key += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    return key;
}

std::string SettingsHelper::MakeKey(std::wstring const &name) {
    return MakeKey(ToUTF8(name));
}

void *SettingsAbstract::MemberPtr(SettingsParameter const &parameter) {
    return reinterpret_cast<char *>(this) + parameter.mOffset;
}

void const *SettingsAbstract::MemberPtr(SettingsParameter const &parameter) const {
    return reinterpret_cast<char const *>(this) + parameter.mOffset;
}

void SettingsAbstract::Reset() {
    for (auto const &parameter : mParameters) {
        if (parameter.mAssign && parameter.mDefaultValue)
            parameter.mAssign(MemberPtr(parameter), parameter.mDefaultValue.get());
    }
}

bool SettingsAbstract::Read(std::filesystem::path const &filename) {
    Reset();
    std::wstring text;
    if (!ReadFileAsWide(filename, text))
        return false;

    std::wstring currentSection;

    auto applyLine = [&](std::wstring line) {
        SettingsHelper::Trim(line);
        if (line.empty() || line.front() == L';' || line.front() == L'#')
            return;
        bool insideQuotes = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == L'"')
                insideQuotes = !insideQuotes;
            else if (line[i] == L';' && !insideQuotes) {
                line.erase(i);
                break;
            }
        }
        SettingsHelper::Trim(line);
        if (line.empty())
            return;
        if (line.front() == L'[') {
            size_t closing = line.rfind(L']');
            if (closing != std::wstring::npos && closing > 0) {
                currentSection = line.substr(1, closing - 1);
                SettingsHelper::Trim(currentSection);
            }
            return;
        }
        size_t equals = line.find(L'=');
        if (equals == std::wstring::npos)
            return;
        std::wstring name = line.substr(0, equals);
        std::wstring value = line.substr(equals + 1);
        SettingsHelper::Trim(name);
        SettingsHelper::Trim(value);
        if (name.empty())
            return;
        bool quoted = value.size() >= 2 && value.front() == L'"' && value.back() == L'"';
        if (quoted)
            value = value.substr(1, value.size() - 2);
        else if (value.empty())
            return;
        if (!currentSection.empty())
            name = currentSection + L"/" + name;
        auto it = mParametersMap.find(SettingsHelper::MakeKey(name));
        if (it == mParametersMap.end())
            return;
        auto const &parameter = mParameters[it->second];
        if (parameter.mFromString)
            parameter.mFromString(MemberPtr(parameter), value);
    };

    size_t position = 0;
    while (position <= text.size()) {
        size_t lineEnd = text.find_first_of(L"\r\n", position);
        if (lineEnd == std::wstring::npos) {
            applyLine(text.substr(position));
            break;
        }
        applyLine(text.substr(position, lineEnd - position));
        bool crlf = text[lineEnd] == L'\r' && (lineEnd + 1) < text.size() && text[lineEnd + 1] == L'\n';
        position = lineEnd + (crlf ? 2 : 1);
    }
    return true;
}

bool SettingsAbstract::Write(std::filesystem::path const &filename, bool onlyNonDefault) const {
    if (filename.has_parent_path() && !filename.parent_path().empty()) {
        std::error_code errorCode;
        if (!std::filesystem::exists(filename.parent_path(), errorCode))
            std::filesystem::create_directories(filename.parent_path(), errorCode);
    }

    std::vector<std::wstring> sectionNames;
    std::vector<std::vector<std::wstring>> sectionLines;
    std::map<std::wstring, size_t> sectionIndices;

    auto sectionFor = [&](std::wstring const &section) -> std::vector<std::wstring> & {
        std::wstring key = SettingsHelper::ToLower(section);
        auto it = sectionIndices.find(key);
        if (it != sectionIndices.end())
            return sectionLines[it->second];
        sectionIndices[key] = sectionNames.size();
        sectionNames.push_back(section);
        sectionLines.emplace_back();
        return sectionLines.back();
    };

    for (auto const &parameter : mParameters) {
        if (!parameter.mToString)
            continue;
        if (onlyNonDefault && parameter.mCompare && parameter.mDefaultValue &&
            parameter.mCompare(MemberPtr(parameter), parameter.mDefaultValue.get()))
            continue;
        std::wstring fullName = SettingsHelper::ToUTF16(parameter.mName);
        std::wstring section;
        std::wstring name = fullName;
        size_t slashPosition = fullName.rfind(L'/');
        if (slashPosition != std::wstring::npos) {
            section = fullName.substr(0, slashPosition);
            name = fullName.substr(slashPosition + 1);
        }
        SettingsHelper::Trim(section);
        SettingsHelper::Trim(name);
        if (name.empty())
            continue;
        std::wstring value;
        parameter.mToString(MemberPtr(parameter), value);
        if (NeedsQuotes(value))
            value = L"\"" + value + L"\"";
        sectionFor(section).push_back(name + L" = " + value);
    }

    std::wstring text;
    auto appendSection = [&](size_t index) {
        if (sectionLines[index].empty())
            return;
        if (!text.empty())
            text += L"\r\n";
        if (!sectionNames[index].empty())
            text += L"[" + sectionNames[index] + L"]\r\n";
        for (auto const &line : sectionLines[index])
            text += line + L"\r\n";
    };
    for (size_t i = 0; i < sectionNames.size(); i++) {
        if (sectionNames[i].empty()) {
            appendSection(i);
            break;
        }
    }
    for (size_t i = 0; i < sectionNames.size(); i++) {
        if (!sectionNames[i].empty())
            appendSection(i);
    }

    return WriteFileFromUtf8(filename, SettingsHelper::ToUTF8(text));
}
