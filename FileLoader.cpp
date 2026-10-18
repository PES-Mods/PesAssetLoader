#include "Main.hpp"
#include "DebugPrint.h"

uintptr_t FileSetLoadOverrideFilter = 0;
uintptr_t gRegisterArchive2 = 0;
enum class AssetLoadingMode { Cached, Dynamic };
AssetLoadingMode Mode = AssetLoadingMode::Cached;
char const *GameDirPath = 0;

struct weFileId {
    short fileIndex;
    short archiveId : 3;
    short : 13;
    unsigned int ToInt() {
        return *reinterpret_cast<unsigned int *>(this);
    }

    unsigned int Key() {
        return MakeKey((ToInt() >> 16) & 7, ToInt() & 0xFFFF);
    }
    static unsigned int MakeKey(unsigned int archiveId, unsigned int fileIndex) {
        return ((archiveId & 7) << 16) | (fileIndex & 0xFFFF);
    }

    short ArchiveId() { return archiveId; }
    short FileIndex() { return fileIndex; }
};

// Language slot of an asset folder: the letter before '_' in "e_text", "g_sound", ...
// '0' - language-independent archives (0_text, 0_sound)
// 'x' - shared folder, used for any language when the file is missing in the language folder
const char kNoLangSlot = '0';
const char kSharedLangSlot = 'x';

// Currently mounted language for each archive id (0_sound, 0_text, lang sound, lang text).
// Sound (commentary) and text languages can differ, and the sound archive is re-mounted at runtime.
static char gMountedLang[4] = { kNoLangSlot, kNoLangSlot, 'e', 'e' };

static unsigned int MakeAssetKey(char langSlot, unsigned int archiveId, unsigned int fileIndex) {
    return ((unsigned int)(unsigned char)langSlot << 24) | weFileId::MakeKey(archiveId, fileIndex);
}

struct ArchiveFolder { unsigned int archiveId; char langSlot; };

// "0_text" -> {1,'0'}, "0_sound" -> {0,'0'}, "<c>_text" -> {3,c}, "<c>_sound" -> {2,c}
static bool ParseArchiveFolderName(string const &name, ArchiveFolder &out) {
    string n = ToLower(name);
    if (n.size() < 3 || n[1] != '_')
        return false;
    char c = n[0];
    if (!((c >= 'a' && c <= 'z') || c == kNoLangSlot))
        return false;
    string type = n.substr(2);
    unsigned int base = (c == kNoLangSlot) ? 0 : 2;
    if (type == "sound")
        out.archiveId = base;
    else if (type == "text")
        out.archiveId = base + 1;
    else
        return false;
    out.langSlot = c;
    return true;
}

// Folders to look up for the archive, in priority order
static vector<char> LangSlotsForArchive(unsigned int archiveId) {
    if (archiveId < 2)
        return { kNoLangSlot };
    return { gMountedLang[archiveId & 3], kSharedLangSlot };
}

static map<unsigned int, string> &AssetsMap() {
    static map<unsigned int, string> assetsMap;
    return assetsMap;
}

static vector<string> &AssetsFolders() {
    static vector<string> assetsFolders;
    return assetsFolders;
}

static string const &NormalizedGameDirPath() {
    static string path;
    if (path.empty()) {
        path = GameDirPath ? GameDirPath : "";
        if (!path.empty() && path.back() != '\\' && path.back() != '/')
            path += '\\';
    }
    return path;
}

static bool ParseFileIndex(const char *name, unsigned int &outIndex) {
    const char *lastUnderscore = nullptr;
    for (const char *p = name; *p != '\0'; ++p) {
        if (*p == '_')
            lastUnderscore = p;
    }
    if (!lastUnderscore)
        return false;

    const char *p = lastUnderscore + 1;
    if (*p < '0' || *p > '9')
        return false;

    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 0xFFFF)
            return false;
        ++p;
    }
    if (*p != '\0' && *p != '.')
        return false;

    outIndex = (unsigned int)v;
    return true;
}

struct FindHandle {
    HANDLE h;
    FindHandle(HANDLE hh = INVALID_HANDLE_VALUE) : h(hh) {}
    ~FindHandle() { if (h != INVALID_HANDLE_VALUE) FindClose(h); }
    FindHandle(const FindHandle &) = delete;
    FindHandle &operator=(const FindHandle &) = delete;
    FindHandle(FindHandle &&o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
    FindHandle &operator=(FindHandle &&o) noexcept {
        if (this != &o) { if (h != INVALID_HANDLE_VALUE) FindClose(h); h = o.h; o.h = INVALID_HANDLE_VALUE; }
        return *this;
    }
};

static bool ScanArchiveFolder(string const &archiveDir, ArchiveFolder const &folder, int wantedIndex) {
    LOG(LOG_LEVEL_1, "[FileLoader] scanning \"%s\"", archiveDir.c_str());
    vector<string> stack;
    stack.push_back(archiveDir);
    bool found = false;
    while (!stack.empty()) {
        string dirPath = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAA fd;
        FindHandle fh(FindFirstFileA((dirPath + "\\*").c_str(), &fd));
        if (fh.h == INVALID_HANDLE_VALUE)
            continue;
        do {
            const char *name = fd.cFileName;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            string entryFullPath = dirPath + "\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(entryFullPath);
                continue;
            }
            unsigned int fileIndex;
            if (!ParseFileIndex(name, fileIndex))
                continue;
            if (wantedIndex >= 0 && fileIndex != (unsigned int)wantedIndex)
                continue;
            if (entryFullPath.size() >= 256)
                continue;
            unsigned int key = MakeAssetKey(folder.langSlot, folder.archiveId, fileIndex);
            if (!AssetsMap().contains(key)) {
                AssetsMap()[key] = entryFullPath;
                LOG(LOG_LEVEL_2, "[FileLoader] registered \"%s\" at [%u,%u] (%c)", entryFullPath.c_str(), folder.archiveId, fileIndex, folder.langSlot);
            }
            if (wantedIndex >= 0)
                return true;
            found = true;
        } while (FindNextFileA(fh.h, &fd) != 0);
    }
    return found;
}

// wantedArchive/wantedIndex/wantedLangSlot: -1 (or 0 for lang slot) means "any"
static bool ScanAssetsFolders(int wantedArchive, int wantedIndex, char wantedLangSlot) {
    for (auto const &rootPath : AssetsFolders()) {
        WIN32_FIND_DATAA fd;
        FindHandle fh(FindFirstFileA((rootPath + "\\*").c_str(), &fd));
        if (fh.h == INVALID_HANDLE_VALUE)
            continue;
        do {
            const char *name = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            ArchiveFolder folder;
            if (!ParseArchiveFolderName(name, folder))
                continue;
            if (wantedArchive >= 0 && folder.archiveId != (unsigned int)wantedArchive)
                continue;
            if (wantedLangSlot && folder.langSlot != wantedLangSlot)
                continue;
            if (ScanArchiveFolder(rootPath + "\\" + name, folder, wantedIndex) && wantedIndex >= 0)
                return true;
        } while (FindNextFileA(fh.h, &fd) != 0);
    }
    return false;
}

static string const *FindAsset(unsigned int archiveId, unsigned int fileIndex) {
    for (char slot : LangSlotsForArchive(archiveId)) {
        auto it = AssetsMap().find(MakeAssetKey(slot, archiveId, fileIndex));
        if (it != AssetsMap().end())
            return &it->second;
    }
    return nullptr;
}

static int MyFilter(weFileId id) {
    //if (id.archiveId == 1)
    //    DebugPrint(Format("loading file: %d\n", id.fileIndex));
    unsigned int archiveId = id.ArchiveId() & 7;
    unsigned int fileIndex = id.FileIndex() & 0xFFFF;
    if (Mode == AssetLoadingMode::Dynamic) {
        for (char slot : LangSlotsForArchive(archiveId)) {
            AssetsMap().erase(MakeAssetKey(slot, archiveId, fileIndex));
            if (ScanAssetsFolders(archiveId, fileIndex, slot))
                break;
        }
    }
    return FindAsset(archiveId, fileIndex) ? 1 : 0;
}

static void CheckKitserverReadFile();

static void OnFileSetLoadOverrideFilter(void *filter) {
    string searchPaths[10] = {
        config().PATH1, config().PATH2, config().PATH3, config().PATH4, config().PATH5,
        config().PATH6, config().PATH7, config().PATH8, config().PATH9, config().PATH10
    };
    for (auto p : searchPaths) {
        if (!p.empty())
            AssetsFolders().push_back(p);
    }
    if (AssetsFolders().empty())
        AssetsFolders().push_back("dat\\assets\\files");
    for (auto &f : AssetsFolders()) {
        while (!f.empty() && (f.back() == '\\' || f.back() == '/'))
            f.pop_back();
    }
    Mode = ToLower(config().MODE) == "dynamic" ? AssetLoadingMode::Dynamic : AssetLoadingMode::Cached;
    if (Mode == AssetLoadingMode::Cached) {
        ScanAssetsFolders(-1, -1, 0);
        // no files: the game's filter (none), the game reads its archives as it is
        if (AssetsMap().empty()) {
            LOG(LOG_LEVEL_1, "[FileLoader] no files, the archives are read as they are");
            plugin::CallDynGlobal(FileSetLoadOverrideFilter, filter);
            return;
        }
    }
    CheckKitserverReadFile();
    plugin::CallDynGlobal(FileSetLoadOverrideFilter, MyFilter);
}

// Kitserver (kload.dll) replaces the game's call of ReadFile (call [ReadFile] -> call afsReadFile) and takes every
// read for a read of an AFS archive: it looks up the archive by the name of the game's file object (esi + 0x60:
// the path; the name after the last '\' among the 8 names in the game's AFS table, entry + 0x10) without checking
// that it found one, and crashes on the loose files. Once kload has patched the call (it does so after the plugin is
// loaded), the call is redirected to ReadFileStub: reads of the archives kload knows go to kload, others (the loose
// files) to ReadFile.
struct KitserverReadFile {
    uintptr_t call;         // the game's call [ReadFile]
    uintptr_t afsTable;     // the game's AFS table: up to 8 pointers to the archives (+0x10 name)
};
constinit static KitserverReadFile gKitserverRead = {};
constinit static uintptr_t gReadFileSlot = 0;       // the import of ReadFile (the operand of the game's call)
constinit static uintptr_t gKitserverAfsReadFile = 0;

static bool __cdecl IsKitserverAfsRead(uint8_t const *file) {
    __try {
        char const *path = (char const *)(file + 0x60);
        char const *name = strrchr(path, '\\');
        if (!name)
            return false;
        name++;
        auto archives = (char const *const *)gKitserverRead.afsTable;
        for (int i = 0; i < 8; i++) {
            if (archives[i] && !strncmp(name, archives[i] + 0x10, 20))
                return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// ReadFile(hFile, buffer, size, read, overlapped) with esi = the game's file object
static __declspec(naked) void ReadFileStub() {
    __asm {
        push esi
        call IsKitserverAfsRead
        add esp, 4
        test al, al
        jz direct
        jmp gKitserverAfsReadFile
    direct:
        mov eax, gReadFileSlot
        jmp dword ptr [eax]
    }
}

// Called before the game reads a loose file (and at startup): takes over kload's patch of the call, once it is there
static void CheckKitserverReadFile() {
    if (!gReadFileSlot || patch::GetBytes(gKitserverRead.call, 1) != "E8")
        return;
    uintptr_t target = injector::GetBranchDestination(gKitserverRead.call);
    if (target == (uintptr_t)ReadFileStub)
        return;
    gKitserverAfsReadFile = target;
    patch::RedirectCall(gKitserverRead.call, ReadFileStub);
    LOG(LOG_LEVEL_1, "[FileLoader] kitserver: loose files are read past its ReadFile hook (%X)", target);
}

static void weFileBuildLoosePath(weFileId id, char *outPath) {
    CheckKitserverReadFile();
    auto asset = FindAsset(id.ArchiveId() & 7, id.FileIndex() & 0xFFFF);
    if (asset) {
        strcpy(outPath, NormalizedGameDirPath().c_str());
        strcat(outPath, asset->c_str());
        LOG(LOG_LEVEL_3, "[FileLoader] loading \"%s\"", outPath);
    }
    else
        outPath[0] = '\0';
}

// Called for every mounted AFS archive (at startup, and again when the commentary language changes)
static char OnRegisterArchive2(int mountId, int langId, int archiveType, int fileCount, int maxFiles, const char *path) {
    unsigned int archiveId = (archiveType + 2 * (langId != 0)) & 3;
    char lang = kNoLangSlot;
    if (langId != 0 && path) {
        const char *fileName = path;
        for (const char *p = path; *p; ++p) {
            if (*p == '\\' || *p == '/')
                fileName = p + 1;
        }
        lang = (char)tolower((unsigned char)fileName[0]);
    }
    gMountedLang[archiveId] = lang;
    LOG(LOG_LEVEL_1, "[FileLoader] mounted \"%s\" as archive %u (language '%c')", path ? path : "", archiveId, lang);
    return CallAndReturnDynGlobal<char>(gRegisterArchive2, mountId, langId, archiveType, fileCount, maxFiles, path);
}

// Directory of the game executable
static char const *ExeDirPath() {
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            path[0] = '\0';
        else if (char *slash = strrchr(path, '\\'))
            slash[1] = '\0';
    }
    return path;
}

void InitFileLoader(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        FileSetLoadOverrideFilter = patch::RedirectCall(0x97797A, OnFileSetLoadOverrideFilter);
        patch::RedirectJump(0x5B8070, weFileBuildLoosePath);
        GameDirPath = (char const *)0xDB06A8;
        gRegisterArchive2 = patch::RedirectCall(0x977934, OnRegisterArchive2);
        break;
    case ID_PES6:
        FileSetLoadOverrideFilter = patch::RedirectCall(0x7E58CA, OnFileSetLoadOverrideFilter);
        patch::RedirectJump(0x65A230, weFileBuildLoosePath);
        GameDirPath = ExeDirPath();
        gRegisterArchive2 = patch::RedirectCall(0x7E5892, OnRegisterArchive2);
        // the game's call [ReadFile] (kitserver: C_READFILE_CS, AFS_PAGELEN_TABLE)
        gKitserverRead = { 0x44C014, 0x3B5CBC0 };
        if (patch::GetBytes(gKitserverRead.call, 2) == "FF 15")
            gReadFileSlot = patch::GetUInt(gKitserverRead.call + 2);
        break;
    }
}
