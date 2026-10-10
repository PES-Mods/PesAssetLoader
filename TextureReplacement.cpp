#include "Main.hpp"
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

// Game textures from DDS or PNG files ([TEXTURES])
//
// Screens draw parts of their textures by texel coordinates, which the game turns into 0..1 with the size of
// the texture it loaded, so a bigger texture inside a .txs shows only a corner of it. Here the game keeps its
// own texture and the file is bound in its place (SetTexture): the uvs are the same at any size of the file.
// Textures are found by the names of the game's texture name tables (the names inside the .txs files);
// a file in the folder of the current text language (e, f, g, ... like e_text.afs) comes first, as the UI
// textures of each language differ.
//
// The game draws a texture through more D3D textures than its own:
//  - the texture-palette pair cache: the image drawn with another texture's palette (e.g. while a screen fades);
//  - copies made at run time (sub_5A4860 -> sub_828910(source id, new id)), e.g. the background of a screen
//    copied to id 29999 for the loading screen shown when leaving it.
// Both are drawn from the source's file too.
namespace TextureReplacement {

struct GameAddresses {
    uintptr_t device;               // IDirect3DDevice8 *
    uintptr_t platformTextures;     // std::set<PlatformTexture *> * ordered by texId
    uintptr_t texturePairs;         // TexturePairEntry * (MRU list)
    uintptr_t textureTables;        // chain of texture name tables { id, name }
    uintptr_t language;             // int, current text language
    char const *languageLetters;    // text archive letter per language (e_text.afs, ...)
    uintptr_t deviceCreateCall;
    uintptr_t textureCopyCall;
};
constinit static GameAddresses gAddr = {};

enum D3D8Method {
    D3D8_Release = 2, D3D8_Present = 15, D3D8_CreateTexture = 20, D3D8_SetTexture = 61,
    D3D8Tex_GetLevelDesc = 14, D3D8Tex_LockRect = 16, D3D8Tex_UnlockRect = 17
};

template<typename Ret = long, typename... Args>
Ret D3D8Call(void *obj, int index, Args... args) {
    using Fn = Ret(__stdcall *)(void *, Args...);
    return (*reinterpret_cast<Fn **>(obj))[index](obj, args...);
}

struct D3DLockedRect { int pitch; void *bits; };
struct SurfDesc { unsigned format, type, usage, pool, size, msType, width, height; };

bool ReadWholeFile(wstring const &path, vector<unsigned char> &out) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size : 0);
    bool ok = size > 0 && fread(out.data(), 1, size, f) == size_t(size);
    fclose(f);
    return ok;
}

// The game's PlatformTexture of a texture id, or null. The set: { proxy, head, size }, nodes +0 left,
// +4 parent, +8 right, +12 value, +17 nil. PlatformTexture: +0 texId, +8 IDirect3DTexture8 *.
unsigned char *FindPlatformTexture(int texId) {
    unsigned char *set = *reinterpret_cast<unsigned char **>(gAddr.platformTextures);
    unsigned char *head = set ? *reinterpret_cast<unsigned char **>(set + 4) : nullptr;
    if (!head)
        return nullptr;
    unsigned char *found = head;
    for (unsigned char *node = *reinterpret_cast<unsigned char **>(head + 4); node && !node[17];) {
        if (**reinterpret_cast<int **>(node + 12) >= texId) {
            found = node;
            node = *reinterpret_cast<unsigned char **>(node);
        }
        else
            node = *reinterpret_cast<unsigned char **>(node + 8);
    }
    if (found == head)
        return nullptr;
    unsigned char *texture = *reinterpret_cast<unsigned char **>(found + 12);
    return *reinterpret_cast<int *>(texture) == texId ? texture : nullptr;
}

// Every texture of the game's name tables: fn(id, name).
template<typename Fn>
void ForEachNamedTexture(Fn fn) {
    for (unsigned char const *node = *reinterpret_cast<unsigned char const *const *>(gAddr.textureTables); node;
         node = *reinterpret_cast<unsigned char const *const *>(node + 4)) {
        int const *table = *reinterpret_cast<int const *const *>(node + 8);
        for (int i = 0; table && table[i * 2 + 1] && i < 4096; i++)
            fn(table[i * 2] & 0x7FFFFFFF, reinterpret_cast<char const *>(table[i * 2 + 1]));
    }
}

// The pair cache: MRU list of { image PlatformTexture *, palette PlatformTexture *, IDirect3DTexture8 *,
// more recent, less recent }; fn(image, d3d).
template<typename Fn>
void ForEachTexturePair(Fn fn) {
    unsigned char *first = *reinterpret_cast<unsigned char **>(gAddr.texturePairs);
    unsigned char *e = first;
    for (int n = 0; e && n < 4096; n++) {
        void *d3d = *reinterpret_cast<void **>(e + 8);
        if (!d3d)
            break;
        fn(*reinterpret_cast<unsigned char **>(e), d3d);
        e = *reinterpret_cast<unsigned char **>(e + 16);
        if (e == first)
            break;
    }
}

struct Replacement {
    int id;
    wstring path;
    void *device = nullptr;     // of the texture below
    void *texture = nullptr;    // IDirect3DTexture8, made on first use
    bool failed = false;
};

static vector<Replacement *> gReplacements;
static unordered_set<string> gNamesSeen;
static unordered_map<void *, Replacement *> gReplacementByGame;   // the game's D3D textures this frame
static unordered_map<void *, void *> gGameByReplacement;
static unordered_set<void *> gCheckedThisFrame;                     // bound this frame, not replaced
static unsigned gRemaps = 0;
static int gLanguage = -2;

struct TextureCopy { int source; void *texture; };
static unordered_map<int, TextureCopy> gCopies;                     // new id -> source
static SRWLOCK gCopiesLock = SRWLOCK_INIT;
static uintptr_t oTextureCopy = 0;

void *__cdecl OnTextureCopy(int source, int id) {
    void *texture = CallAndReturnDynGlobal<void *>(oTextureCopy, source, id);
    if (texture) {
        AcquireSRWLockExclusive(&gCopiesLock);
        gCopies[id] = { source, texture };
        ReleaseSRWLockExclusive(&gCopiesLock);
    }
    return texture;
}

// [TEXTURES] LOG: named textures drawn without a file, once per name.
static unordered_map<void *, string> gNames;
static set<string> gNamesLogged;

void MapNames() {
    gNames.clear();
    unordered_map<unsigned char *, char const *> byPlatform;
    ForEachNamedTexture([&](int id, char const *name) {
        if (unsigned char *pt = FindPlatformTexture(id)) {
            byPlatform.emplace(pt, name);
            if (void *d3d = *reinterpret_cast<void **>(pt + 8))
                gNames.emplace(d3d, name);
        }
    });
    ForEachTexturePair([&](unsigned char *image, void *d3d) {
        auto it = byPlatform.find(image);
        if (it != byPlatform.end())
            gNames.emplace(d3d, it->second);
    });
}

void LogUnreplaced(void *texture) {
    auto it = gNames.find(texture);
    if (it == gNames.end() || !gNamesLogged.insert(it->second).second)
        return;
    SurfDesc d = {};
    D3D8Call(texture, D3D8Tex_GetLevelDesc, 0u, &d);
    LOG(LOG_LEVEL_1, "texture %s (%ux%u) drawn, no file", it->second.c_str(), d.width, d.height);
}

void ScanNames(void *dev) {
    int language = *reinterpret_cast<int *>(gAddr.language);
    if (language != gLanguage) {
        gLanguage = language;
        for (Replacement *r : gReplacements) {
            if (r->texture && r->device == dev)
                D3D8Call<unsigned long>(r->texture, D3D8_Release);
            delete r;
        }
        gReplacements.clear();
        gNamesSeen.clear();
        gGameByReplacement.clear();
    }
    wstring base = config().TEXTURES_FOLDER;
    if (!(base.size() > 1 && base[1] == L':'))
        base = PES::GameDirPath(base);
    if (GetFileAttributesW(base.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    vector<wstring> folders;
    if (language >= 0 && size_t(language) < strlen(gAddr.languageLetters))
        folders.push_back(base + L"\\" + wchar_t(gAddr.languageLetters[language]));
    folders.push_back(base);
    ForEachNamedTexture([&](int id, char const *name) {
        if (!gNamesSeen.insert(name).second)
            return;
        wstring file = wstring(name, name + strlen(name));
        for (wstring const &folder : folders) {
            for (wchar_t const *ext : { L".dds", L".png" }) {
                wstring path = folder + L"\\" + file + ext;
                if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    Replacement *r = new Replacement;
                    r->id = id;
                    r->path = path;
                    gReplacements.push_back(r);
                    return;
                }
            }
        }
    });
}

// The D3D textures the game draws a replaced texture with: its own (also with an upload pending: the game
// uploads it before drawing, mostly into the same D3D texture), those of its copies, and pair cache entries.
void MapReplacements() {
    gReplacementByGame.clear();
    if (gReplacements.empty())
        return;
    unordered_map<unsigned char *, Replacement *> byPlatform;
    unordered_map<int, Replacement *> byId;
    auto add = [&](unsigned char *pt, Replacement *r) {
        byPlatform[pt] = r;
        if (void *d3d = *reinterpret_cast<void **>(pt + 8))
            gReplacementByGame[d3d] = r;
    };
    for (Replacement *r : gReplacements) {
        byId[r->id] = r;
        if (unsigned char *pt = FindPlatformTexture(r->id))
            add(pt, r);
    }
    AcquireSRWLockShared(&gCopiesLock);
    for (auto const &copy : gCopies) {
        int source = copy.second.source;
        for (int depth = 0; depth < 4 && !byId.count(source); depth++) {
            auto it = gCopies.find(source);
            if (it == gCopies.end())
                break;
            source = it->second.source;
        }
        auto r = byId.find(source);
        if (r == byId.end())
            continue;
        if (unsigned char *pt = FindPlatformTexture(copy.first))
            add(pt, r->second);
    }
    ReleaseSRWLockShared(&gCopiesLock);
    if (byPlatform.empty())
        return;
    ForEachTexturePair([&](unsigned char *image, void *d3d) {
        auto it = byPlatform.find(image);
        if (it != byPlatform.end())
            gReplacementByGame[d3d] = it->second;
    });
}

// Half size, averaging 2x2 texels (colour weighted by alpha, so transparent texels do not darken edges).
void HalveArgb(vector<unsigned> const &src, int w, int h, vector<unsigned> &dst, int &nw, int &nh) {
    nw = max(1, w / 2);
    nh = max(1, h / 2);
    dst.assign(size_t(nw) * nh, 0);
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            unsigned a = 0, r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    unsigned c = src[size_t(min(2 * y + dy, h - 1)) * w + min(2 * x + dx, w - 1)];
                    unsigned ca = c >> 24;
                    a += ca;
                    r += ((c >> 16) & 0xFF) * ca;
                    g += ((c >> 8) & 0xFF) * ca;
                    b += (c & 0xFF) * ca;
                }
            }
            dst[size_t(y) * nw + x] = a ? (((a + 2) / 4) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a) : 0;
        }
    }
}

// PNG: a managed A8R8G8B8 texture with all mip levels.
void *CreateTextureFromPng(void *dev, wstring const &path) {
    vector<unsigned char> data;
    if (!ReadWholeFile(path, data))
        return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(data.data(), int(data.size()), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        LOG(LOG_LEVEL_1, "cannot read %S", path.c_str());
        if (px)
            stbi_image_free(px);
        return nullptr;
    }
    vector<unsigned> level(size_t(w) * h), next;
    for (size_t i = 0; i < level.size(); i++) {
        unsigned char const *p = px + i * 4;
        level[i] = (unsigned(p[3]) << 24) | (unsigned(p[0]) << 16) | (unsigned(p[1]) << 8) | p[2];
    }
    stbi_image_free(px);
    unsigned levels = 1;
    for (int s = max(w, h); s > 1; s >>= 1)
        levels++;
    void *tex = nullptr;
    if (D3D8Call(dev, D3D8_CreateTexture, unsigned(w), unsigned(h), levels, 0u, 21u /* A8R8G8B8 */, 1u /* MANAGED */, &tex) < 0 || !tex) {
        LOG(LOG_LEVEL_1, "cannot create a %dx%d texture for %S", w, h, path.c_str());
        return nullptr;
    }
    int lw = w, lh = h;
    for (unsigned l = 0; l < levels; l++) {
        D3DLockedRect lr;
        if (D3D8Call(tex, D3D8Tex_LockRect, l, &lr, (RECT *)nullptr, 0u) >= 0) {
            for (int row = 0; row < lh; row++)
                memcpy((unsigned char *)lr.bits + row * lr.pitch, &level[size_t(row) * lw], size_t(lw) * 4);
            D3D8Call(tex, D3D8Tex_UnlockRect, l);
        }
        if (l + 1 < levels) {
            int nw, nh;
            HalveArgb(level, lw, lh, next, nw, nh);
            level.swap(next);
            lw = nw;
            lh = nh;
        }
    }
    return tex;
}

// DDS: DXT1, DXT3, DXT5 or 32-bit (A8R8G8B8 / X8R8G8B8), with the mip levels the file has, copied as they are.
void *CreateTextureFromDds(void *dev, wstring const &path) {
    vector<unsigned char> data;
    if (!ReadWholeFile(path, data))
        return nullptr;
    auto u32 = [&](size_t o) { return o + 4 <= data.size() ? *reinterpret_cast<unsigned const *>(&data[o]) : 0u; };
    if (data.size() < 128 || u32(0) != 0x20534444 /* "DDS " */) {
        LOG(LOG_LEVEL_1, "not a DDS file: %S", path.c_str());
        return nullptr;
    }
    unsigned h = u32(12), w = u32(16), mips = (u32(8) & 0x20000) ? u32(28) : 1;   // DDSD_MIPMAPCOUNT
    unsigned pfFlags = u32(80), fourCC = u32(84), bits = u32(88), rMask = u32(92), aMask = u32(104);
    unsigned format = 0, blockBytes = 0;
    if (pfFlags & 4) {                                      // DDPF_FOURCC
        if (fourCC == 0x31545844)                           // DXT1
            format = fourCC, blockBytes = 8;
        else if (fourCC == 0x33545844 || fourCC == 0x35545844)   // DXT3, DXT5
            format = fourCC, blockBytes = 16;
    }
    else if (bits == 32 && rMask == 0xFF0000)
        format = (pfFlags & 1) && aMask == 0xFF000000 ? 21u /* A8R8G8B8 */ : 22u /* X8R8G8B8 */;
    if (!format || !w || !h) {
        LOG(LOG_LEVEL_1, "unsupported DDS format (DXT1, DXT3, DXT5 or 32-bit ARGB): %S", path.c_str());
        return nullptr;
    }
    mips = max(1u, mips);
    void *tex = nullptr;
    if (D3D8Call(dev, D3D8_CreateTexture, w, h, mips, 0u, format, 1u /* MANAGED */, &tex) < 0 || !tex) {
        LOG(LOG_LEVEL_1, "cannot create a %ux%u texture for %S", w, h, path.c_str());
        return nullptr;
    }
    size_t offset = 128;
    unsigned lw = w, lh = h;
    for (unsigned l = 0; l < mips; l++) {
        unsigned rows = blockBytes ? max(1u, (lh + 3) / 4) : lh;
        size_t rowBytes = blockBytes ? size_t(max(1u, (lw + 3) / 4)) * blockBytes : size_t(lw) * 4;
        if (offset + rowBytes * rows > data.size())
            break;
        D3DLockedRect lr;
        if (D3D8Call(tex, D3D8Tex_LockRect, l, &lr, (RECT *)nullptr, 0u) >= 0) {
            for (unsigned row = 0; row < rows; row++)
                memcpy((unsigned char *)lr.bits + row * lr.pitch, &data[offset + row * rowBytes], rowBytes);
            D3D8Call(tex, D3D8Tex_UnlockRect, l);
        }
        offset += rowBytes * rows;
        lw = max(1u, lw / 2);
        lh = max(1u, lh / 2);
    }
    return tex;
}

void *ReplacementTexture(void *dev, void *gameTexture) {
    if (config().TEXTURES_LOG && !gReplacementByGame.count(gameTexture))
        LogUnreplaced(gameTexture);
    if (gReplacements.empty())
        return nullptr;
    auto it = gReplacementByGame.find(gameTexture);
    if (it == gReplacementByGame.end()) {
        // a D3D texture made since Present (texture uploaded, copied or paired with a palette during the frame)
        if (gRemaps >= 16 || !gCheckedThisFrame.insert(gameTexture).second)
            return nullptr;
        gRemaps++;
        MapReplacements();
        it = gReplacementByGame.find(gameTexture);
        if (it == gReplacementByGame.end())
            return nullptr;
    }
    Replacement *r = it->second;
    if (r->device != dev) {
        // a new device: the old one's textures are gone with it
        r->device = dev;
        r->texture = nullptr;
        r->failed = false;
    }
    if (!r->texture && !r->failed) {
        bool dds = r->path.size() > 4 && !_wcsicmp(r->path.c_str() + r->path.size() - 4, L".dds");
        r->texture = dds ? CreateTextureFromDds(dev, r->path) : CreateTextureFromPng(dev, r->path);
        r->failed = !r->texture;
    }
    if (r->texture)
        gGameByReplacement[r->texture] = gameTexture;
    return r->texture;
}

// ---------------------------------------------------------------------------------------------
// device hooks (vtable entries, chained to whatever was there: PesFontHD hooks the same vtable)

static void *oPresent = nullptr, *oSetTexture = nullptr;
static uintptr_t oDeviceCreate = 0;

long __stdcall OnPresent(void *dev, void const *a, void const *b, void *c, void const *d) {
    static unsigned frame = 0;
    if (frame++ % 60 == 0)
        ScanNames(dev);         // name tables are added as modes load
    gCheckedThisFrame.clear();
    gRemaps = 0;
    MapReplacements();
    if (config().TEXTURES_LOG)
        MapNames();
    return reinterpret_cast<long(__stdcall *)(void *, void const *, void const *, void *, void const *)>(oPresent)(dev, a, b, c, d);
}

long __stdcall OnSetTexture(void *dev, unsigned stage, void *tex) {
    if (tex) {
        if (void *replacement = ReplacementTexture(dev, tex))
            tex = replacement;
    }
    return reinterpret_cast<long(__stdcall *)(void *, unsigned, void *)>(oSetTexture)(dev, stage, tex);
}

// The vtable is d3d8's, shared by every device: hooked once (for a later device it may already be hooked
// again by another plugin on top of ours).
void HookVtableEntry(void **vt, int index, void *hook, void **orig) {
    if (*orig || vt[index] == hook)
        return;
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old))
        return;
    *orig = vt[index];
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
}

void __cdecl OnDeviceCreate() {
    CallDynGlobal(oDeviceCreate);
    if (void *dev = *reinterpret_cast<void **>(gAddr.device)) {
        void **vt = *reinterpret_cast<void ***>(dev);
        HookVtableEntry(vt, D3D8_Present, (void *)OnPresent, &oPresent);
        HookVtableEntry(vt, D3D8_SetTexture, (void *)OnSetTexture, &oSetTexture);
    }
}

void Install(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        gAddr = { 0xDB4D80, 0xF75B60, 0xF77878, 0xFE5E4C, 0xFE6234 + 4, "efgisj", 0x40E921, 0x5A4895 };
        break;
    case ID_PES6:
        gAddr = { 0xF83E68, 0xF84350, 0xF86098, 0x3A67F50, 0x1030ED8 + 4, "efgisp", 0x405678, 0x9546C5 };
        break;
    default:
        return;
    }
    if (patch::GetBytes(gAddr.deviceCreateCall, 1) != "E8" || patch::GetBytes(gAddr.textureCopyCall, 1) != "E8") {
        LOG(LOG_LEVEL_1, "texture replacement: unexpected code, not installed");
        return;
    }
    oDeviceCreate = patch::RedirectCall(gAddr.deviceCreateCall, OnDeviceCreate);
    oTextureCopy = patch::RedirectCall(gAddr.textureCopyCall, OnTextureCopy);
}

}

// For other plugins (PesFontHD): the game texture a texture bound in its place stands for, else the texture.
extern "C" __declspec(dllexport) void *PesAssetLoaderGameTexture(void *texture) {
    auto it = TextureReplacement::gGameByReplacement.find(texture);
    return it != TextureReplacement::gGameByReplacement.end() ? it->second : texture;
}

void InitTextureReplacement(PES::Version v) {
    if (config().TEXTURES)
        TextureReplacement::Install(v);
}
