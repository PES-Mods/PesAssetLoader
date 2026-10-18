#include "Main.hpp"
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Game texts from .tr files ([TEXT_LOADER])
//
// The game's texts are string tables (.str files in the text archives), loaded into a list of string files.
// A text is asked for by id: table id << 16 | string id (LocalizedStringByID and the text layouts), through
// two functions: the string file holding the table (sub_59DB50) and the entry of the id in it (sub_59DB90).
// Both are replaced here: an id defined by a .tr file gets an entry of ours, also in a table no loaded file
// has (then the file lookup gives an empty file of ours), so texts can be replaced and added.
//
// An entry may name a shorter wording of the same text (short version id, a string id of the same table): the
// game draws that one instead when the text does not fit its box. A replaced text keeps the short version of
// the game's entry only when that one is replaced too (else a translation would turn back into the original
// where it is too long); "ID/SHORT" sets it.
//
// .tr files: plugins\**\*.tr, then dat\assets\translation\**\*.tr (by path; a later file wins), text in UTF-8 (with or
// without BOM), UTF-16LE with BOM, or the system code page:
//
//   ; comment (also # and //, at the start of a line)
//   [f, g]                  ; the lines below are for these languages (e_text.afs letter, name or code);
//   []                      ; for every language (the default); a language's own line wins over this
//   25|142 text             ; table id|string id (as the strings editor shows them)
//   1638542 text            ; the same as one number: table << 16 | string id (decimal or 0x hex)
//   25|142/143 text         ; with short version 143 (string id in the same table; /0 none)
//   25|150 "  spaces kept  "  ; quotes keep spaces around the text (and are removed)
//   25|151 line 1\nline 2     ; \n new line, \\ backslash
//   25|152 [color=ff0000ff]red[color=ffffffff] [button=ja] [\uE000] [spacing=-1] [font=abc] [\x11]
//                           ; tags as in the strings editor of PesAfsEditor
//   COUNTRY|42 Argentina, ARG     ; a country (nationality): its name, its abbreviation (COUNTRY_42 the same)
//   TEAM|70 Chelsea, CLS          ; a team (TEAM_70 the same)
//   TEAM|200 "MyTeam, Retro", MTR ; quotes: a name with a comma (or spaces kept)
//   TEAM|71 Everton               ; the name only (", EVE": the abbreviation only)
//
// Country and team names are not texts of the string tables: the game takes them from tables of its own (teams: the
// option file's names of the edited clubs first) through CountryName(country, abbreviation) and TeamName(team,
// abbreviation, defaults); both are hooked at their start, a name of ours wins. The screens of saved games take the
// names from the save's copy of the teams' data (RecordName(data, abbreviation)): the team is the one in the data
// (the club a Master League team is made of). The ids are the game's (PES5 and PES6
// differ). A name is kept to what the option file holds (48 bytes, the abbreviation 3): the game copies them there and
// into buffers of that size (fixed buffers of ours, as the game copies 49 / 4 bytes of a name).
namespace TextLoader {

struct GameAddresses {
    uintptr_t stringFiles;          // first loaded string file
    uintptr_t language;             // int, current text language
    char const *languageLetters;
    uintptr_t findFile;             // sub_59DB50(id): the string file holding the table of id
    uintptr_t findEntry;            // sub_59DB90(file, id): the entry of id in that file
};
constinit GameAddresses gAddr = {};

struct NameAddresses {
    uintptr_t countryName;          // char *(country, abbreviation): 0xFFFF the nation free
    uintptr_t teamName;             // char *(team, kind (1: abbreviation), defaults (no option file names))
    unsigned nationFree;            // the country of 0xFFFF
    uintptr_t recordName;           // char *(team data, abbreviation): the name in a copy of a team's data
    unsigned recordTeam;            // in the data: the team (word)
};
constinit NameAddresses gNames = {};

// sub_59DB50 and sub_59DB90, as they are (both games)
char const FIND_FILE_CODE[] =
    "A1 ?? ?? ?? ?? 57 8B 7C 24 08 C1 FF 10 85 C0 75 02 5F C3 53 56 8B 70 04 33 C9 85 F6 7E 16 8B 50 08 8B 1A "
    "8B 5B 04 66 39 7B 04 74 0F 41 83 C2 04 3B CE 7C ED 8B 40 10 85 C0 75 DA 5E 5B 5F C3";
char const FIND_ENTRY_CODE[] =
    "56 8B 74 24 08 33 C0 85 F6 74 56 8B 56 04 53 8B 5C 24 10 8B CB C1 F9 10 85 D2 57 7E 18 8B 76 08 8B 3E 8B 7F "
    "04 66 39 4F 04 74 0E 40 83 C6 04 3B C2 7C ED EB 04 8B 7C 24 10 3B C2 74 20 8B 0F 85 C9 8D 47 08 74 17 66 39 "
    "18 74 0E 49 83 C0 0C 85 C9 75 F3 5F 5B 33 C0 5E C3 85 C9 75 02 33 C0 5F 5B 5E C3";

// A string file: { file id, table count, tables (TableRef *[]), previous, next }; TableRef +4: the table
// { string count, table id, Entry[] }.
struct StringFile { int fileId, tableCount; unsigned char **tables; StringFile *prev, *next; };
struct Entry { unsigned short id, shortId, size, length; char const *text; };   // size: bytes with the null
static_assert(sizeof(Entry) == 12);

constexpr int LANGUAGES = 6;
constexpr int ALL = LANGUAGES;          // slot of the texts for every language

struct Text {
    bool defined[LANGUAGES + 1] = {};
    int shortId[LANGUAGES + 1] = {};    // -1: as the game's entry, when that short version is a text of ours
    string text[LANGUAGES + 1];
    Entry entry[LANGUAGES + 1] = {};
};

// Containers are made on first use: the plugin loads the texts from the constructor of a global object in
// another file, which may run before this file's globals are constructed.
unordered_map<unsigned, unique_ptr<Text>> &Texts() {
    static unordered_map<unsigned, unique_ptr<Text>> texts;
    return texts;
}

unordered_set<unsigned> &TablesWithTexts() {
    static unordered_set<unsigned> tables;
    return tables;
}

enum NameKind { COUNTRY, TEAM };
constexpr size_t NAME_BYTES[2] = { 48, 3 };    // the full name, the abbreviation (without the null)

struct Name {
    bool defined[2][LANGUAGES + 1] = {};        // [full name, abbreviation][language slot]
    char text[2][LANGUAGES + 1][64] = {};       // zero-padded
};

// kind << 16 | id
unordered_map<unsigned, unique_ptr<Name>> &Names() {
    static unordered_map<unsigned, unique_ptr<Name>> names;
    return names;
}

StringFile gEmptyFile = { -1, 0, nullptr, nullptr, nullptr };

int Language() {
    int language = *reinterpret_cast<int *>(gAddr.language);
    return language >= 0 && language < LANGUAGES ? language : ALL;
}

Entry *OurEntry(unsigned id, int language) {
    auto it = Texts().find(id);
    if (it == Texts().end())
        return nullptr;
    Text &t = *it->second;
    int slot = language != ALL && t.defined[language] ? language : ALL;
    return t.defined[slot] ? &t.entry[slot] : nullptr;
}

StringFile *GameFindFile(unsigned id) {
    unsigned short table = id >> 16;
    for (StringFile *f = *reinterpret_cast<StringFile **>(gAddr.stringFiles); f; f = f->next) {
        for (int i = 0; i < f->tableCount; i++) {
            if (*reinterpret_cast<unsigned short *>(*reinterpret_cast<unsigned char **>(f->tables[i] + 4) + 4) == table)
                return f;
        }
    }
    return nullptr;
}

Entry *GameFindEntry(StringFile *f, unsigned id) {
    if (!f)
        return nullptr;
    for (int i = 0; i < f->tableCount; i++) {
        int const *table = *reinterpret_cast<int **>(f->tables[i] + 4);
        if (*reinterpret_cast<unsigned short const *>(table + 1) != (id >> 16))
            continue;
        Entry *e = reinterpret_cast<Entry *>(const_cast<int *>(table + 2));
        for (int n = table[0]; n > 0; n--, e++) {
            if (e->id == (id & 0xFFFF))
                return e;
        }
        return nullptr;
    }
    return nullptr;
}

StringFile *__cdecl OnFindFile(unsigned id) {
    StringFile *f = GameFindFile(id);
    return f || !TablesWithTexts().count(id >> 16) ? f : &gEmptyFile;
}

Entry *__cdecl OnFindEntry(StringFile *f, unsigned id) {
    if (!f)
        return nullptr;
    int language = Language();
    Entry *ours = OurEntry(id, language);
    if (!ours)
        return f == &gEmptyFile ? nullptr : GameFindEntry(f, id);
    Text &t = *Texts()[id];
    int slot = int(ours - t.entry);
    if (t.shortId[slot] < 0) {
        Entry *game = f == &gEmptyFile ? nullptr : GameFindEntry(f, id);
        unsigned short shortId = game ? game->shortId : 0;
        ours->shortId = shortId && OurEntry((id & 0xFFFF0000) | shortId, language) ? shortId : 0;
    }
    return ours;
}

// ---------------------------------------------------------------------------------------------
// country and team names

constinit uintptr_t oCountryName = 0, oTeamName = 0, oRecordName = 0;

char const *OurName(NameKind kind, unsigned id, int part) {
    auto it = Names().find(unsigned(kind) << 16 | id);
    if (it == Names().end())
        return nullptr;
    Name &n = *it->second;
    int language = Language();
    int slot = language != ALL && n.defined[part][language] ? language : ALL;
    return n.defined[part][slot] ? n.text[part][slot] : nullptr;
}

char const *__cdecl OnCountryName(int country, int abbreviation) {
    unsigned id = country == 0xFFFF ? gNames.nationFree : unsigned(country);
    if (char const *name = id <= 0xFFFF ? OurName(COUNTRY, id, abbreviation ? 1 : 0) : nullptr)
        return name;
    return CallAndReturnDynGlobal<char const *>(oCountryName, country, abbreviation);
}

char const *__cdecl OnTeamName(int team, int kind, int defaults) {
    if (char const *name = OurName(TEAM, unsigned(team) & 0xFFFF, kind == 1 ? 1 : 0))
        return name;
    return CallAndReturnDynGlobal<char const *>(oTeamName, team, kind, defaults);
}

char const *__cdecl OnRecordName(uint8_t const *data, int kind) {
    if (data) {
        unsigned team = *reinterpret_cast<uint16_t const *>(data + gNames.recordTeam);
        if (char const *name = OurName(TEAM, team, kind == 1 ? 1 : 0))
            return name;
    }
    return CallAndReturnDynGlobal<char const *>(oRecordName, data, kind);
}

// ---------------------------------------------------------------------------------------------
// .tr files

string ToUtf8(wchar_t const *s, int n) {
    int size = WideCharToMultiByte(CP_UTF8, 0, s, n, nullptr, 0, nullptr, nullptr);
    string out(size_t(max(size, 0)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, s, n, out.data(), size, nullptr, nullptr);
    return out;
}

string DecodeFile(vector<unsigned char> const &data) {
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE)
        return ToUtf8(reinterpret_cast<wchar_t const *>(data.data() + 2), int((data.size() - 2) / 2));
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
        return string(data.begin() + 3, data.end());
    string s(data.begin(), data.end());
    if (s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0) > 0)
        return s;
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
    wstring w(size_t(max(n, 0)), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), n);
    return ToUtf8(w.data(), int(w.size()));
}

void AppendUtf8(string &out, unsigned cp) {
    wchar_t w[2];
    int n = 1;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        w[0] = wchar_t(0xD800 + (cp >> 10));
        w[1] = wchar_t(0xDC00 + (cp & 0x3FF));
        n = 2;
    }
    else
        w[0] = wchar_t(cp);
    out += ToUtf8(w, n);
}

bool IsHex(string const &s, size_t at, size_t n) {
    if (at + n > s.size())
        return false;
    for (size_t i = at; i < at + n; i++) {
        if (!isxdigit(static_cast<unsigned char>(s[i])))
            return false;
    }
    return true;
}

// "[name=" at `at` with a value of `chars` (accepted characters), 1..maxLen long, closed by ']':
// the value, and `at` moved past the tag
bool ReadTag(string const &s, size_t &at, char const *name, size_t maxLen, int (*accept)(int), string &value) {
    size_t len = strlen(name);
    if (s.compare(at, len, name) != 0)
        return false;
    size_t end = s.find(']', at + len);
    if (end == string::npos || end == at + len || end - (at + len) > maxLen)
        return false;
    for (size_t i = at + len; i < end; i++) {
        if (!accept(static_cast<unsigned char>(s[i])))
            return false;
    }
    value = s.substr(at + len, end - at - len);
    at = end + 1;
    return true;
}

int IsHexChar(int c) { return isxdigit(c); }
int IsAlnumChar(int c) { return isalnum(c); }
int IsSpacingChar(int c) { return isdigit(c) || c == '-'; }
int IsLowerAlpha(int c) { return c >= 'a' && c <= 'z'; }

// Tags and escapes to the game's text (0x11 marks: c colour, w spacing, m font, jklnopq buttons).
string GameText(string const &s) {
    string out;
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == '\\')) {
            out += s[i + 1] == 'n' ? '\n' : '\\';
            i += 2;
            continue;
        }
        if (c == '[') {
            size_t at = i;
            string v;
            if (ReadTag(s, at, "[color=", 8, IsHexChar, v) && v.size() == 8) {
                transform(v.begin(), v.end(), v.begin(), [](char ch) { return char(tolower(static_cast<unsigned char>(ch))); });
                out += "\x11" "c" + v;
                i = at;
                continue;
            }
            at = i;
            if (ReadTag(s, at, "[spacing=", 4, IsSpacingChar, v) && v.find('-', 1) == string::npos && v != "-") {
                char hex[4];
                sprintf_s(hex, "%02x", atoi(v.c_str()) & 0xFF);
                out += "\x11" "w";
                out += hex;
                i = at;
                continue;
            }
            at = i;
            if (ReadTag(s, at, "[font=", 4, IsAlnumChar, v)) {
                out += "\x11" "m" + v;
                i = at;
                continue;
            }
            at = i;
            if (ReadTag(s, at, "[button=", 2, IsLowerAlpha, v) && v.size() == 2 && strchr("jklnopq", v[0])) {
                out += "\x11" + v;
                i = at;
                continue;
            }
            if (s.compare(i, 3, "[\\u") == 0 && IsHex(s, i + 3, 4) && i + 7 < s.size() && s[i + 7] == ']') {
                AppendUtf8(out, strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                i += 8;
                continue;
            }
            if (s.compare(i, 3, "[\\x") == 0 && IsHex(s, i + 3, 2) && i + 5 < s.size() && s[i + 5] == ']') {
                if (char code = char(strtoul(s.substr(i + 3, 2).c_str(), nullptr, 16)))
                    out += code;
                i += 6;
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

string Trim(string const &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos)
        return string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

string Lower(string s) {
    transform(s.begin(), s.end(), s.begin(), [](char ch) { return char(tolower(static_cast<unsigned char>(ch))); });
    return s;
}

// language slot of a name in a [ ] line, -1 unknown
int LanguageOf(string const &name) {
    static char const *const names[][4] = {
        { "e", "en", "english", nullptr }, { "f", "fr", "french", nullptr }, { "g", "de", "german", nullptr },
        { "i", "it", "italian", nullptr }, { "s", "es", "spanish", nullptr }, { nullptr }
    };
    for (int l = 0; l < LANGUAGES; l++) {
        if (name.size() == 1 && name[0] == gAddr.languageLetters[l])
            return l;
        for (int k = 0; k < 4 && names[l][k]; k++) {
            if (name == names[l][k])
                return l;
        }
    }
    if (gAddr.languageLetters[5] == 'p' && (name == "pt" || name == "portuguese"))
        return 5;
    if (gAddr.languageLetters[5] == 'j' && (name == "ja" || name == "jp" || name == "japanese"))
        return 5;
    return -1;
}

bool ParseNumber(string const &s, unsigned &value) {
    if (s.empty())
        return false;
    char *end = nullptr;
    bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    unsigned long v = strtoul(s.c_str() + (hex ? 2 : 0), &end, hex ? 16 : 10);
    if (*end || !isxdigit(static_cast<unsigned char>(s[hex ? 2 : 0])))
        return false;
    value = unsigned(v);
    return true;
}

// "TABLE|STRING" or "ID", either with "/SHORT"
bool ParseId(string const &token, unsigned &id, int &shortId) {
    string s = token;
    shortId = -1;
    size_t slash = s.find('/');
    if (slash != string::npos) {
        unsigned v;
        if (!ParseNumber(s.substr(slash + 1), v) || v > 0xFFFF)
            return false;
        shortId = int(v);
        s.resize(slash);
    }
    size_t bar = s.find('|');
    if (bar != string::npos) {
        unsigned table, str;
        if (!ParseNumber(s.substr(0, bar), table) || !ParseNumber(s.substr(bar + 1), str) || table > 0xFFFF || str > 0xFFFF)
            return false;
        id = table << 16 | str;
        return true;
    }
    return ParseNumber(s, id);
}

// "COUNTRY|42" / "TEAM_70" (any case)
bool ParseNameId(string const &token, NameKind &kind, unsigned &id) {
    string t = Lower(token);
    static char const *const prefixes[2] = { "country", "team" };
    for (int k = 0; k < 2; k++) {
        size_t len = strlen(prefixes[k]);
        if (t.size() > len + 1 && t.compare(0, len, prefixes[k]) == 0 && (t[len] == '|' || t[len] == '_')) {
            kind = NameKind(k);
            return ParseNumber(t.substr(len + 1), id) && id <= 0xFFFF;
        }
    }
    return false;
}

// "full name, abbreviation": a part in quotes may have commas; an empty part: the game's
bool SplitName(string const &s, string (&parts)[2], bool (&given)[2]) {
    int n = 0;
    for (size_t i = 0;;) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            i++;
        string part;
        bool quoted = false;
        if (i < s.size() && s[i] == '"') {
            size_t close = s.find('"', i + 1);
            if (close == string::npos)
                return false;
            part = s.substr(i + 1, close - i - 1);
            quoted = true;
            for (i = close + 1; i < s.size() && (s[i] == ' ' || s[i] == '\t'); i++)
                ;
            if (i < s.size() && s[i] != ',')
                return false;
        }
        else {
            size_t comma = s.find(',', i);
            part = Trim(s.substr(i, comma == string::npos ? string::npos : comma - i));
            i = comma == string::npos ? s.size() : comma;
        }
        if (n == 2)
            return false;
        parts[n] = part;
        given[n] = quoted || !part.empty();
        n++;
        if (i >= s.size())
            return true;
        i++;    // the comma
    }
}

// a name kept to `bytes` (whole UTF-8 characters)
bool FitName(string &name, size_t bytes) {
    if (name.size() <= bytes)
        return true;
    size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80)
        end--;
    name.resize(end);
    return false;
}

void LoadFile(path const &file, int &count) {
    vector<unsigned char> data;
    {
        FILE *f = _wfopen(file.c_str(), L"rb");
        if (!f) {
            LOG(LOG_LEVEL_1, "texts: cannot open %S", file.c_str());
            return;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        data.resize(size_t(max(size, 0L)));
        if (size > 0 && fread(data.data(), 1, size_t(size), f) != size_t(size))
            data.clear();
        fclose(f);
    }
    string content = DecodeFile(data);
    bool languages[LANGUAGES + 1] = {};
    languages[ALL] = true;
    int lineNo = 0, loaded = 0;
    for (size_t pos = 0; pos <= content.size();) {
        size_t eol = content.find('\n', pos);
        if (eol == string::npos)
            eol = content.size();
        string line = Trim(content.substr(pos, eol - pos));
        pos = eol + 1;
        lineNo++;
        if (line.empty() || line[0] == ';' || line[0] == '#' || line.compare(0, 2, "//") == 0)
            continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            string rest = close == string::npos ? string() : Trim(line.substr(close + 1));
            if (close == string::npos || !(rest.empty() || rest[0] == ';' || rest[0] == '#' || rest.compare(0, 2, "//") == 0)) {
                LOG(LOG_LEVEL_1, "texts: %S(%d): bad language line", file.c_str(), lineNo);
                continue;
            }
            fill(begin(languages), end(languages), false);
            string list = Trim(line.substr(1, close - 1));
            if (list.empty()) {
                languages[ALL] = true;
                continue;
            }
            for (size_t a = 0; a <= list.size();) {
                size_t comma = list.find(',', a);
                if (comma == string::npos)
                    comma = list.size();
                string name = Lower(Trim(list.substr(a, comma - a)));
                a = comma + 1;
                int l = LanguageOf(name);
                if (l >= 0)
                    languages[l] = true;
                else if (!name.empty())
                    LOG(LOG_LEVEL_1, "texts: %S(%d): unknown language \"%s\"", file.c_str(), lineNo, name.c_str());
            }
            continue;
        }
        size_t space = line.find_first_of(" \t");
        NameKind kind;
        unsigned nameId;
        if (ParseNameId(line.substr(0, space), kind, nameId)) {
            string parts[2];
            bool given[2] = {};
            if (space == string::npos || !SplitName(Trim(line.substr(space)), parts, given) || !(given[0] || given[1])) {
                LOG(LOG_LEVEL_1, "texts: %S(%d): bad name (name, abbreviation)", file.c_str(), lineNo);
                continue;
            }
            auto &n = Names()[unsigned(kind) << 16 | nameId];
            if (!n)
                n = make_unique<Name>();
            for (int part = 0; part < 2; part++) {
                if (!given[part])
                    continue;
                string text = GameText(parts[part]);
                if (!FitName(text, NAME_BYTES[part]))
                    LOG(LOG_LEVEL_1, "texts: %S(%d): %s cut to %u bytes", file.c_str(), lineNo,
                        part ? "abbreviation" : "name", unsigned(NAME_BYTES[part]));
                for (int l = 0; l <= LANGUAGES; l++) {
                    if (!languages[l])
                        continue;
                    n->defined[part][l] = true;
                    memset(n->text[part][l], 0, sizeof(n->text[part][l]));
                    memcpy(n->text[part][l], text.data(), text.size());
                }
            }
            loaded++;
            continue;
        }
        unsigned id;
        int shortId;
        if (!ParseId(line.substr(0, space), id, shortId)) {
            LOG(LOG_LEVEL_1, "texts: %S(%d): bad id \"%s\"", file.c_str(), lineNo, line.substr(0, space).c_str());
            continue;
        }
        if (space == string::npos) {
            LOG(LOG_LEVEL_1, "texts: %S(%d): no text (use \"\" for an empty one)", file.c_str(), lineNo);
            continue;
        }
        string text = Trim(line.substr(space));
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
            text = text.substr(1, text.size() - 2);
        text = GameText(text);
        auto &t = Texts()[id];
        if (!t)
            t = make_unique<Text>();
        for (int l = 0; l <= LANGUAGES; l++) {
            if (!languages[l])
                continue;
            t->defined[l] = true;
            t->shortId[l] = shortId;
            t->text[l] = text;
        }
        TablesWithTexts().insert(id >> 16);
        loaded++;
    }
    LOG(LOG_LEVEL_2, "texts: %S: %d", file.c_str(), loaded);
    count += loaded;
}

void BuildEntries();

void LoadTexts() {
    int files = 0, count = 0;
    for (wchar_t const *folder : { L"plugins", L"dat\\assets\\translation" }) {
        path base = PES::GameDirPath(folder);
        error_code ec;
        if (!is_directory(base, ec))
            continue;
        vector<path> found;
        for (recursive_directory_iterator it(base, directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec)
                break;
            if (it->is_regular_file(ec) && !_wcsicmp(it->path().extension().c_str(), L".tr"))
                found.push_back(it->path());
        }
        sort(found.begin(), found.end(), [](path const &a, path const &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
        for (path const &file : found) {
            LoadFile(file, count);
            files++;
        }
    }
    BuildEntries();
    if (files)
        LOG(LOG_LEVEL_1, "texts: %d from %d .tr files (%u country and team names)", count, files,
            unsigned(Names().size()));
}

// the entries of the game's format, pointing at our texts
void BuildEntries() {
    for (auto &it : Texts()) {
        Text &t = *it.second;
        for (int l = 0; l <= LANGUAGES; l++) {
            if (!t.defined[l])
                continue;
            Entry &e = t.entry[l];
            e.id = it.first & 0xFFFF;
            e.shortId = t.shortId[l] > 0 ? (unsigned short)t.shortId[l] : 0;
            e.size = (unsigned short)min<size_t>(t.text[l].size() + 1, 0xFFFF);
            e.length = (unsigned short)count_if(t.text[l].begin(), t.text[l].end(), [](char ch) { return (ch & 0xC0) != 0x80; });
            e.text = t.text[l].c_str();
        }
    }
}

void Install(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        gAddr = { 0xFE598C, 0xFE6234 + 4, "efgisj", 0x59DB50, 0x59DB90 };
        gNames = { 0x8EFAA0, 0x8EFF50, 108, 0x8EFF30, 0x6C };
        break;
    case ID_PES6:
        gAddr = { 0x3A67B94, 0x1030ED8 + 4, "efgisp", 0x94E380, 0x94E3C0 };
        gNames = { 0x862F70, 0x863570, 105, 0x863550, 0x36 };
        break;
    default:
        return;
    }
    size_t fileLen = (sizeof(FIND_FILE_CODE)) / 3, entryLen = (sizeof(FIND_ENTRY_CODE)) / 3;
    string fileCode = patch::GetBytes(gAddr.findFile, fileLen);
    bool strings = fileCode.substr(15) == string(FIND_FILE_CODE).substr(15) &&
        *reinterpret_cast<uintptr_t *>(gAddr.findFile + 1) == gAddr.stringFiles &&
        patch::GetBytes(gAddr.findEntry, entryLen) == FIND_ENTRY_CODE;
    if (!strings)
        LOG(LOG_LEVEL_1, "texts: unexpected code, the texts are not installed");
    LoadTexts();
    if (strings && !Texts().empty()) {
        patch::RedirectJump(gAddr.findFile, OnFindFile);
        patch::RedirectJump(gAddr.findEntry, OnFindEntry);
    }
    if (Names().empty())
        return;
    // CountryName: mov eax, [esp+4]; cmp eax, 0FFFFh; TeamName: push esi; mov esi, [esp+8]; push edi;
    // mov edi, [esp+14h]
    oCountryName = MakeTrampoline(gNames.countryName, "8B 44 24 04 3D FF FF 00 00");
    oTeamName = MakeTrampoline(gNames.teamName, "56 8B 74 24 08 57 8B 7C 24 14");
    if (oCountryName && oTeamName) {
        patch::RedirectJump(gNames.countryName, OnCountryName);
        patch::RedirectJump(gNames.teamName, OnTeamName);
    }
    else
        LOG(LOG_LEVEL_1, "texts: unexpected code at %X / %X, the country and team names are not installed",
            gNames.countryName, gNames.teamName);
    // RecordName: mov eax, [esp+4]; test eax, eax
    oRecordName = MakeTrampoline(gNames.recordName, "8B 44 24 04 85 C0");
    if (oRecordName)
        patch::RedirectJump(gNames.recordName, OnRecordName);
    else
        LOG(LOG_LEVEL_1, "texts: unexpected code at %X, the team names of saved games are not replaced",
            gNames.recordName);
}

}

uintptr_t GameTeamNameFunction() {
    return TextLoader::oTeamName;
}

void InitTextLoader(PES::Version v) {
    if (config().TEXT_LOADER)
        TextLoader::Install(v);
}
