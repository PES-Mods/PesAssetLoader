#include "FlagReplacer.hpp"
#include <map>
#include <deque>
#include <xmmintrin.h>
#include "stb_image.h"

// Country flags, club badges and patterns from PNG / DDS files ([FLAGS])
//
// The game keeps the flag files (.flg, one per size: PES5 22x15, 32x32, 64x48, 64x64; PES6 32x32, 64x64) in
// memory and makes a texture for every flag it shows: FlagTextureCreate(file, flag id, folder) creates a paletted
// texture of the file's raster size (e.g. 32x16 for 22x15) with the folder's palette and the flag in its top-left
// corner; the screens draw only that corner. A flag with a file FLAGS_FOLDER\folder_<n>\<flag id>.png|.dds
// (<flag id>@<w>x<h>: only for the file of that size) gets a 32-bit texture with the same layout, FLAGS_SCALE
// times bigger, with its own colors and mip levels. The picture keeps its proportions and is centered in the
// flag's area; flags and badges are made FLAGS_ASPECT times as wide (1: as they are; the game's own pictures are
// about 0.87 times as wide as they look on the screen), patterns are not.
// Folders of the .flg files: 0 country flags, 1 club badges (and some special ones), 2 patterns.
namespace FlagReplacer {

using namespace TextureLimits;

// folders 0..2: the .flg folders; per team: 3, 4: emblems (clubs, national teams), 5: team flags (the waving flags),
// 6: big flags
static char const *const kFlagFolders[] = { "folder_0", "folder_1", "folder_2", "clubs", "countries", "team_flags",
    "big_flags" };

struct FlagPicture {
    vector<uint32_t> pixels;    // A8R8G8B8, the whole raster
    unsigned int width = 0, height = 0;
    uint64_t lastUse = 0;
};

// (folder, flag id, width, height: 0 for any size) -> file
static map<uint64_t, wstring> &FlagFiles() {
    static map<uint64_t, wstring> files;
    return files;
}

// made pictures, by (folder, flag id, size of the flag file)
static map<uint64_t, FlagPicture> &FlagPictures() {
    static map<uint64_t, FlagPicture> pictures;
    return pictures;
}

constinit static size_t gFlagPicturesSize = 0;
constinit static uint64_t gFlagClock = 0;
constinit static unsigned int gFlagScale = 4;
constinit static float gFlagAspect = 1.0f;
constinit static uintptr_t gFlagTexRefTexture = 0;    // offset of Texture * in the game's texture reference
constinit static bool gFlagMips = true;
constinit static size_t gFlagCacheLimit = 96u << 20;
static uintptr_t oFlagTextureCreate;

static uint64_t FlagKey(unsigned int folder, unsigned int id, unsigned int width, unsigned int height) {
    return ((uint64_t)folder << 48) | ((uint64_t)(id & 0xFFFF) << 32) | ((width & 0xFFFF) << 16) | (height & 0xFFFF);
}

static bool IsDdsPath(wstring const &path) {
    return path.size() > 4 && !_wcsicmp(path.c_str() + path.size() - 4, L".dds");
}

// "<id>", "<id>@<w>x<h>" (+ .png / .dds)
static bool ParseFlagFileName(wstring const &name, unsigned int &id, unsigned int &width, unsigned int &height) {
    size_t dot = name.rfind(L'.');
    if (dot == wstring::npos)
        return false;
    wstring ext = name.substr(dot);
    if (_wcsicmp(ext.c_str(), L".png") && _wcsicmp(ext.c_str(), L".dds"))
        return false;
    wstring stem = name.substr(0, dot);
    width = height = 0;
    size_t at = stem.find(L'@');
    if (at != wstring::npos) {
        if (swscanf(stem.c_str() + at + 1, L"%ux%u", &width, &height) != 2 || !width || !height)
            return false;
        stem.resize(at);
    }
    if (stem.empty() || stem.size() > 5 || stem.find_first_not_of(L"0123456789") != wstring::npos)
        return false;
    id = (unsigned int)wcstoul(stem.c_str(), nullptr, 10);
    return id <= 0xFFFF;
}

// team_flags, big_flags: "<team id>" (variation 0), "<team id>_<n>", "<team id> (<n>)", "<team id>(<n>)"
static bool ParseTeamFlagFileName(wstring const &name, unsigned int &team, unsigned int &variation) {
    size_t dot = name.rfind(L'.');
    if (dot == wstring::npos)
        return false;
    wstring ext = name.substr(dot);
    if (_wcsicmp(ext.c_str(), L".png") && _wcsicmp(ext.c_str(), L".dds"))
        return false;
    wstring stem = name.substr(0, dot);
    size_t digits = stem.find_first_not_of(L"0123456789");
    wstring id = stem.substr(0, digits);
    if (id.empty() || id.size() > 5)
        return false;
    team = (unsigned int)wcstoul(id.c_str(), nullptr, 10);
    variation = 0;
    if (digits != wstring::npos) {
        wstring rest = stem.substr(digits);
        unsigned int n = 0;
        wchar_t tail[8] = {};
        if (swscanf(rest.c_str(), L"_%u%7ls", &n, tail) == 1 || swscanf(rest.c_str(), L" (%u)%7ls", &n, tail) == 1
            || swscanf(rest.c_str(), L"(%u)%7ls", &n, tail) == 1)
            variation = n;
        else
            return false;
    }
    return team <= 0xFFFF && variation <= 0xFFFF;
}

static wstring FlagsFolderPath() {
    wstring base = config().FLAGS_FOLDER;
    if (!(base.size() > 1 && base[1] == L':'))
        base = PES::GameDirPath(base);
    return base;
}

static void ScanFlagFiles() {
    wstring base = FlagsFolderPath();
    for (unsigned int folder = 0; folder < 7; folder++) {
        wstring dir = base + L"\\" + wstring(kFlagFolders[folder], kFlagFolders[folder] + strlen(kFlagFolders[folder]));
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            unsigned int id, width, height;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (folder >= 5) {
                // team flags: the variation in the place of the size
                if (!ParseTeamFlagFileName(fd.cFileName, id, height))
                    continue;
                width = 0;
            }
            else if (!ParseFlagFileName(fd.cFileName, id, width, height))
                continue;
            wstring path = dir + L"\\" + fd.cFileName;
            uint64_t key = FlagKey(folder, id, width, height);
            auto it = FlagFiles().find(key);
            // DDS before PNG (faster to load)
            bool dds = IsDdsPath(path);
            if (it == FlagFiles().end() || dds)
                FlagFiles()[key] = path;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

static bool ReadWholeFile(wstring const &path, vector<uint8_t> &out) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    bool ok = size > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// A DDS file: DXT1, DXT3, DXT5 or 32-bit (A8R8G8B8 / X8R8G8B8), with its mip levels
struct DdsInfo {
    unsigned int width = 0, height = 0, levels = 1;
    uint8_t format = 0;         // platform format: 1, 3, 5 (DXT) or PLATFORM_ARGB8888
    bool opaque = false;        // X8R8G8B8
};

static size_t DdsLevelSize(uint8_t format, unsigned int width, unsigned int height) {
    if (format == PLATFORM_ARGB8888)
        return (size_t)width * height * 4;
    return (size_t)max(1u, (width + 3) / 4) * max(1u, (height + 3) / 4) * (format == 1 ? 8 : 16);
}

static bool ParseDds(vector<uint8_t> const &file, DdsInfo &info) {
    if (file.size() < 128 || memcmp(file.data(), "DDS ", 4))
        return false;
    auto u32 = [&](size_t offset) { return *(uint32_t const *)(file.data() + offset); };
    info.height = u32(12);
    info.width = u32(16);
    info.levels = (u32(8) & 0x20000) ? max(1u, u32(28)) : 1;      // DDSD_MIPMAPCOUNT
    uint32_t pfFlags = u32(80), fourCC = u32(84), bits = u32(88);
    if (pfFlags & 4) {      // DDPF_FOURCC
        if (fourCC == 0x31545844)
            info.format = 1;
        else if (fourCC == 0x33545844)
            info.format = 3;
        else if (fourCC == 0x35545844)
            info.format = 5;
        else
            return false;
    }
    else if (bits == 32 && u32(92) == 0xFF0000 && u32(96) == 0xFF00 && u32(100) == 0xFF) {
        info.format = PLATFORM_ARGB8888;
        info.opaque = !((pfFlags & 1) && u32(104) == 0xFF000000);
    }
    else
        return false;
    if (!info.width || !info.height || info.width > 8192 || info.height > 8192)
        return false;
    // only the levels that are in the file
    size_t size = 128;
    unsigned int levels = 0;
    for (; levels < info.levels; levels++) {
        size_t next = size + DdsLevelSize(info.format, max(1u, info.width >> levels), max(1u, info.height >> levels));
        if (next > file.size())
            break;
        size = next;
    }
    info.levels = levels;
    return levels > 0;
}

// Level 0 of a DDS file as A8R8G8B8 pixels
static bool DecodeDds(vector<uint8_t> const &file, vector<uint32_t> &pixels, unsigned int &width, unsigned int &height) {
    DdsInfo info;
    if (!ParseDds(file, info))
        return false;
    width = info.width;
    height = info.height;
    size_t size = DdsLevelSize(info.format, width, height);
    // a texture header in front of the texels, for DecodeTexture
    vector<uint8_t> buffer(sizeof(Texture) + size);
    auto t = (Texture *)buffer.data();
    t->width = (uint16_t)width;
    t->height = (uint16_t)height;
    t->texelsOffset = (uint16_t)sizeof(Texture);
    t->platformFormat = info.format;
    memcpy(buffer.data() + sizeof(Texture), file.data() + 128, size);
    pixels = DecodeTexture(t, false);
    if (info.opaque) {
        for (uint32_t &p : pixels)
            p |= 0xFF000000;
    }
    return !pixels.empty();
}

// The picture of a PNG / DDS file (its data) as A8R8G8B8 pixels
static bool DecodeImage(wstring const &path, vector<uint8_t> const &file, vector<uint32_t> &pixels,
    unsigned int &width, unsigned int &height)
{
    if (IsDdsPath(path))
        return DecodeDds(file, pixels, width, height);
    int w, h, n;
    unsigned char *rgba = stbi_load_from_memory(file.data(), (int)file.size(), &w, &h, &n, 4);
    if (!rgba)
        return false;
    width = (unsigned int)w;
    height = (unsigned int)h;
    pixels.resize((size_t)w * h);
    for (size_t i = 0; i < pixels.size(); i++) {
        unsigned char const *p = rgba + i * 4;
        pixels[i] = ((uint32_t)p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2];
    }
    stbi_image_free(rgba);
    return true;
}

static bool LoadFlagImage(wstring const &path, vector<uint32_t> &pixels, unsigned int &width, unsigned int &height) {
    vector<uint8_t> file;
    return ReadWholeFile(path, file) && DecodeImage(path, file, pixels, width, height);
}

// Resampling filters (FLAGS_FILTER): box (the area under every texel), bilinear, bicubic (Catmull-Rom),
// lanczos (Lanczos 3: the sharpest)
enum FlagFilter { FILTER_BOX, FILTER_BILINEAR, FILTER_BICUBIC, FILTER_LANCZOS };

static double FilterSupport(FlagFilter filter) {
    switch (filter) {
    case FILTER_BOX: return 0.5;
    case FILTER_BILINEAR: return 1.0;
    case FILTER_BICUBIC: return 2.0;
    default: return 3.0;
    }
}

static double FilterKernel(FlagFilter filter, double x) {
    x = fabs(x);
    switch (filter) {
    case FILTER_BOX:
        return x <= 0.5 ? 1.0 : 0.0;
    case FILTER_BILINEAR:
        return x < 1.0 ? 1.0 - x : 0.0;
    case FILTER_BICUBIC: {
        // Catmull-Rom (a = -0.5)
        const double a = -0.5;
        if (x < 1.0)
            return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
        if (x < 2.0)
            return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
        return 0.0;
    }
    default: {
        if (x < 1e-8)
            return 1.0;
        if (x >= 3.0)
            return 0.0;
        const double pi = 3.14159265358979323846;
        return 3.0 * sin(pi * x) * sin(pi * x / 3.0) / (pi * pi * x * x);
    }
    }
}

struct ResampleTaps {
    vector<unsigned int> first, count;  // per destination texel: the first source texel and how many
    vector<float> weights;              // 'count' weights per destination texel, 'stride' apart
    unsigned int stride = 0;
};

// Weights of the source texels for every destination texel along one axis; when shrinking, the filter is
// widened to the destination texel (so every source texel counts: no aliasing)
static void ResampleWeights(unsigned int src, unsigned int dst, FlagFilter filter, ResampleTaps &out) {
    double scale = (double)src / dst;
    double filterScale = max(scale, 1.0);
    double support = FilterSupport(filter) * filterScale;
    out.stride = (unsigned int)ceil(support * 2.0) + 2;
    out.first.assign(dst, 0);
    out.count.assign(dst, 0);
    out.weights.assign((size_t)dst * out.stride, 0.0f);
    vector<double> w(out.stride);
    for (unsigned int i = 0; i < dst; i++) {
        double center = (i + 0.5) * scale;
        int from = max(0, (int)floor(center - support));
        int to = min((int)src, (int)ceil(center + support));
        unsigned int n = 0;
        double sum = 0.0;
        for (int s = from; s < to && n < out.stride; s++, n++) {
            w[n] = FilterKernel(filter, (s + 0.5 - center) / filterScale);
            sum += w[n];
        }
        if (sum == 0.0) {
            // the box filter between two texels when growing: the nearest one
            from = std::clamp((int)center, 0, (int)src - 1);
            n = 1;
            w[0] = sum = 1.0;
        }
        out.first[i] = (unsigned int)from;
        out.count[i] = n;
        for (unsigned int k = 0; k < n; k++)
            out.weights[(size_t)i * out.stride + k] = (float)(w[k] / sum);
    }
}

constinit static FlagFilter gFlagFilter = FILTER_LANCZOS;

// Scratch buffers of the resizing, kept: fresh big blocks cost a page fault per page
static vector<float> &ResizeScratch(int i) {
    static vector<float> buffers[2];
    return buffers[i];
}

// Resize A8R8G8B8 pixels (premultiplied while filtering, so transparent texels don't darken the edges)
static vector<uint32_t> ResizeArgb(vector<uint32_t> const &src, unsigned int sw, unsigned int sh, unsigned int dw,
    unsigned int dh, FlagFilter filter = gFlagFilter)
{
    if (sw == dw && sh == dh)
        return src;
    ResampleTaps tx, ty;
    ResampleWeights(sw, dw, filter, tx);
    ResampleWeights(sh, dh, filter, ty);
    // premultiplied source
    vector<float> &pre = ResizeScratch(0);
    pre.resize((size_t)sw * sh * 4);
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        uint32_t c = src[i];
        float a = (c >> 24) / 255.0f;
        pre[i * 4 + 0] = a * ((c >> 16) & 0xFF);
        pre[i * 4 + 1] = a * ((c >> 8) & 0xFF);
        pre[i * 4 + 2] = a * (c & 0xFF);
        pre[i * 4 + 3] = a;
    }
    // horizontally
    vector<float> &rows = ResizeScratch(1);
    rows.resize((size_t)dw * sh * 4);
    for (unsigned int y = 0; y < sh; y++) {
        float const *line = &pre[(size_t)y * sw * 4];
        for (unsigned int x = 0; x < dw; x++) {
            // a texel is 4 floats: one SSE register
            __m128 acc = _mm_setzero_ps();
            float const *w = &tx.weights[(size_t)x * tx.stride];
            float const *p = line + (size_t)tx.first[x] * 4;
            for (unsigned int k = 0; k < tx.count[x]; k++, p += 4)
                acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(w[k]), _mm_loadu_ps(p)));
            _mm_storeu_ps(&rows[((size_t)y * dw + x) * 4], acc);
        }
    }
    // vertically
    vector<uint32_t> out((size_t)dw * dh);
    for (unsigned int y = 0; y < dh; y++) {
        float const *w = &ty.weights[(size_t)y * ty.stride];
        for (unsigned int x = 0; x < dw; x++) {
            __m128 sum = _mm_setzero_ps();
            float const *p = &rows[((size_t)ty.first[y] * dw + x) * 4];
            for (unsigned int k = 0; k < ty.count[y]; k++, p += (size_t)dw * 4)
                sum = _mm_add_ps(sum, _mm_mul_ps(_mm_set1_ps(w[k]), _mm_loadu_ps(p)));
            float acc[4];
            _mm_storeu_ps(acc, sum);
            // the sharper filters overshoot: clamp
            float alpha = std::clamp(acc[3], 0.0f, 1.0f);
            uint32_t color = 0;
            if (alpha > 1e-6f) {
                float inv = 1.0f / alpha;
                for (int c = 0; c < 3; c++)
                    color = (color << 8) | (uint32_t)std::clamp((int)(acc[c] * inv + 0.5f), 0, 255);
            }
            out[(size_t)y * dw + x] = ((uint32_t)(alpha * 255.0f + 0.5f) << 24) | color;
        }
    }
    return out;
}

// Mip levels 'from'..n of a 32-bit texture, each from the one above with FLAGS_FILTER (sharper than averaging
// 2x2 texels, which is what the box filter does)
static void BuildFlagMips(Texture *t, unsigned int from = 1) {
    auto texels = (uint32_t *)Texels(t);
    unsigned int w = t->width, h = t->height;
    for (unsigned int i = 1; i < from; i++) {
        texels += (size_t)w * h;
        w = max(1u, w / 2);
        h = max(1u, h / 2);
    }
    vector<uint32_t> level(texels, texels + (size_t)w * h);
    uint32_t *dst = texels + (size_t)w * h;
    for (unsigned int i = from; i < ((t->vramPages_levels >> 12) + 1u); i++) {
        unsigned int nw = max(1u, w / 2), nh = max(1u, h / 2);
        level = ResizeArgb(level, w, h, nw, nh);
        memcpy(dst, level.data(), level.size() * 4);
        dst += level.size();
        w = nw;
        h = nh;
    }
}

// A DDS file that has the size of the texture (or a mip level of that size) is used as it is: its picture and mip
// levels, in its format (DXT or 32-bit), nothing is resized. Mip levels missing in a 32-bit file are made here.
// The renderer finds the levels by their size in 16-byte units, each a quarter of the one above (rounded up), so
// a DXT texture keeps only the levels where that holds (DXT1 down to 8x8, DXT3/5 down to 4x4).
static Texture *DdsTexture(Texture *owner, vector<uint8_t> const &file, unsigned int width, unsigned int height,
    bool mips)
{
    DdsInfo info;
    if (!ParseDds(file, info))
        return nullptr;
    unsigned int first = 0;
    size_t offset = 128;
    while (first < info.levels && (max(1u, info.width >> first) != width || max(1u, info.height >> first) != height)) {
        offset += DdsLevelSize(info.format, max(1u, info.width >> first), max(1u, info.height >> first));
        first++;
    }
    if (first == info.levels)
        return nullptr;
    unsigned int available = mips ? info.levels - first : 1;
    if (info.format == PLATFORM_ARGB8888) {
        Texture *t = CreateArgbTexture(owner, width, height);
        if (!t)
            return nullptr;
        unsigned int levels = (t->vramPages_levels >> 12) + 1;
        if (!mips) {
            t->vramPages_levels &= 0x0FFF;
            levels = 1;
        }
        unsigned int copied = min(available, levels);
        auto dst = (uint32_t *)Texels(t);
        auto src = (uint32_t const *)(file.data() + offset);
        size_t count = 0;
        for (unsigned int i = 0; i < copied; i++)
            count += (size_t)max(1u, width >> i) * max(1u, height >> i);
        memcpy(dst, src, count * 4);
        if (info.opaque) {
            for (size_t i = 0; i < count; i++)
                dst[i] |= 0xFF000000;
        }
        if (copied < levels)
            BuildFlagMips(t, copied);
        return t;
    }
    // DXT: the levels whose size the renderer computes right
    size_t qwc = DdsLevelSize(info.format, width, height) / 16;
    if (!qwc || DdsLevelSize(info.format, width, height) % 16)
        return nullptr;
    unsigned int levels = 0;
    size_t total = 0;
    for (size_t q = qwc; levels < available; levels++, q = (q + 3) / 4) {
        size_t size = DdsLevelSize(info.format, max(1u, width >> levels), max(1u, height >> levels));
        if (size != q * 16)
            break;
        total += size;
    }
    size_t size = sizeof(Texture) + total;
    auto t = (Texture *)_aligned_malloc(size, 16);
    if (!t)
        return nullptr;
    memcpy(t, owner, sizeof(Texture));
    memcpy((uint8_t *)t + sizeof(Texture), file.data() + offset, total);
    t->totalSize = (uint32_t)size;
    t->texelsOffset = (uint16_t)sizeof(Texture);
    t->clutOffset = 0;
    t->hasClut = 0;
    t->clutSizeQwc = 0;
    t->texelFormat = 0;
    t->platformFormat = info.format;
    t->platformFlags = 0;
    t->width = (uint16_t)width;
    t->height = (uint16_t)height;
    unsigned int log2w = 0, log2h = 0;
    while ((1u << log2w) < width)
        log2w++;
    while ((1u << log2h) < height)
        log2h++;
    t->log2Width = (uint8_t)log2w;
    t->log2Height = (uint8_t)log2h;
    t->copyBufferWidth = t->width;
    t->copyBufferHeight = t->height;
    t->texelsSizeQwc = (uint32_t)qwc;
    t->vramPages_levels = (uint16_t)((levels - 1) << 12);
    memoryStats().textureCopiesSize += size;
    memoryStats().textureCopies++;
    return t;
}

static Texture *DdsTexture(Texture *owner, wstring const &path, unsigned int width, unsigned int height, bool mips) {
    vector<uint8_t> file;
    if (!ReadWholeFile(path, file))
        return nullptr;
    return DdsTexture(owner, file, width, height, mips);
}

// The last decoded files: the game makes the emblems of both sizes one after the other
struct DecodedImage {
    wstring path;
    vector<uint32_t> pixels;
    unsigned int width = 0, height = 0;
};

static deque<DecodedImage> &DecodedImages() {
    static deque<DecodedImage> images;
    return images;
}

static bool LoadFlagImageCached(wstring const &path, vector<uint32_t> &pixels, unsigned int &width, unsigned int &height) {
    for (DecodedImage const &d : DecodedImages()) {
        if (d.path == path) {
            pixels = d.pixels;
            width = d.width;
            height = d.height;
            return true;
        }
    }
    if (!LoadFlagImage(path, pixels, width, height))
        return false;
    DecodedImages().push_back({ path, pixels, width, height });
    if (DecodedImages().size() > 4)
        DecodedImages().pop_front();
    return true;
}

// Transparent texels take the color of the nearest visible one: filtering and mip levels blend them in at the
// edges of the picture (instead of black)
static void BleedArgb(vector<uint32_t> &pixels, unsigned int width, unsigned int height) {
    vector<uint8_t> known(pixels.size());
    vector<uint32_t> queue;
    queue.reserve(pixels.size());
    for (size_t i = 0; i < pixels.size(); i++) {
        if (pixels[i] >> 24) {
            known[i] = 1;
            queue.push_back((uint32_t)i);
        }
    }
    if (queue.empty() || queue.size() == pixels.size())
        return;
    for (size_t head = 0; head < queue.size(); head++) {
        uint32_t i = queue[head];
        int x = (int)(i % width), y = (int)(i / width);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if ((dx || dy) && nx >= 0 && ny >= 0 && nx < (int)width && ny < (int)height) {
                    uint32_t j = (uint32_t)ny * width + nx;
                    if (!known[j]) {
                        known[j] = 1;
                        pixels[j] = (pixels[j] & 0xFF000000) | (pixels[i] & 0xFFFFFF);
                        queue.push_back(j);
                    }
                }
            }
        }
    }
}

// The picture of a flag file at the layout of the game's flag (w x h in the top-left corner of a raster),
// FLAGS_SCALE times bigger. Kept in memory (up to FLAGS_CACHE_MB): some screens make all the emblems at once,
// again and again.
static FlagPicture const *GetFlagPicture(uint64_t key, wstring const &path, unsigned int folder, unsigned int w,
    unsigned int h, unsigned int rasterWidth, unsigned int rasterHeight)
{
    auto it = FlagPictures().find(key);
    if (it != FlagPictures().end()) {
        it->second.lastUse = ++gFlagClock;
        return &it->second;
    }
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    vector<uint32_t> image;
    unsigned int iw, ih;
    if (!LoadFlagImageCached(path, image, iw, ih)) {
        LOG(LOG_LEVEL_1, "[Flags] can't read %S (PNG, or DDS: DXT1/DXT3/DXT5/32-bit)", path.c_str());
        return nullptr;
    }
    unsigned int k = gFlagScale;
    unsigned int areaWidth = w * k, areaHeight = h * k;
    // fit by the side that limits it, keeping the proportions (flags and badges: the width times FLAGS_ASPECT;
    // the game's patterns fill their whole area)
    double width = iw * (folder == 2 ? 1.0 : (double)gFlagAspect), height = ih;
    double s = min(areaWidth / width, areaHeight / height);
    unsigned int dw = (unsigned int)std::clamp(lround(width * s), 1L, (long)areaWidth);
    unsigned int dh = (unsigned int)std::clamp(lround(height * s), 1L, (long)areaHeight);
    vector<uint32_t> resized = ResizeArgb(image, iw, ih, dw, dh);
    FlagPicture picture;
    picture.width = rasterWidth * k;
    picture.height = rasterHeight * k;
    picture.pixels.assign((size_t)picture.width * picture.height, 0);
    unsigned int ox = (areaWidth - dw) / 2, oy = (areaHeight - dh) / 2;
    for (unsigned int y = 0; y < dh; y++)
        memcpy(&picture.pixels[(size_t)(oy + y) * picture.width + ox], &resized[(size_t)y * dw], dw * 4);
    BleedArgb(picture.pixels, picture.width, picture.height);
    picture.lastUse = ++gFlagClock;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    LOG(LOG_LEVEL_2, "[Flags] %S (%ux%u) -> %ux%u in %ux%u, %.1f ms", path.c_str(), iw, ih, dw, dh, picture.width,
        picture.height, (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
    // forget the least recently used pictures
    gFlagPicturesSize += picture.pixels.size() * 4;
    while (gFlagPicturesSize > gFlagCacheLimit && !FlagPictures().empty()) {
        auto oldest = FlagPictures().begin();
        for (auto i = FlagPictures().begin(); i != FlagPictures().end(); ++i) {
            if (i->second.lastUse < oldest->second.lastUse)
                oldest = i;
        }
        gFlagPicturesSize -= oldest->second.pixels.size() * 4;
        FlagPictures().erase(oldest);
    }
    return &(FlagPictures()[key] = std::move(picture));
}

// --- team emblems: per club / national team ---
// FLAGS_FOLDER\clubs\<team id>.png|.dds and FLAGS_FOLDER\countries\<team id>.png|.dds (national teams) replace the
// emblem of that team wherever the game shows the emblem of a team, whatever emblem it has (Edit mode).
// The game gets a team's emblem id from the team (TeamEmblem: club data, or the flag of the country) and makes
// one texture per emblem, shared by every team with that emblem. Only the calls of the functions that show the
// emblem of a team are redirected (the team's data, Edit mode and the Master League keep the real emblem): for a
// team with a file they get a team emblem id (0x10000 + team). For such an id the emblem dispatcher is called
// with an emblem of the clubs folder, and the flag id it passes to the flag functions is replaced by a team flag
// id (FlagTextureCreate makes a texture of its own for it, which gets the team's picture).
// The team flags (waving flags) of clubs with a file get it as their emblem.
const int kTeamEmblem = 0x10000;
const int kTeamFlagId = 20000;      // + team & 0x1FF: texture ids far from the game's flags (27000...)

struct TeamEmblemAddresses {
    uintptr_t accessors[2];         // int TeamEmblem(uint16 team): two variants (the screens use either)
    uintptr_t calls[2][8];          // their calls in the functions that show the emblem of a team (0: end)
    uintptr_t flagCalls[2];         // the calls of accessors[0] where the team flag is set (Edit Flag, match)
    uintptr_t dispatcherCall;       // EmblemCreate(eax: emblem id, size) in the emblem cache
    uintptr_t flagObjectCall;       // FlagGet(context, file, flag id, folder) in the dispatcher
    uintptr_t flagTexIdCall;        // FlagTextureId(file, flag id, folder) in the dispatcher
    int firstClubEmblem;            // emblem id of the first emblem of the clubs folder
};
constinit static TeamEmblemAddresses gTeamEmblem = {};
constinit static uintptr_t gEmblemDispatcher = 0, gFlagObject = 0, gFlagTexId = 0;
constinit static int gPendingTeamFlagId = -1;
constinit static int gFlagTeam = -1, gFlagTeamEmblem = -1;    // the team of the last team flag set
constinit static uint16_t gTeamOfFlagId[512] = {};          // team flag id - kTeamFlagId -> team

static bool IsNationalTeam(unsigned int team) {
    return team < 64 || team >= 0xFFF0;
}

// The team's file (folder 3: clubs, 4: countries), for the flags of this size (or any size)
static map<uint64_t, wstring>::const_iterator FindTeamFile(unsigned int team, unsigned int w, unsigned int h) {
    unsigned int folder = IsNationalTeam(team) ? 4 : 3;
    auto file = FlagFiles().find(FlagKey(folder, team, w, h));
    if (file == FlagFiles().end())
        file = FlagFiles().find(FlagKey(folder, team, 0, 0));
    return file;
}

static bool HasTeamFile(unsigned int team) {
    unsigned int folder = IsNationalTeam(team) ? 4 : 3;
    auto it = FlagFiles().lower_bound(FlagKey(folder, team, 0, 0));
    return it != FlagFiles().end() && (it->first >> 32) == (FlagKey(folder, team, 0, 0) >> 32);
}

static int TeamEmblemFor(unsigned int team, int emblem) {
    team &= 0xFFFF;
    return HasTeamFile(team) ? kTeamEmblem + (int)team : emblem;
}

static int __cdecl OnTeamEmblem0(unsigned int team) {
    return TeamEmblemFor(team, CallAndReturnDynGlobal<int>(gTeamEmblem.accessors[0], team));
}

static int __cdecl OnTeamEmblem1(unsigned int team) {
    return TeamEmblemFor(team, CallAndReturnDynGlobal<int>(gTeamEmblem.accessors[1], team));
}

// The team flag keeps the real emblem (the game accepts only its own ids there): the team is remembered
static int __cdecl OnTeamFlagEmblem(unsigned int team) {
    int emblem = CallAndReturnDynGlobal<int>(gTeamEmblem.accessors[0], team);
    gFlagTeam = (int)(team & 0xFFFF);
    gFlagTeamEmblem = emblem;
    return emblem;
}

static void *__cdecl OnFlagObject(int context, int file, int flagId, int folder) {
    if (gPendingTeamFlagId >= 0)
        flagId = gPendingTeamFlagId;
    return CallAndReturnDynGlobal<void *>(gFlagObject, context, file, flagId, folder);
}

static int __cdecl OnFlagTexId(int file, int flagId, int folder) {
    if (gPendingTeamFlagId >= 0)
        flagId = gPendingTeamFlagId;
    return CallAndReturnDynGlobal<int>(gFlagTexId, file, flagId, folder);
}

// EmblemCreate takes the emblem id in eax
static uint8_t *CallEmblemDispatcher(int emblem, int emblemSize) {
    uintptr_t fn = gEmblemDispatcher;
    uint8_t *result;
    __asm {
        push emblemSize
        mov eax, emblem
        call fn
        add esp, 4
        mov result, eax
    }
    return result;
}

static uint8_t *__cdecl TeamEmblemCreate(int emblem, int size) {
    unsigned int team = (unsigned int)(emblem - kTeamEmblem) & 0xFFFF;
    int index = (int)(team & 0x1FF);
    gTeamOfFlagId[index] = (uint16_t)team;
    gPendingTeamFlagId = kTeamFlagId + index;
    uint8_t *object = CallEmblemDispatcher(gTeamEmblem.firstClubEmblem, size);
    gPendingTeamFlagId = -1;
    // the emblem cache finds it by its emblem id and size
    if (object)
        *(int *)(object + 0x20) = emblem;
    return object;
}

static __declspec(naked) void EmblemDispatcherStub() {
    __asm {
        cmp eax, kTeamEmblem
        jae team
        jmp dword ptr [gEmblemDispatcher]
    team:
        push dword ptr [esp + 4]
        push eax
        call TeamEmblemCreate
        add esp, 8
        ret
    }
}

static void InstallTeamEmblems(TeamEmblemAddresses const &a) {
    bool any = false;
    for (auto const &f : FlagFiles())
        any = any || (f.first >> 48) >= 3;
    if (!any)
        return;
    // check everything first
    auto callTo = [](uintptr_t at, uintptr_t target) {
        return patch::GetBytes(at, 1) == "E8" && injector::GetBranchDestination(at) == target;
    };
    uintptr_t dispatcher = patch::GetBytes(a.dispatcherCall, 1) == "E8" ? injector::GetBranchDestination(a.dispatcherCall) : 0;
    uintptr_t flagObject = patch::GetBytes(a.flagObjectCall, 1) == "E8" ? injector::GetBranchDestination(a.flagObjectCall) : 0;
    uintptr_t flagTexId = patch::GetBytes(a.flagTexIdCall, 1) == "E8" ? injector::GetBranchDestination(a.flagTexIdCall) : 0;
    bool ok = dispatcher && flagObject && flagTexId;
    for (int i = 0; i < 2 && ok; i++) {
        for (uintptr_t at : a.calls[i])
            ok = ok && (!at || callTo(at, a.accessors[i]));
    }
    for (uintptr_t at : a.flagCalls)
        ok = ok && callTo(at, a.accessors[0]);
    if (!ok) {
        LOG(LOG_LEVEL_1, "[Flags] unexpected code, team emblems are not replaced");
        return;
    }
    gTeamEmblem = a;
    gEmblemDispatcher = dispatcher;
    gFlagObject = flagObject;
    gFlagTexId = flagTexId;
    for (uintptr_t at : a.calls[0]) {
        if (at)
            patch::RedirectCall(at, OnTeamEmblem0);
    }
    for (uintptr_t at : a.calls[1]) {
        if (at)
            patch::RedirectCall(at, OnTeamEmblem1);
    }
    for (uintptr_t at : a.flagCalls)
        patch::RedirectCall(at, OnTeamFlagEmblem);
    patch::RedirectCall(a.dispatcherCall, EmblemDispatcherStub);
    patch::RedirectCall(a.flagObjectCall, OnFlagObject);
    patch::RedirectCall(a.flagTexIdCall, OnFlagTexId);
    LOG(LOG_LEVEL_1, "[Flags] team emblems: on");
}

// --- the emblems of saved games (Load / Save data, the replays) ---
// A save keeps the emblems of its two teams as pictures (32x32, 8 bits) next to their ids, and these screens make
// the textures from those pictures (SavedEmblem(save header, texture id, which: 0, 1 the first team, 2, 3 the second;
// 4..7 other pictures)), so a Master League save showed the game's emblem. The leagues and cups make it from the team
// (TeamEmblemTexture(texture id, team, 1, 1)): the teams' emblems of the saves are made that way too, so they get the
// files of the emblems or of the teams. A replay of a Master League match has the Master League's own team (MyTeam,
// HisTeam): the save's copy of its data (the teams' records follow each other) has the team it is made of, whose
// emblem is used. A Master League save keeps only the picture of its team (4, 6; 5, 7 bigger) and its name: the team is
// the mode's team, else the club with that name (the game's names, as the save has them).
struct SavedEmblemAddresses {
    uintptr_t savedEmblem;          // SavedEmblem(header, texture id, which)
    uintptr_t teamEmblemTexture;    // TeamEmblemTexture(texture id, team, size, 1)
    uintptr_t emblemContext;        // (pointer) the emblems of these screens, used by TeamEmblemTexture
    unsigned int teams;             // in the header: the ids of the two teams (words)
    unsigned int myTeam;            // the first of the Master League's own teams
    unsigned int records;           // in the header: the data of the two teams
    unsigned int recordSize;
    unsigned int recordBase;        // in a team's data: the team it is made of (word)
    uintptr_t teamName;             // char *TeamName(team, kind, defaults)
    unsigned int modeTeam;          // in the header of a Master League save: the mode's team (word)
    unsigned int savedName;         // its team's name (48 bytes)
};
constinit static SavedEmblemAddresses gSavedEmblem = {};
constinit static uintptr_t oSavedEmblem = 0;

// the club with this name (the game's names: the edited one, the default one)
static unsigned int TeamOfName(char const *name) {
    uintptr_t teamName = GameTeamNameFunction();
    if (!teamName)
        teamName = gSavedEmblem.teamName;
    for (unsigned int team = 64; name[0] && team < gSavedEmblem.myTeam; team++) {
        for (int defaults = 0; defaults < 2; defaults++) {
            auto n = CallAndReturnDynGlobal<char const *>(teamName, team, 0, defaults);
            if (n && !strcmp(n, name))
                return team;
        }
    }
    return 0xFFFF;
}

static void *__cdecl OnSavedEmblem(uint8_t const *header, int texId, int which) {
    if (header && which >= 0 && which <= 7 && *(void *const *)gSavedEmblem.emblemContext) {
        unsigned int saved, team, size = 1;
        char name[49] = {};
        if (which <= 3) {
            unsigned int second = which >= 2;
            saved = team = *(uint16_t const *)(header + gSavedEmblem.teams + 2 * second);
            if (team >= gSavedEmblem.myTeam) {
                // the Master League's own team: the club it is made of
                team = *(uint16_t const *)(header + gSavedEmblem.records + gSavedEmblem.recordSize * second
                    + gSavedEmblem.recordBase);
                if (team < 64)
                    team = 0xFFFF;
            }
        }
        else {
            // a Master League save: the mode's team, else the club of the save's name
            size = which & 1 ? 2 : 1;
            saved = team = *(uint16_t const *)(header + gSavedEmblem.modeTeam);
            if (team < 64 || team >= gSavedEmblem.myTeam) {
                memcpy(name, header + gSavedEmblem.savedName, 48);
                team = TeamOfName(name);
            }
        }
        // logged once per picture and team
        constinit static uint32_t logged[32] = {};
        uint32_t key = (uint32_t)which << 28 | (saved & 0x3FFF) << 14 | (team & 0x3FFF);
        for (uint32_t &l : logged) {
            if (l == key)
                break;
            if (!l) {
                l = key;
                LOG(LOG_LEVEL_1, "[Flags] saved game emblem %d: team %u, emblem of team %u (%s)", which, saved, team,
                    name);
                break;
            }
        }
        if (team < gSavedEmblem.myTeam)
            return CallAndReturnDynGlobal<void *>(gSavedEmblem.teamEmblemTexture, texId, team, size, 1);
    }
    return CallAndReturnDynGlobal<void *>(oSavedEmblem, header, texId, which);
}

static void InstallSavedEmblems(SavedEmblemAddresses const &a) {
    // SavedEmblem: sub esp, 8; push edi; mov edi, [esp+14h]; TeamEmblemTexture: push ebp; mov ebp, [esp+8]; push ebp
    oSavedEmblem = patch::GetBytes(a.teamEmblemTexture, 6) == "55 8B 6C 24 08 55"
        ? MakeTrampoline(a.savedEmblem, "83 EC 08 57 8B 7C 24 14") : 0;
    if (!oSavedEmblem) {
        LOG(LOG_LEVEL_1, "[Flags] unexpected code at %X, the emblems of saved games are not replaced", a.savedEmblem);
        return;
    }
    gSavedEmblem = a;
    patch::RedirectJump(a.savedEmblem, OnSavedEmblem);
}

// A 32-bit texture with these pixels in place of 'owner'
static void SetFlagTexture(Texture *owner, uint32_t const *pixels, unsigned int width, unsigned int height,
    bool mips, char const *reason)
{
    Texture *clone = CreateArgbTexture(owner, width, height);
    if (!clone)
        return;
    memcpy(Texels(clone), pixels, (size_t)width * height * 4);
    if (mips)
        BuildFlagMips(clone);
    else
        clone->vramPages_levels &= 0x0FFF;
    RegisterClone(owner, clone, reason);
}

// The flag object (48 bytes): +8 the flag file (+0: its data: size, width, height, raster width, raster height),
// +0x14 flag id, +0x18 texture reference, +0x28 folder
static void ReplaceFlagTexture(uint8_t *flag) {
    unsigned int folder = flag[0x28];
    unsigned int id = *(uint32_t *)(flag + 0x14);
    auto texRef = *(uint8_t **)(flag + 0x18);
    auto flagFile = *(uint8_t **)(flag + 0x08);
    if (folder >= 3 || !texRef || !flagFile || !*(uint8_t **)flagFile)
        return;
    uint8_t const *data = *(uint8_t **)flagFile;
    unsigned int w = data[4], h = data[5], rasterWidth = data[6], rasterHeight = data[7];
    auto owner = *(Texture **)(texRef + gFlagTexRefTexture);
    if (!owner || owner->width != rasterWidth || owner->height != rasterHeight || w > rasterWidth || h > rasterHeight)
        return;
    // a team emblem (see the team emblems): the team's file
    unsigned int keyFolder = folder, keyId = id;
    if (folder == 1 && id >= (unsigned int)kTeamFlagId && id < (unsigned int)kTeamFlagId + 512) {
        keyId = gTeamOfFlagId[id - kTeamFlagId];
        keyFolder = IsNationalTeam(keyId) ? 4 : 3;
    }
    auto file = FlagFiles().find(FlagKey(keyFolder, keyId, w, h));
    if (file == FlagFiles().end())
        file = FlagFiles().find(FlagKey(keyFolder, keyId, 0, 0));
    if (file == FlagFiles().end())
        return;
    // a DDS of the texture's size: as it is (when nothing has to be squeezed: FLAGS_ASPECT 1, or a pattern)
    wstring const &path = file->second;
    if (IsDdsPath(path) && (folder == 2 || fabs(gFlagAspect - 1.0f) < 0.001f)) {
        if (Texture *t = DdsTexture(owner, path, rasterWidth * gFlagScale, rasterHeight * gFlagScale, gFlagMips)) {
            LOG(LOG_LEVEL_2, "[Flags] %S: %ux%u as it is (format %u, %u levels)", path.c_str(), t->width, t->height,
                t->platformFormat, (t->vramPages_levels >> 12) + 1);
            RegisterClone(owner, t, "flag");
            return;
        }
    }
    FlagPicture const *picture = GetFlagPicture(FlagKey(keyFolder, keyId, w, h), file->second, folder, w, h, rasterWidth,
        rasterHeight);
    if (!picture) {
        FlagFiles().erase(file);    // reported once
        return;
    }
    // the emblems on the screens: FLAGS_MIPMAPS (their copies, e.g. on the stadium screens, have mip levels)
    SetFlagTexture(owner, picture->pixels.data(), picture->width, picture->height, gFlagMips, "flag");
}

// --- team flags (the waving flags) ---
// The game makes the flag of a team as an emblem of its own (PES5 ids 448-449, PES6 456-457): the emblem, pattern
// and two colors of the team are set first (Edit Flag), then the builder composes a 64x64 RGBA5551 texture from
// the pixels of the emblem's texture: national teams (emblems of the country flags) get their flag stretched to
// 64x48, the others the pattern (folder 2 of the 64x64 file: index 1 -> color 1, 2 -> color 2) at 64x48 with the
// emblem reduced to 48x48 over it at x 8. The flag takes the top 64x48 of the texture.
// The emblem's texture still has the game's picture, so for an emblem with a file the flag is composed again here
// in 32 bits, FLAGS_SCALE times bigger, with the pattern from folder_2 if it has a file there (dark ->
// color 1, light -> color 2). A team with a file in team_flags gets the first flag of its atlas instead (see the
// flags in the stands).

// The cloth flags of a kind in the stands: two objects (home, away), the flags of a team in one mesh
struct ClothFlags {
    uintptr_t objects;              // the game's pointer to the objects
    unsigned int stride;            // size of an object
    unsigned int countOffset;       // number of flags
    unsigned int drawOffset;        // draw data: +2 number of triangles, +8 vertices (24 bytes: x, y, z, color, u, v)
    unsigned int maxFlags;
    unsigned int verticesPerFlag;   // the front and the back: rows (along the flag) x columns (across it) each
    unsigned int columns;
    unsigned int trianglesPerFlag;
    uintptr_t bindCall;             // the call that binds the texture of the flags (SetTexturePalette(id))
};

struct TeamFlagAddresses {
    uintptr_t builderCall;      // call TeamFlagBuild(id, size) in the emblem dispatcher
    uintptr_t builder;
    uintptr_t emblem;           // int emblem, int pattern, uint32_t colors[2] (R, G, B, A)
    uintptr_t fileForSize;      // int (int size): flag file of the size
    uintptr_t flagFile;         // FlagsFolder *(int file): +0 its data
    uintptr_t findFlag;         // record *(int file, int flag id, int folder): +2 data size, +4 data
    int lastCountry, firstClub, lastClub;
    uintptr_t bigFlagTextureCall;   // the call that binds the texture of the big flag (SetTexturePalette(id))
    ClothFlags clothFlags[2];   // the flags on poles, the small flags in the crowd
};
constinit static TeamFlagAddresses gTeamFlag = {};

static uint32_t ToArgb(uint32_t rgba) {
    // R, G, B, A in memory -> A8R8G8B8 (opaque)
    return 0xFF000000u | ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
}

static uint32_t Mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0xFF000000u;
    for (int ch = 0; ch < 24; ch += 8) {
        float v = ((a >> ch) & 0xFF) * (1.0f - t) + ((b >> ch) & 0xFF) * t;
        out |= (uint32_t)lround(v) << ch;
    }
    return out;
}

// The pattern of the team flag at width x height: from folder_2, or the game's one
static bool TeamFlagPattern(int pattern, int size, uint32_t color1, uint32_t color2, unsigned int width,
    unsigned int height, vector<uint32_t> &out)
{
    vector<uint32_t> image;
    unsigned int iw = 0, ih = 0;
    auto file = FlagFiles().find(FlagKey(2, (unsigned int)pattern, 64, 64));
    if (file == FlagFiles().end())
        file = FlagFiles().find(FlagKey(2, (unsigned int)pattern, 0, 0));
    if (file != FlagFiles().end() && LoadFlagImageCached(file->second, image, iw, ih)) {
        // dark -> color 1, light -> color 2
        for (uint32_t &c : image) {
            float lum = (0.299f * ((c >> 16) & 0xFF) + 0.587f * ((c >> 8) & 0xFF) + 0.114f * (c & 0xFF)) / 255.0f;
            c = (c & 0xFF000000) | (Mix(color1, color2, lum) & 0xFFFFFF);
        }
    }
    else {
        int fileIndex = CallAndReturnDynGlobal<int>(gTeamFlag.fileForSize, size);
        auto flags = CallAndReturnDynGlobal<uint8_t *>(gTeamFlag.flagFile, fileIndex);
        auto record = CallAndReturnDynGlobal<uint8_t *>(gTeamFlag.findFlag, fileIndex, pattern, 2);
        if (!flags || !*(uint8_t **)flags || !record)
            return false;
        uint8_t const *data = *(uint8_t **)flags;
        iw = data[4];
        ih = data[5];
        vector<char> indices((size_t)iw * ih);
        int n = stbi_zlib_decode_buffer(indices.data(), (int)indices.size(), *(char const **)(record + 4),
            *(uint16_t *)(record + 2));
        if (n != (int)indices.size())
            return false;
        image.resize(indices.size());
        for (size_t i = 0; i < image.size(); i++)
            image[i] = indices[i] == 1 ? color1 : indices[i] == 2 ? color2 : 0;
    }
    out = ResizeArgb(image, iw, ih, width, height);
    return true;
}

static void AlphaOver(uint32_t &dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (!a)
        return;
    uint32_t da = dst >> 24;
    uint32_t outA = a + da * (255 - a) / 255;
    uint32_t out = outA << 24;
    for (int ch = 0; ch < 24; ch += 8) {
        uint32_t sc = (src >> ch) & 0xFF, dc = (dst >> ch) & 0xFF;
        out |= ((sc * a + dc * da * (255 - a) / 255) / max(outA, 1u)) << ch;
    }
    dst = out;
}

const int kStadiumFlagTexture = 28990;      // + side: the stadium copies of the team flags (home, away)
constinit static uintptr_t gBindTexture = 0;    // SetTexturePalette(texture id): binds a texture before a draw

static bool IsPowerOfTwo(unsigned int v) {
    return v && !(v & (v - 1));
}

// A team_flags / big_flags file, used in the layout it has (the texture is as big as the file): a DDS as it is (its
// format and mip levels), a PNG in 32 bits with mip levels made here (resized to powers of two when it isn't)
struct TextureFile {
    bool dds = false;
    vector<uint8_t> data;       // the DDS file
    vector<uint32_t> pixels;    // level 0 as A8R8G8B8 (a DDS: when decoded)
    unsigned int width = 0, height = 0;
    unsigned int imageWidth = 0, imageHeight = 0;   // the size of the picture (a PNG is resized to powers of two)
};

// Pixels put in the top left corner of a bigger texture: the rest repeats the last column and row (so the picture's
// edge is drawn as it is)
static vector<uint32_t> PadArgb(vector<uint32_t> const &src, unsigned int sw, unsigned int sh, unsigned int dw,
    unsigned int dh)
{
    vector<uint32_t> out((size_t)dw * dh);
    for (unsigned int y = 0; y < dh; y++) {
        uint32_t const *line = &src[(size_t)min(y, sh - 1) * sw];
        uint32_t *dst = &out[(size_t)y * dw];
        memcpy(dst, line, (size_t)sw * 4);
        for (unsigned int x = sw; x < dw; x++)
            dst[x] = line[sw - 1];
    }
    return out;
}

// pad: a PNG whose sizes aren't powers of two is padded to them (else resized)
static bool ReadTextureFile(wstring const &path, TextureFile &f, bool pad = false) {
    vector<uint8_t> file;
    if (!ReadWholeFile(path, file))
        return false;
    f.dds = IsDdsPath(path);
    if (f.dds) {
        DdsInfo info;
        if (!ParseDds(file, info))
            return false;
        if (!IsPowerOfTwo(info.width) || !IsPowerOfTwo(info.height)) {
            LOG(LOG_LEVEL_1, "[Flags] %S: %ux%u, the sizes of a DDS file must be powers of two", path.c_str(),
                info.width, info.height);
            return false;
        }
        f.width = f.imageWidth = info.width;
        f.height = f.imageHeight = info.height;
        f.data = std::move(file);
        return true;
    }
    unsigned int iw, ih;
    if (!DecodeImage(path, file, f.pixels, iw, ih))
        return false;
    f.imageWidth = iw;
    f.imageHeight = ih;
    f.width = 1;
    f.height = 1;
    while (f.width < iw && f.width < 4096)
        f.width *= 2;
    while (f.height < ih && f.height < 4096)
        f.height *= 2;
    if (f.width != iw || f.height != ih) {
        if (pad && iw <= f.width && ih <= f.height)
            f.pixels = PadArgb(f.pixels, iw, ih, f.width, f.height);
        else
            f.pixels = ResizeArgb(f.pixels, iw, ih, f.width, f.height);
    }
    return true;
}

// Level 0 of the file as A8R8G8B8 pixels
static bool DecodeTextureFile(TextureFile &f) {
    unsigned int width, height;
    return !f.pixels.empty() || DecodeDds(f.data, f.pixels, width, height);
}

// The texture of the file, with the header (id, ...) of 'owner'
static Texture *TextureFromFile(Texture *owner, TextureFile const &f) {
    if (f.dds)
        return DdsTexture(owner, f.data, f.width, f.height, true);
    Texture *t = CreateArgbTexture(owner, f.width, f.height);
    if (t) {
        memcpy(Texels(t), f.pixels.data(), f.pixels.size() * 4);
        BuildFlagMips(t);
    }
    return t;
}

Texture *LoadPictureTexture(wstring const &path, uint32_t texNo, bool mips, unsigned int &width, unsigned int &height) {
    TextureFile f;
    if (!ReadTextureFile(path, f, true))
        return nullptr;
    Texture header = {};
    header.texNo = texNo;
    Texture *t;
    if (f.dds)
        t = DdsTexture(&header, f.data, f.width, f.height, mips);
    else {
        t = CreateArgbTexture(&header, f.width, f.height);
        if (t) {
            BleedArgb(f.pixels, f.width, f.height);
            memcpy(Texels(t), f.pixels.data(), f.pixels.size() * 4);
            if (mips)
                BuildFlagMips(t);
            else
                t->vramPages_levels &= 0x0FFF;
        }
    }
    if (!t)
        return nullptr;
    t->texNo = texNo;
    AddOwnTexture(t);
    width = f.imageWidth;
    height = f.imageHeight;
    return t;
}

static unsigned int TextureLevels(Texture const *t) {
    return (t->vramPages_levels >> 12) + 1u;
}

// The team's files of the kind (1: team_flags, 2: big_flags), by variation
static vector<wstring> TeamFlagFiles(int team, unsigned int kind) {
    vector<wstring> files;
    if (team < 0)
        return files;
    uint64_t first = FlagKey(4 + kind, (unsigned int)team, 0, 0);
    for (auto it = FlagFiles().lower_bound(first); it != FlagFiles().end() && (it->first >> 32) == (first >> 32); ++it)
        files.push_back(it->second);
    return files;
}

// A random number (the variations of the flags)
static unsigned int FlagRandom() {
    static uint32_t state = GetTickCount() | 1;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// --- the flags in the stands ---
// The flags on poles (up to 6 per team) and the small flags in the crowd (up to 14 per team) are cloth: one mesh per
// team and kind, drawn with the stadium flag texture of the team (28990 home, 28991 away), each flag a grid of
// vertices with the texture coordinates of the team flag (u 0..1 along the flag, v 0..0.75 across it: the top 3/4
// of the texture). The game sets the coordinates when it sets up the stadium; with team flag files the plugin sets
// them every time before the flags are drawn.
// A team_flags file is an atlas of flags: 2x2 cells (a square file) or 4x2 (twice as wide as high), every cell one
// whole flag. For a match the team gets it as a texture of its own (kFlagAtlasTexture + side, the file as it is),
// every flag in the stands one of the cells (random, again for every match), and its first cell as the team flag
// (the menus, the celebration, the big flag without a file of its own). With more files: one of them for a match.
const int kFlagAtlasTexture = 28986;
const unsigned int kMaxClothFlags = 16;
constinit static bool gClothFlagsHook = false;

struct FlagAtlas {
    Texture *texture;
    unsigned int width, height, columns;
    uint8_t cells[2][kMaxClothFlags];   // per kind of flags (on poles, small): the cell of every flag
};
constinit static FlagAtlas gFlagAtlas[2] = {};

static void ClearFlagAtlas(int side) {
    if (gFlagAtlas[side].texture)
        DeleteOwnTexture(gFlagAtlas[side].texture);
    gFlagAtlas[side] = {};
}

// Random cells for the flags: every cell once before any of them again
static void ShuffleCells(uint8_t *cells, unsigned int count) {
    for (unsigned int first = 0; first < kMaxClothFlags; first += count) {
        uint8_t order[8];
        for (unsigned int i = 0; i < count; i++)
            order[i] = (uint8_t)i;
        for (unsigned int i = count - 1; i > 0; i--)
            std::swap(order[i], order[FlagRandom() % (i + 1)]);
        for (unsigned int i = 0; i < count && first + i < kMaxClothFlags; i++)
            cells[first + i] = order[i];
    }
}

// A team_flags file: the team flag (its first cell in the layout of the team flag texture: the top 3/4 of a square
// texture as big as the cell) and, for the stands of a match (side 0, 1: home, away), the atlas
static bool LoadFlagAtlas(wstring const &path, int side, Texture *owner, vector<uint32_t> &teamFlag,
    unsigned int &size)
{
    TextureFile f;
    if (!ReadTextureFile(path, f) || !DecodeTextureFile(f)) {
        LOG(LOG_LEVEL_1, "[Flags] can't read %S (PNG, or DDS: DXT1/DXT3/DXT5/32-bit)", path.c_str());
        return false;
    }
    unsigned int columns = f.width == f.height ? 2 : f.width == 2 * f.height ? 4 : 0;
    if (!columns || f.height < 16) {
        LOG(LOG_LEVEL_1, "[Flags] %S: %ux%u is not an atlas of team flags (2x2 cells: square, 4x2: twice as wide "
            "as high)", path.c_str(), f.width, f.height);
        return false;
    }
    unsigned int cell = f.height / 2;
    vector<uint32_t> first((size_t)cell * cell);
    for (unsigned int y = 0; y < cell; y++)
        memcpy(&first[(size_t)y * cell], &f.pixels[(size_t)y * f.width], cell * 4);
    size = min(cell, 1024u);
    teamFlag.assign((size_t)size * size, 0);
    vector<uint32_t> flag = ResizeArgb(first, cell, cell, size, size * 3 / 4);
    memcpy(teamFlag.data(), flag.data(), flag.size() * 4);
    BleedArgb(teamFlag, size, size);
    if (side < 0 || !gClothFlagsHook)
        return true;
    ClearFlagAtlas(side);
    Texture *t = TextureFromFile(owner, f);
    if (!t)
        return true;
    t->texNo = (uint32_t)(kFlagAtlasTexture + side);
    AddOwnTexture(t);
    FlagAtlas &atlas = gFlagAtlas[side];
    atlas.texture = t;
    atlas.width = f.width;
    atlas.height = f.height;
    atlas.columns = columns;
    for (auto &cells : atlas.cells)
        ShuffleCells(cells, columns * 2);
    LOG(LOG_LEVEL_2, "[Flags] flags in the stands: %S, %ux2 flags of %ux%u (format %u, %u levels)", path.c_str(),
        columns, cell, cell, t->platformFormat, TextureLevels(t));
    return true;
}

// The texture coordinates of the flags of a kind of a team: in the cells of the team's atlas, or the game's ones
static void SetClothFlagCoordinates(unsigned int kind, int side) {
    ClothFlags const &c = gTeamFlag.clothFlags[kind];
    auto objects = *(uint8_t **)c.objects;
    if (!objects)
        return;
    uint8_t *object = objects + (size_t)c.stride * side;
    unsigned int count = *(uint32_t *)(object + c.countOffset);
    uint8_t *draw = object + c.drawOffset;
    auto vertices = *(float **)(draw + 8);
    // only a mesh as expected: the number of flags, the triangles of every flag
    if (!vertices || !count || count > c.maxFlags || count > kMaxClothFlags
        || *(uint16_t *)(draw + 2) != count * c.trianglesPerFlag)
        return;
    FlagAtlas const &atlas = gFlagAtlas[side];
    unsigned int face = c.verticesPerFlag / 2, rows = face / c.columns;
    for (unsigned int i = 0; i < count; i++) {
        float u0 = 0.0f, du = 1.0f, v0 = 0.0f, dv = 0.75f;
        if (atlas.texture) {
            // the cell, half a texel in from its edges (no texels of the next cells)
            unsigned int cell = atlas.cells[kind][i], cellSize = atlas.height / 2;
            u0 = ((cell % atlas.columns) * cellSize + 0.5f) / atlas.width;
            v0 = ((cell / atlas.columns) * cellSize + 0.5f) / atlas.height;
            du = (cellSize - 1.0f) / atlas.width;
            dv = (cellSize - 1.0f) / atlas.height;
        }
        // the front, then the back with the same coordinates; vertices: x, y, z, color, u, v
        float *v = vertices + (size_t)i * c.verticesPerFlag * 6;
        for (unsigned int k = 0; k < c.verticesPerFlag; k++, v += 6) {
            unsigned int j = k % face;
            v[4] = u0 + du * (float)(j / c.columns) / (float)(rows - 1);
            v[5] = v0 + dv * (float)(j % c.columns) / (float)(c.columns - 1);
        }
    }
}

static void BindClothFlags(unsigned int kind, int texId) {
    int side = texId - kStadiumFlagTexture;
    if (side == 0 || side == 1) {
        SetClothFlagCoordinates(kind, side);
        if (gFlagAtlas[side].texture)
            texId = kFlagAtlasTexture + side;
    }
    CallDynGlobal(gBindTexture, texId);
}

static void __cdecl OnPoleFlagsTexture(int texId) {
    BindClothFlags(0, texId);
}

static void __cdecl OnSmallFlagsTexture(int texId) {
    BindClothFlags(1, texId);
}

// --- the big flag ---
// The big flag of the match is drawn with the stadium flag texture of its team (28990 home, 28991 away: the copies
// of the team flags), bound by one call before it is drawn. With a file in big_flags (<team id>) the team gets
// a texture of its own (28988 + side, ids the game doesn't use), made when the team flag is made: the file as it is
// (the big flag shows the top 74% of it: u 0..1, v 0..0.74).
const int kBigFlagTexture = 28988;
constinit static bool gBigFlagHook = false;
constinit static Texture *gBigFlag[2] = {};
constinit static int gBigFlagTeam[2] = { -1, -1 };

static void PrepareBigFlag(int side, int team, Texture *flagTexture) {
    if (!gBigFlagHook)
        return;
    vector<wstring> files = TeamFlagFiles(team, 2);
    if (files.empty()) {
        if (gBigFlag[side])
            DeleteOwnTexture(gBigFlag[side]);
        gBigFlag[side] = nullptr;
        gBigFlagTeam[side] = -1;
        return;
    }
    // one of the variations, chosen again for every match
    if (gBigFlag[side] && gBigFlagTeam[side] == team && files.size() == 1)
        return;
    wstring const &file = files[FlagRandom() % files.size()];
    TextureFile f;
    Texture *t = ReadTextureFile(file, f) ? TextureFromFile(flagTexture, f) : nullptr;
    if (!t) {
        LOG(LOG_LEVEL_1, "[Flags] can't read %S (PNG, or DDS: DXT1/DXT3/DXT5/32-bit)", file.c_str());
        return;
    }
    t->texNo = (uint32_t)(kBigFlagTexture + side);
    if (gBigFlag[side])
        DeleteOwnTexture(gBigFlag[side]);
    AddOwnTexture(t);
    gBigFlag[side] = t;
    gBigFlagTeam[side] = team;
    LOG(LOG_LEVEL_2, "[Flags] big flag of team %d: %S (%ux%u, format %u, %u levels)", team, file.c_str(), t->width,
        t->height, t->platformFormat, TextureLevels(t));
}

static void __cdecl OnBigFlagTexture(int texId) {
    int side = texId - kStadiumFlagTexture;
    if ((side == 0 || side == 1) && gBigFlag[side])
        texId = kBigFlagTexture + side;
    CallDynGlobal(gBindTexture, texId);
}

static void ReplaceTeamFlag(uint32_t *flag, int size) {
    auto spec = (int *)gTeamFlag.emblem;
    int emblem = spec[0], pattern = spec[1];
    uint32_t color1 = ToArgb((uint32_t)spec[2]), color2 = ToArgb((uint32_t)spec[3]);
    auto texRef = (uint8_t *)flag[6];
    if (!texRef)
        return;
    auto owner = *(Texture **)(texRef + gFlagTexRefTexture);
    if (!owner || owner->width != 64 || owner->height != 64)
        return;
    // the team the flag is for (its flag was set by the game with this emblem)
    int team = gFlagTeam >= 0 && emblem == gFlagTeamEmblem ? gFlagTeam : -1;
    // the stadium flags of the match (home, away): the big flag, the flags in the stands
    int side = (int)owner->texNo - kStadiumFlagTexture;
    bool stadium = side == 0 || side == 1;
    if (stadium)
        PrepareBigFlag(side, team, owner);
    unsigned int k = gFlagScale;
    unsigned int width = 64 * k, height = 48 * k;      // the flag: the top 64x48 of the texture
    vector<uint32_t> picture;
    char const *what;
    vector<wstring> teamFlags = TeamFlagFiles(team, 1);
    if (!teamFlags.empty()) {
        // the team's atlas: one of them for a match, the first one elsewhere (Edit mode)
        wstring const &file = stadium ? teamFlags[FlagRandom() % teamFlags.size()] : teamFlags[0];
        unsigned int pictureSize;
        if (LoadFlagAtlas(file, stadium ? side : -1, owner, picture, pictureSize)) {
            LOG(LOG_LEVEL_2, "[Flags] team flag of team %d: %S", team, file.c_str());
            SetFlagTexture(owner, picture.data(), pictureSize, pictureSize, true, "team flag");
            return;
        }
    }
    if (stadium && gFlagAtlas[side].texture)
        ClearFlagAtlas(side);
    picture.assign((size_t)width * width, 0);
    if (emblem >= 0 && emblem <= gTeamFlag.lastCountry) {
        auto file = FlagFiles().find(FlagKey(0, (unsigned int)emblem, 64, 64));
        if (file == FlagFiles().end())
            file = FlagFiles().find(FlagKey(0, (unsigned int)emblem, 0, 0));
        vector<uint32_t> image;
        unsigned int iw, ih;
        if (file == FlagFiles().end() || !LoadFlagImageCached(file->second, image, iw, ih))
            return;
        // the country flag fills the flag
        vector<uint32_t> flagPixels = ResizeArgb(image, iw, ih, width, height);
        memcpy(picture.data(), flagPixels.data(), flagPixels.size() * 4);
        what = "country";
    }
    else if (emblem > gTeamFlag.lastCountry) {
        // the club's own emblem file (clubs\<team id>), or the file of its emblem
        unsigned int keyFolder = 1, keyId = (unsigned int)(emblem - gTeamFlag.firstClub);
        bool teamFile = gFlagTeam >= 0 && emblem == gFlagTeamEmblem && !IsNationalTeam((unsigned int)gFlagTeam)
            && HasTeamFile((unsigned int)gFlagTeam);
        if (teamFile) {
            keyFolder = 3;
            keyId = (unsigned int)gFlagTeam;
        }
        else if (emblem < gTeamFlag.firstClub || emblem > gTeamFlag.lastClub)
            return;
        auto file = FlagFiles().find(FlagKey(keyFolder, keyId, 64, 64));
        if (file == FlagFiles().end())
            file = FlagFiles().find(FlagKey(keyFolder, keyId, 0, 0));
        if (file == FlagFiles().end())
            return;
        // the emblem as on the 64x64 emblems, reduced to 48x48
        FlagPicture const *badge = GetFlagPicture(FlagKey(keyFolder, keyId, 64, 64), file->second, 1, 64, 64, 64, 64);
        vector<uint32_t> background;
        if (!badge || !TeamFlagPattern(pattern, size, color1, color2, width, height, background))
            return;
        memcpy(picture.data(), background.data(), background.size() * 4);
        unsigned int badgeSize = 48 * k;
        vector<uint32_t> reduced = ResizeArgb(badge->pixels, badge->width, badge->height, badgeSize, badgeSize);
        for (unsigned int y = 0; y < badgeSize; y++) {
            for (unsigned int x = 0; x < badgeSize; x++)
                AlphaOver(picture[(size_t)y * width + 8 * k + x], reduced[(size_t)y * badgeSize + x]);
        }
        what = "club";
    }
    else
        return;
    BleedArgb(picture, width, width);
    LOG(LOG_LEVEL_2, "[Flags] team flag: %s emblem %d, pattern %d", what, emblem, pattern);
    // the waving flags are 3D: always with mip levels
    SetFlagTexture(owner, picture.data(), width, width, true, "team flag");
}

static uint32_t *OnTeamFlagBuild(int id, int size) {
    auto flag = CallAndReturnDynGlobal<uint32_t *>(gTeamFlag.builder, id, size);
    if (flag)
        ReplaceTeamFlag(flag, size);
    return flag;
}

static void InstallTeamFlags(TeamFlagAddresses const &a) {
    if (patch::GetBytes(a.builderCall, 1) != "E8" || injector::GetBranchDestination(a.builderCall) != a.builder) {
        LOG(LOG_LEVEL_1, "[Flags] unexpected code at %X, team flags are not replaced", a.builderCall);
        return;
    }
    gTeamFlag = a;
    patch::RedirectCall(a.builderCall, OnTeamFlagBuild);
    // SetTexturePalette(id): binds the textures of the big flag and of the other flags in the stands
    if (patch::GetBytes(a.bigFlagTextureCall, 1) == "E8")
        gBindTexture = injector::GetBranchDestination(a.bigFlagTextureCall);
    bool bigFlags = false, teamFlags = false;
    for (auto const &f : FlagFiles()) {
        bigFlags = bigFlags || (f.first >> 48) == 6;
        teamFlags = teamFlags || (f.first >> 48) == 5;
    }
    // the big flag: only with big flag files
    if (bigFlags && gBindTexture) {
        gBigFlagHook = true;
        patch::RedirectCall(a.bigFlagTextureCall, OnBigFlagTexture);
    }
    // the flags in the stands: only with team flag files
    if (teamFlags) {
        bool ok = gBindTexture != 0;
        for (ClothFlags const &c : a.clothFlags)
            ok = ok && patch::GetBytes(c.bindCall, 1) == "E8" && injector::GetBranchDestination(c.bindCall) == gBindTexture;
        if (ok) {
            gClothFlagsHook = true;
            patch::RedirectCall(a.clothFlags[0].bindCall, OnPoleFlagsTexture);
            patch::RedirectCall(a.clothFlags[1].bindCall, OnSmallFlagsTexture);
        }
        else
            LOG(LOG_LEVEL_1, "[Flags] unexpected code, the flags in the stands show the team flag");
    }
}

static void *OnFlagTextureCreate(int file, int flagId, int folder) {
    auto flag = CallAndReturnDynGlobal<uint8_t *>(oFlagTextureCreate, file, flagId, folder);
    if (flag)
        ReplaceFlagTexture(flag);
    return flag;
}

// The hook is installed only when there are flag files
static void Install(uintptr_t flagTextureCreate, uintptr_t texRefTexture, TeamFlagAddresses const &teamFlags,
    TeamEmblemAddresses const &teamEmblems, SavedEmblemAddresses const &savedEmblems)
{
    if (!config().FLAGS)
        return;
    ScanFlagFiles();
    if (FlagFiles().empty())
        return;
    if (!HooksInstalled()) {
        LOG(LOG_LEVEL_1, "[Flags] the texture hooks are not installed, flags are not replaced");
        return;
    }
    oFlagTextureCreate = MakeTrampoline(flagTextureCreate, "51 57 8B 7C 24 0C 57");
    if (!oFlagTextureCreate) {
        LOG(LOG_LEVEL_1, "[Flags] unexpected code at %X, flags are not replaced", flagTextureCreate);
        return;
    }
    gFlagTexRefTexture = texRefTexture;
    // a power of two: the texture keeps the layout of the game's one
    int scale = std::clamp(config().FLAGS_SCALE, 1, 16);
    gFlagScale = 1;
    while ((int)gFlagScale < scale)
        gFlagScale *= 2;
    try {
        gFlagAspect = std::clamp(std::stof(config().FLAGS_ASPECT), 0.25f, 4.0f);
    }
    catch (...) {
        gFlagAspect = 1.0f;
    }
    gFlagMips = config().FLAGS_MIPMAPS;
    gFlagCacheLimit = (size_t)std::clamp(config().FLAGS_CACHE_MB, 0, 1024) << 20;
    string filter = config().FLAGS_FILTER;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](char c) { return (char)tolower((unsigned char)c); });
    gFlagFilter = filter == "box" ? FILTER_BOX : filter == "bilinear" ? FILTER_BILINEAR
        : filter == "bicubic" ? FILTER_BICUBIC : FILTER_LANCZOS;
    patch::RedirectJump(flagTextureCreate, OnFlagTextureCreate);
    InstallTeamFlags(teamFlags);
    InstallTeamEmblems(teamEmblems);
    InstallSavedEmblems(savedEmblems);
    char const *filters[] = { "box", "bilinear", "bicubic", "lanczos" };
    LOG(LOG_LEVEL_1, "[Flags] %u files in %S, scale %u, aspect %.3f, filter %s, mip levels %s, cache %u MB",
        (unsigned int)FlagFiles().size(), FlagsFolderPath().c_str(), gFlagScale, gFlagAspect, filters[gFlagFilter],
        gFlagMips ? "on" : "off", (unsigned int)(gFlagCacheLimit >> 20));
}

}

void InitFlagReplacer(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        // FlagTextureCreate; Texture * in the texture reference; team flags
        FlagReplacer::Install(0x5A7C70, 0x10, { 0x5443A9, 0x544160, 0xFE26B8, 0x543B80, 0x5A35D0, 0x5A3630, 110, 111, 283, 0x86770B,
              { { 0x386CE54, 11860, 3400, 13968, 6, 60, 5, 80, 0x867E23 },
                { 0x386CE58, 8544, 1728, 10140, 14, 24, 2, 20, 0x8683FF } } },
            { { 0x8F0900, 0x8F02B0 },
              { { 0x543117, 0x5431E5, 0x54326D, 0x5432E5, 0x543345, 0x5433E5, 0x543445 },
                { 0x543125, 0x5431F1, 0x543279, 0x5432F1, 0x543351, 0x5433F1, 0x543460 } },
              { 0x5431A3, 0x9209C9 }, 0x544419, 0x54433B, 0x54436C, 111 },
            { 0x457AA0, 0x457A10, 0xF7ACF8, 0x150, 242, 0x2C, 140, 0x6C, 0x8EFF50, 0x60, 0x68 });
        break;
    case ID_PES6:
        FlagReplacer::Install(0x95D720, 0x08, { 0x9B13F9, 0x9B11B0, 0x3A6B0D8, 0x9B1500, 0x950ED0, 0x950F30, 114, 115, 291, 0x89D03B,
              { { 0x12460A4, 11860, 3400, 13968, 6, 60, 5, 80, 0x89D763 },
                { 0x12460A8, 8544, 1728, 10140, 14, 24, 2, 20, 0x89DD3F } } },
            { { 0x863FF0, 0x8639F0 },
              { { 0x9B03A7, 0x9B0485, 0x9B050D, 0x9B05C5, 0x9B0625, 0x9B0705, 0x9B07A5 },
                { 0x9B03B5, 0x9B0491, 0x9B0519, 0x9B05D1, 0x9B0631, 0x9B0711, 0x9B07C0 } },
              { 0x9B0443, 0x8264A9 }, 0x9B1469, 0x9B138B, 0x9B13BC, 115 },
            { 0xB3BF40, 0xB3BEB0, 0x3B51284, 0xE8, 294, 0x2C, 88, 0x36, 0x863570, 0x60, 0x68 });
        break;
    default:
        break;
    }
}
