#include "Main.hpp"
#include "DebugPrint.h"

uintptr_t FileSetLoadOverrideFilter = 0;
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

static map<unsigned int, string> &AssetsMap() {
    static map<unsigned int, string> assetsMap;
    return assetsMap;
}

static vector<string> &AssetsFolders() {
    static vector<string> assetsFolders;
    return assetsFolders;
}

static map<string, unsigned int> &ArchiveIDs() {
    static map<string, unsigned int> archiveIDs = {
        { "0_sound", 0 }, { "0_text", 1 },
        { "e_sound", 2 }, { "e_text", 3 },
        { "j_sound", 2 }, { "j_text", 3 },
        { "g_sound", 2 }, { "g_text", 3 },
        { "f_sound", 2 }, { "f_text", 3 },
        { "i_sound", 2 }, { "i_text", 3 },
        { "s_sound", 2 }, { "s_text", 3 },
        { "k_sound", 2 }, { "k_text", 3 }
    };
    return archiveIDs;
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

static bool ScanArchiveFolder(string const &archiveDir, unsigned int archiveId, int wantedIndex) {
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
            unsigned int key = weFileId::MakeKey(archiveId, fileIndex);
            if (!AssetsMap().contains(key)) {
                AssetsMap()[key] = entryFullPath;
                LOG(LOG_LEVEL_2, "[FileLoader] registered \"%s\" at [%u,%u]", entryFullPath.c_str(), archiveId, fileIndex);
            }
            if (wantedIndex >= 0)
                return true;
            found = true;
        } while (FindNextFileA(fh.h, &fd) != 0);
    }
    return found;
}

static void ScanAssetsFolders(int wantedArchive, int wantedIndex) {
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
            auto it = ArchiveIDs().find(ToLower(string(name)));
            if (it == ArchiveIDs().end())
                continue;
            if (wantedArchive >= 0 && it->second != (unsigned int)wantedArchive)
                continue;
            if (ScanArchiveFolder(rootPath + "\\" + name, it->second, wantedIndex) && wantedIndex >= 0)
                return;
        } while (FindNextFileA(fh.h, &fd) != 0);
    }
}

static int MyFilter(weFileId id) {
    //if (id.archiveId == 1)
    //    DebugPrint(Format("loading file: %d\n", id.fileIndex));
    unsigned int key = id.Key();
    if (Mode == AssetLoadingMode::Dynamic) {
        AssetsMap().erase(key);
        ScanAssetsFolders((key >> 16) & 7, key & 0xFFFF);
    }
    return AssetsMap().contains(key) ? 1 : 0;
}

static void OnFileSetLoadOverrideFilter(void *) {
    string searchPaths[10] = {
        config().PATH1, config().PATH2, config().PATH3, config().PATH4, config().PATH5,
        config().PATH6, config().PATH7, config().PATH8, config().PATH9, config().PATH10
    };
    for (auto p : searchPaths) {
        if (!p.empty())
            AssetsFolders().push_back(p);
    }
    if (AssetsFolders().empty())
        AssetsFolders().push_back("dat");
    for (auto &f : AssetsFolders()) {
        while (!f.empty() && (f.back() == '\\' || f.back() == '/'))
            f.pop_back();
    }
    Mode = ToLower(config().MODE) == "dynamic" ? AssetLoadingMode::Dynamic : AssetLoadingMode::Cached;
    if (Mode == AssetLoadingMode::Cached)
        ScanAssetsFolders(-1, -1);
    plugin::CallDynGlobal(FileSetLoadOverrideFilter, MyFilter);
}

static void weFileBuildLoosePath(weFileId id, char *outPath) {
    auto it = AssetsMap().find(id.Key());
    if (it != AssetsMap().end()) {
        strcpy(outPath, NormalizedGameDirPath().c_str());
        strcat(outPath, it->second.c_str());
        LOG(LOG_LEVEL_3, "[FileLoader] loading \"%s\"", outPath);
    }
    else
        outPath[0] = '\0';
}

void OnLoadBallTexture(weFileId fileId, void *callback, const void *userData, unsigned int userDataSize) {
    Call<0x5B73D0>(fileId, callback, userData, userDataSize); // weFileLoad
    Call<0x5B73D0>(weFileId::MakeKey(1, 12'000), callback, userData, userDataSize); // weFileLoad
}

void OnClearBall() {
    Call<0x61E4B0>(); // ClearBall
    auto nmTex = CallAndReturn<void *, 0x8D5600>(12'000); // GetTextureAssetIfLoaded
    if (nmTex) {
        Call<0x8D57D0>(nmTex); // TexDel
        Call<0x8311F0>(nmTex); // ArenaFree
    }
}

int OnGetNormalMapParams(void *mat, float *dir) {
    void *light = (float *)(0xE97540 + 0x60 * 2);

    *raw_ptr<int>(light, 0x50) = 8;

    *raw_ptr<float>(light, 0x0) = 1.0f;
    *raw_ptr<float>(light, 0x4) = 1.0f;
    *raw_ptr<float>(light, 0x8) = 1.0f;
    *raw_ptr<float>(light, 0xC) = 1.0f;

    *raw_ptr<float>(light, 0x10) = 1.0f;
    *raw_ptr<float>(light, 0x14) = 1.0f;
    *raw_ptr<float>(light, 0x18) = 1.0f;
    *raw_ptr<float>(light, 0x1C) = 1.0f;

    dir[0] = 0.5f;
    dir[1] = 0.0f;
    dir[2] = 0.5f;
    dir[3] = 1.0f;
    return CallAndReturn<int, 0x429CA0>(mat, dir);
}

uintptr_t gOrigLoadBodyModel = 0;

void OnLoadBodyModel(void *fileResult) {
    auto id = raw_ptr<weFileId>(fileResult, 0x40);
    DebugPrint(Format("loading kit file: { archive: %d, file: %d }\n", id->ArchiveId(), id->FileIndex()));
    CallDynGlobal(gOrigLoadBodyModel, fileResult);
}

void InitFileLoader(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        FileSetLoadOverrideFilter = patch::RedirectCall(0x97797A, OnFileSetLoadOverrideFilter);
        patch::RedirectJump(0x5B8070, weFileBuildLoosePath);
        GameDirPath = (char const *)0xDB06A8;

        gOrigLoadBodyModel = patch::SetPointer(0x8509D0 + 1, OnLoadBodyModel);

        //patch::RedirectCall(0x61E569, OnLoadBallTexture);
        //patch::RedirectCall(0x61E52A, OnClearBall);

        //patch::RedirectCall(0x423F1E, OnGetNormalMapParams);

        //patch::RedirectJump(0x424702, 0x424A8A);
        //patch::RedirectJump(0x424575, 0x4245C0);
        break;
    }
}
