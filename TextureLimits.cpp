#include "Main.hpp"
#include <set>
#include <deque>

// Textures of any resolution and format.
//
// The game creates most asset textures (faces, hairs, kits, gloves, ball, ...) with a fixed size and format
// (e.g. 64x128 PAL8 for faces) and then copies the texels/palette from the file texture into them.
// When the file texture doesn't match, we don't copy: we make a copy of the whole file texture instead
// ("clone") and bind it to the texture id, so the renderer uses the clone. The game texture ("owner")
// stays allocated and untouched; operations that the game makes later on the owner pointer are redirected
// to the clone where it makes sense (TexAdd, palette copies, scaling) or skipped where they can't work
// with another size/format (decal stamping).
namespace TextureLimits {

#pragma pack(push, 1)
struct Texture {
    uint32_t signature;
    uint32_t count;
    uint32_t totalSize;
    uint32_t texNo;
    uint16_t texelsOffset;
    uint16_t clutOffset;
    uint16_t width;
    uint16_t height;
    uint8_t textureBufferWidth;
    uint8_t texelFormat;      // 0 - RGBA8888, 2 - RGBA5551, 19 - PAL8, 20 - PAL4
    uint8_t log2Width;
    uint8_t log2Height;
    uint8_t hasClut;
    uint8_t clutFormat;
    uint8_t clutWidth;
    uint8_t clutHeight;
    uint32_t texelsSizeQwc;   // size of the first mip level in 16-byte units
    uint32_t clutSizeQwc;
    uint16_t copyBufferWidth;
    uint16_t copyBufferHeight;
    uint8_t copyTextureBufferWidth;
    uint8_t copyBufferFormat;
    uint16_t vramPages_levels; // number of additional mip levels in the upper 4 bits
    int16_t mipLevelOffsets[6];
    uint8_t platformFormat;   // 0 - texelFormat is used, otherwise DXT1/DXT3/DXT5/...
    uint8_t platformFlags;
    uint8_t pad[2];
};
static_assert(sizeof(Texture) == 64, "Texture header size");

#pragma pack(pop)

// Natural alignment (as in the game): refCount is followed by 2 bytes of padding
struct PlatformTexture {
    int texId;
    int16_t refCount;
    void *pD3DTexture;
    int paletteNumber;
    void *paletteEntries;
    char bHasAlpha;
    char bCreated;
    char bUnloaded;
    char bOwnsTexture;
    Texture *pTexture;
};
static_assert(offsetof(PlatformTexture, pD3DTexture) == 0x08, "PlatformTexture layout");
static_assert(offsetof(PlatformTexture, bCreated) == 0x15, "PlatformTexture layout");
static_assert(offsetof(PlatformTexture, pTexture) == 0x18, "PlatformTexture layout");

const uint8_t TEX_PAL8 = 19;
const uint8_t TEX_PAL4 = 20;

struct GameAddresses {
    uintptr_t GetPlatformTexture;  // PlatformTexture *(int texId), increments refCount
    uintptr_t PlatformTextureDestroy;
    uintptr_t TexTBufRemove;       // schedules re-creation of the D3D texture
    uintptr_t TexAddressC;
    uintptr_t CreateD3D;           // PlatformTexture::CreateD3D(Texture *) (thiscall): binds the texture, re-creates the D3D texture
};
constinit static GameAddresses gAddr = {};

// owner -> clone, clone -> owner
static map<Texture *, Texture *> &Clones() {
    static map<Texture *, Texture *> clones;
    return clones;
}

static map<Texture *, Texture *> &Owners() {
    static map<Texture *, Texture *> owners;
    return owners;
}

// Low-detail textures: far-distance models (or the hair model at a distance) use a smaller copy of the
// main texture with its own id. With LOD_MAIN_TEXTURE they use the main texture (with its mip levels).
struct LodTextureRange { int first; int count; int toMain; };
static constexpr LodTextureRange kLodTextures[] = {
    { 2150, 26, -50 }, // faces: 2150+n -> 2100+n
    { 1450, 26, -50 }, // hairs: 1450+n -> 1400+n
    { 4025, 4,  -25 }, // kits: 4025+n -> 4000+n (the palette stays 4025+n: it has the team colours)
    { 4030, 2,  -25 }, // goalkeeper gloves: 4030+n -> 4005+n
    { 1303, 3,  -3  }, // boots: 1303+n -> 1300+n
};
constinit static bool gLodMainTexture = false;

static int MainTextureId(int texId) {
    for (auto const &r : kLodTextures) {
        if (texId >= r.first && texId < r.first + r.count)
            return texId + r.toMain;
    }
    return texId;
}

static bool IsUnusedLodTexture(uint32_t texId) {
    return gLodMainTexture && MainTextureId((int)texId) != (int)texId;
}

// Recently replaced file textures: the game copies their palette to the low-detail texture
static Texture *(&ReplacedSources())[8] {
    static Texture *sources[8] = {};
    return sources;
}

static bool IsReplacedSource(Texture *t) {
    for (auto s : ReplacedSources()) {
        if (s && s == t)
            return true;
    }
    return false;
}

static void AddReplacedSource(Texture *t) {
    static unsigned int next = 0;
    ReplacedSources()[next++ % std::size(ReplacedSources())] = t;
}

static bool IsClone(Texture *t) { return t && Owners().contains(t); }
static bool HasClone(Texture *t) { return t && Clones().contains(t); }

// A clone can be passed instead of the owner when the game gets the texture by its id
static Texture *OwnerOf(Texture *t) {
    auto it = Owners().find(t);
    return it != Owners().end() ? it->second : t;
}

static Texture *Resolve(Texture *t) {
    auto it = Clones().find(t);
    // the game can reuse the memory of a deleted texture for another one
    if (it == Clones().end() || it->second->texNo != t->texNo)
        return t;
    return it->second;
}

// For palette operations: the replacement, if it has a palette of the same format
static Texture *ResolvePalette(Texture *t) {
    Texture *r = Resolve(t);
    return (r != t && (r->platformFormat != 0 || r->texelFormat != t->texelFormat || !r->clutOffset)) ? t : r;
}

static PlatformTexture *AcquirePlatformTexture(int texId) {
    return CallAndReturnDynGlobal<PlatformTexture *>(gAddr.GetPlatformTexture, texId);
}

static void ReleasePlatformTexture(PlatformTexture *pt) {
    if ((uint16_t)pt->refCount <= 1)
        CallMethodDynGlobal(gAddr.PlatformTextureDestroy, pt);
    else
        pt->refCount--;
}

static void *TexAddressC(Texture *t) {
    return CallAndReturnDynGlobal<void *>(gAddr.TexAddressC, t);
}

static unsigned int NumLevels(Texture const *t) {
    return (t->vramPages_levels >> 12) + 1;
}

// The same way the renderer walks the mip levels (see CreateD3DTexture)
static size_t TexelsSize(Texture const *t) {
    size_t total = 0;
    size_t qwc = t->texelsSizeQwc;
    for (unsigned int i = 0; i < NumLevels(t); i++) {
        total += qwc * 16;
        qwc = (qwc + 3) / 4;
    }
    return total;
}

static size_t BlockSize(Texture const *t) {
    size_t size = sizeof(Texture);
    if (t->texelsOffset)
        size = max(size, t->texelsOffset + TexelsSize(t));
    if (t->clutOffset)
        size = max(size, t->clutOffset + (size_t)t->clutSizeQwc * 16);
    return size;
}

static bool IsPaletted(Texture const *t) {
    return t->platformFormat == 0 && (t->texelFormat == TEX_PAL8 || t->texelFormat == TEX_PAL4);
}

static bool SameFormat(Texture const *a, Texture const *b) {
    return a->platformFormat == b->platformFormat && (a->platformFormat != 0 || a->texelFormat == b->texelFormat);
}

// Copy of 'src' starting from mip level 'level' (smaller levels follow it), as one block:
// header, palette (if any), texels of all levels
static Texture *CreateClone(Texture *src, uint32_t texNo, unsigned int level = 0) {
    unsigned int srcLevels = NumLevels(src);
    if (level >= srcLevels)
        level = srcLevels - 1;
    size_t skip = 0;
    size_t qwc = src->texelsSizeQwc;
    for (unsigned int i = 0; i < level; i++) {
        skip += qwc * 16;
        qwc = (qwc + 3) / 4;
    }
    unsigned int levels = srcLevels - level;
    size_t texelsSize = 0;
    size_t q = qwc;
    for (unsigned int i = 0; i < levels; i++) {
        texelsSize += q * 16;
        q = (q + 3) / 4;
    }
    size_t clutSize = src->clutOffset ? (size_t)src->clutSizeQwc * 16 : 0;
    size_t texelsOffset = sizeof(Texture) + clutSize;
    size_t size = texelsOffset + texelsSize;
    auto clone = (Texture *)_aligned_malloc(size, 16);
    if (!clone)
        return nullptr;
    memcpy(clone, src, sizeof(Texture));
    if (clutSize)
        memcpy((uint8_t *)clone + sizeof(Texture), (uint8_t *)src + src->clutOffset, clutSize);
    memcpy((uint8_t *)clone + texelsOffset, (uint8_t *)src + src->texelsOffset + skip, texelsSize);
    clone->texNo = texNo;
    clone->totalSize = (uint32_t)size;
    clone->clutOffset = clutSize ? (uint16_t)sizeof(Texture) : 0;
    clone->texelsOffset = (uint16_t)texelsOffset;
    if (level) {
        clone->width = (uint16_t)max(src->width >> level, 1);
        clone->height = (uint16_t)max(src->height >> level, 1);
        clone->log2Width = (uint8_t)max((int)src->log2Width - (int)level, 0);
        clone->log2Height = (uint8_t)max((int)src->log2Height - (int)level, 0);
        clone->copyBufferWidth = clone->width;
        clone->copyBufferHeight = clone->height;
        clone->texelsSizeQwc = (uint32_t)qwc;
        clone->vramPages_levels = (uint16_t)(((levels - 1) << 12) | (src->vramPages_levels & 0xFFF));
    }
    memoryStats().textureCopiesSize += size;
    memoryStats().textureCopies++;
    if (IsPaletted(clone)) {
        // the game copies paletted texels as they are into a texture which is not swizzled
        clone->copyBufferFormat = clone->texelFormat;
        clone->platformFlags &= ~1;
    }
    return clone;
}

// A deleted texture id keeps pointing to its last texture until the next TexAdd (the game does the same with
// its own textures), so a clone is not freed immediately: it waits in a short queue.
static void FreeClone(Texture *clone) {
    static deque<Texture *> retired;
    const size_t kMaxRetired = 16;
    Owners().erase(clone);
    memoryStats().textureCopiesSize -= clone->totalSize;
    memoryStats().textureCopies--;
    retired.push_back(clone);
    if (retired.size() > kMaxRetired) {
        _aligned_free(retired.front());
        retired.pop_front();
    }
}

// Bind texture id to 'texture' if it's currently bound to 'from', and re-create the D3D texture
static void Rebind(uint32_t texNo, Texture *from, Texture *to) {
    auto pt = AcquirePlatformTexture(texNo);
    if (pt->pTexture == from) {
        // Re-create right away: this also updates the pixel/palette pointers and drops the cached
        // texture+palette pairs (e.g. kits are drawn with the palette of another texture)
        if (pt->bCreated)
            CallMethodDynGlobal(gAddr.CreateD3D, pt, to);
        else
            pt->pTexture = to; // will be created by TexAdd
    }
    ReleasePlatformTexture(pt);
}

static void RemoveClone(Texture *owner) {
    auto it = Clones().find(owner);
    if (it == Clones().end())
        return;
    Texture *clone = it->second;
    Clones().erase(it);
    if (owner->texNo)
        Rebind(owner->texNo, clone, owner);
    FreeClone(clone);
}

// Use a copy of 'src' in place of 'owner'. Returns false if the game should do the copy itself.
static void MirrorToOwner(Texture *owner, Texture *clone);

// Make 'clone' the texture of 'owner' (it replaces the previous clone, if any)
static void RegisterClone(Texture *owner, Texture *clone, char const *reason) {
    MirrorToOwner(owner, clone);
    Texture *old = nullptr;
    auto it = Clones().find(owner);
    if (it != Clones().end())
        old = it->second;
    Clones()[owner] = clone;
    Owners()[clone] = owner;
    Rebind(owner->texNo, old ? old : owner, clone);
    if (old)
        FreeClone(old);
    LOG(LOG_LEVEL_2, "[TextureLimits] %s: texture %u %ux%u (format %u/%u) replaced by %ux%u (format %u/%u, %u levels)",
        reason, owner->texNo, owner->width, owner->height, owner->texelFormat, owner->platformFormat,
        clone->width, clone->height, clone->texelFormat, clone->platformFormat, NumLevels(clone));
}

// Use a copy of 'src' in place of 'owner'. Returns false if the game should do the copy itself.
static bool SetClone(Texture *owner, Texture *src, char const *reason, unsigned int level = 0) {
    // id 1 is shared by temporary textures (e.g. when kit patterns are applied)
    if (!owner || !src || owner->texNo <= 1)
        return false;
    // the model uses the main texture instead: nothing to copy
    if (IsUnusedLodTexture(owner->texNo))
        return true;
    AddReplacedSource(src);
    Texture *clone = CreateClone(src, owner->texNo, level);
    if (!clone)
        return false;
    RegisterClone(owner, clone, reason);
    return true;
}

// The game often fills only some mip levels of a texture (only the first one when it scales or draws into it).
// The other levels would show garbage when they are used, so the texture is reduced to the levels it gets:
// the renderer makes the rest from them.
static void LimitLevels(Texture *t, unsigned int levels) {
    if (t && levels && NumLevels(t) > levels)
        t->vramPages_levels = (uint16_t)(((levels - 1) << 12) | (t->vramPages_levels & 0xFFF));
}

// Same format and size: the game copies the texels itself (as many levels as 'src' has from 'level')
static bool SameImage(Texture *dst, Texture *src, unsigned int level = 0) {
    return dst && src && dst->platformFormat == 0 && SameFormat(dst, src) && level < NumLevels(src)
        && (src->width >> level) == dst->width && (src->height >> level) == dst->height;
}

// A texture from a replaced file: bigger than the game texture, or in a format the game doesn't use for it
// (DXT, 32-bit). Other differences are the game's own (e.g. a 4-bit texture copied into an 8-bit one), and the
// game does them itself.
static bool IsReplacement(Texture *dst, Texture *src, unsigned int level = 0) {
    return dst && src && ((src->platformFormat != 0 && dst->platformFormat == 0)
        || (src->width >> level) > dst->width || (src->height >> level) > dst->height);
}

// Can the game copy texels of 'src' (starting from mip level 'level') into 'dst'
static bool CanCopyTexels(Texture *dst, Texture *src, unsigned int level = 0) {
    return dst->platformFormat == 0 && SameFormat(dst, src)
        && (src->width >> level) == dst->width && (src->height >> level) == dst->height
        && NumLevels(src) >= NumLevels(dst) + level;
}

static uintptr_t oCopyTextureData, oCopyBaseTexels, oSetImageFromMip, oCopyImageScaled, oCopyTexturePalette;
static uintptr_t oTexAdd, oTexDel, oStampIndexedDecal, oFindAndStampRowMarker, oCopyClutRange, oTintClutRange;
static uintptr_t oGetTextureAssetIfLoaded, oBootBind, oGetTexMatrix;

static uintptr_t oTexAddOriginal() { return oTexAdd; }
static uintptr_t oTexDelOriginal() { return oTexDel; }

// PS2 palettes of 256 colors are stored with bits 3 and 4 of the index swapped
static unsigned int ClutPos(unsigned int index) {
    return (index & 0xE7) | ((index & 8) << 1) | ((index & 16) >> 1);
}

// --- paletted texel access (level 0, linear layout) ---

static uint8_t *Texels(Texture *t) {
    return (uint8_t *)t + t->texelsOffset;
}

static unsigned int GetIndex(Texture *t, int x, int y) {
    size_t i = (size_t)y * t->width + x;
    if (t->texelFormat == TEX_PAL8)
        return Texels(t)[i];
    uint8_t b = Texels(t)[i / 2];
    return (x & 1) ? (b >> 4) : (b & 0xF);
}

static void PutIndex(Texture *t, int x, int y, unsigned int value) {
    size_t i = (size_t)y * t->width + x;
    if (t->texelFormat == TEX_PAL8) {
        Texels(t)[i] = (uint8_t)value;
        return;
    }
    uint8_t &b = Texels(t)[i / 2];
    b = (x & 1) ? (uint8_t)((b & 0x0F) | ((value & 0xF) << 4)) : (uint8_t)((b & 0xF0) | (value & 0xF));
}

// The game texture keeps a downscaled copy of its (bigger) paletted replacement, for the game code which
// reads it in the original size (low-detail copies, previews, ...)
static void MirrorToOwner(Texture *owner, Texture *clone) {
    if (!owner || !clone || owner == clone || !IsPaletted(owner) || !IsPaletted(clone) || owner->texelFormat != clone->texelFormat
        || !owner->texelsOffset || clone->width < owner->width || clone->height < owner->height)
        return;
    for (unsigned int y = 0; y < owner->height; y++) {
        for (unsigned int x = 0; x < owner->width; x++)
            PutIndex(owner, x, y, GetIndex(clone, x * clone->width / owner->width, y * clone->height / owner->height));
    }
    if (owner->clutOffset && clone->clutOffset)
        memcpy((uint8_t *)owner + owner->clutOffset, (uint8_t *)clone + clone->clutOffset, owner->texelFormat == TEX_PAL8 ? 1024 : 64);
}

static uint8_t Log2Ceil(unsigned int value) {
    uint8_t n = 0;
    while ((1u << n) < value)
        n++;
    return n;
}

// Empty paletted texture (one level) of the given size, with the header and palette of 't'
static Texture *CreatePalettedTexture(Texture *t, unsigned int width, unsigned int height) {
    size_t clutSize = t->clutOffset ? (size_t)t->clutSizeQwc * 16 : 0;
    size_t texelsSize = t->texelFormat == TEX_PAL8 ? (size_t)width * height : (size_t)width * height / 2;
    size_t texelsOffset = sizeof(Texture) + clutSize;
    size_t size = texelsOffset + texelsSize;
    auto n = (Texture *)_aligned_malloc(size, 16);
    if (!n)
        return nullptr;
    memcpy(n, t, sizeof(Texture));
    if (clutSize)
        memcpy((uint8_t *)n + sizeof(Texture), (uint8_t *)t + t->clutOffset, clutSize);
    memset((uint8_t *)n + texelsOffset, 0, texelsSize);
    n->totalSize = (uint32_t)size;
    n->clutOffset = clutSize ? (uint16_t)sizeof(Texture) : 0;
    n->texelsOffset = (uint16_t)texelsOffset;
    n->width = (uint16_t)width;
    n->height = (uint16_t)height;
    n->log2Width = Log2Ceil(width);
    n->log2Height = Log2Ceil(height);
    n->copyBufferWidth = n->width;
    n->copyBufferHeight = n->height;
    n->copyBufferFormat = n->texelFormat;
    n->platformFlags &= ~1;
    n->texelsSizeQwc = (uint32_t)(texelsSize / 16);
    n->vramPages_levels &= 0x0FFF;
    memoryStats().textureCopiesSize += size;
    memoryStats().textureCopies++;
    return n;
}

// --- HD shadows ---
// Kit numbers are drawn from the number font into a temporary texture, which is then copied to the player's
// number texture. With a bigger font, the same drawing is also done in its resolution into a "shadow" of the
// temporary texture, and the shadow is used when the game copies it.
static map<Texture *, Texture *> &Shadows() {
    static map<Texture *, Texture *> shadows;
    return shadows;
}

static Texture *ShadowOf(Texture *t) {
    auto it = Shadows().find(t);
    return it != Shadows().end() ? it->second : nullptr;
}

static void DropShadow(Texture *t) {
    auto it = Shadows().find(t);
    if (it == Shadows().end())
        return;
    memoryStats().textureCopiesSize -= it->second->totalSize;
    memoryStats().textureCopies--;
    _aligned_free(it->second);
    Shadows().erase(it);
}

static Texture *ShadowFor(Texture *t, float scale) {
    unsigned int width = (unsigned int)lround(t->width * scale);
    unsigned int height = (unsigned int)lround(t->height * scale);
    Texture *shadow = ShadowOf(t);
    if (shadow && shadow->width == width && shadow->height == height)
        return shadow;
    DropShadow(t);
    shadow = CreatePalettedTexture(t, width, height);
    if (shadow)
        Shadows()[t] = shadow;
    return shadow;
}

// StampIndexedDecal at any scale: 'rect' (source x, y, width, height; negative width - mirrored) and x, y are
// in the original texel coordinates; dstScale/srcScale - how many times the textures are bigger than that
static void ScaledStamp(Texture *dst, float dstScale, Texture *src, float srcScale, int indexOffset, int x, int y,
    int16_t const *rect)
{
    int srcX = rect[0], srcY = rect[1], w = rect[2], h = rect[3];
    bool mirror = w < 0;
    int width = (int)lround(abs(w) * dstScale);
    int height = (int)lround(h * dstScale);
    int dx = (int)lround(x * dstScale);
    int dy = (int)lround(y * dstScale);
    for (int j = 0; j < height; j++) {
        int ty = dy + j;
        int sy = (int)((srcY + (j + 0.5f) / dstScale) * srcScale);
        if (ty < 0 || ty >= dst->height || sy < 0 || sy >= src->height)
            continue;
        for (int i = 0; i < width; i++) {
            int tx = mirror ? dx - i - 1 : dx + i;
            int sx = (int)((srcX + (i + 0.5f) / dstScale) * srcScale);
            if (tx < 0 || tx >= dst->width || sx < 0 || sx >= src->width)
                continue;
            unsigned int index = GetIndex(src, sx, sy);
            if (indexOffset < 0)
                PutIndex(dst, tx, ty, index);
            else if (index)
                PutIndex(dst, tx, ty, index + indexOffset);
        }
    }
}

// --- 32-bit compositing ---
// Platform format 6: D3DFMT_A8R8G8B8 texels (B, G, R, A in memory, alpha 0..255), uploaded as they are
const uint8_t PLATFORM_ARGB8888 = 6;

static uint32_t Ps2Alpha(uint32_t a) {
    return a >= 0x80 ? 255 : a * 2;
}

static void DecodeDxtColors(uint8_t const *block, uint32_t colors[4], bool fourColors) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);
    auto expand = [](uint16_t c, int &r, int &g, int &b) {
        r = ((c >> 11) & 31) * 255 / 31;
        g = ((c >> 5) & 63) * 255 / 63;
        b = (c & 31) * 255 / 31;
    };
    int r0, g0, b0, r1, g1, b1;
    expand(c0, r0, g0, b0);
    expand(c1, r1, g1, b1);
    auto argb = [](int a, int r, int g, int b) { return (uint32_t)((a << 24) | (r << 16) | (g << 8) | b); };
    colors[0] = argb(255, r0, g0, b0);
    colors[1] = argb(255, r1, g1, b1);
    if (fourColors || c0 > c1) {
        colors[2] = argb(255, (2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3);
        colors[3] = argb(255, (r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3);
    }
    else {
        colors[2] = argb(255, (r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2);
        colors[3] = 0;
    }
}

// Level 0 of 't' as A8R8G8B8 pixels (empty if the format is not supported). With 'indexZeroTransparent'
// palette index 0 is transparent (the game doesn't copy it when it stamps a texture over another one).
static vector<uint32_t> DecodeTexture(Texture *t, bool indexZeroTransparent) {
    unsigned int w = t->width, h = t->height;
    vector<uint32_t> out;
    uint8_t const *texels = Texels(t);
    if (IsPaletted(t)) {
        auto clut = (uint32_t const *)TexAddressC(t);
        if (!clut)
            return out;
        out.resize((size_t)w * h);
        bool pal8 = t->texelFormat == TEX_PAL8;
        for (unsigned int y = 0; y < h; y++) {
            for (unsigned int x = 0; x < w; x++) {
                unsigned int index = GetIndex(t, x, y);
                uint32_t c = clut[pal8 ? ClutPos(index) : index];
                // palette entries: R, G, B, A (0..0x80)
                out[(size_t)y * w + x] = (indexZeroTransparent && index == 0) ? 0
                    : ((Ps2Alpha(c >> 24) << 24) | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF));
            }
        }
        return out;
    }
    if (t->platformFormat == 0 && t->texelFormat == 0) { // PS2 RGBA: R, G, B, A (0..0x80)
        out.resize((size_t)w * h);
        for (size_t i = 0; i < out.size(); i++) {
            uint8_t const *p = texels + i * 4;
            out[i] = (Ps2Alpha(p[3]) << 24) | (p[0] << 16) | (p[1] << 8) | p[2];
        }
        return out;
    }
    if (t->platformFormat == PLATFORM_ARGB8888) {
        out.assign((uint32_t const *)texels, (uint32_t const *)texels + (size_t)w * h);
        return out;
    }
    if (t->platformFormat == 1 || t->platformFormat == 3 || t->platformFormat == 5) { // DXT1, DXT3, DXT5
        size_t blockSize = t->platformFormat == 1 ? 8 : 16;
        unsigned int bw = max(1u, (w + 3) / 4), bh = max(1u, (h + 3) / 4);
        out.resize((size_t)w * h);
        for (unsigned int by = 0; by < bh; by++) {
            for (unsigned int bx = 0; bx < bw; bx++) {
                uint8_t const *block = texels + ((size_t)by * bw + bx) * blockSize;
                uint8_t const *colorBlock = blockSize == 16 ? block + 8 : block;
                uint32_t colors[4];
                DecodeDxtColors(colorBlock, colors, blockSize == 16);
                uint32_t indices = colorBlock[4] | (colorBlock[5] << 8) | (colorBlock[6] << 16) | (colorBlock[7] << 24);
                uint8_t alphas[8] = {};
                uint64_t alphaBits = 0;
                if (t->platformFormat == 5) {
                    alphas[0] = block[0];
                    alphas[1] = block[1];
                    for (int i = 0; i < 6; i++)
                        alphaBits |= (uint64_t)block[2 + i] << (8 * i);
                    if (alphas[0] > alphas[1]) {
                        for (int i = 1; i < 7; i++)
                            alphas[i + 1] = (uint8_t)(((7 - i) * alphas[0] + i * alphas[1]) / 7);
                    }
                    else {
                        for (int i = 1; i < 5; i++)
                            alphas[i + 1] = (uint8_t)(((5 - i) * alphas[0] + i * alphas[1]) / 5);
                        alphas[6] = 0;
                        alphas[7] = 255;
                    }
                }
                for (int py = 0; py < 4; py++) {
                    for (int px = 0; px < 4; px++) {
                        unsigned int x = bx * 4 + px, y = by * 4 + py;
                        if (x >= w || y >= h)
                            continue;
                        int i = py * 4 + px;
                        uint32_t c = colors[(indices >> (2 * i)) & 3];
                        if (t->platformFormat == 3) {
                            uint32_t a = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
                            c = (c & 0xFFFFFF) | ((a * 17) << 24);
                        }
                        else if (t->platformFormat == 5)
                            c = (c & 0xFFFFFF) | ((uint32_t)alphas[(alphaBits >> (3 * i)) & 7] << 24);
                        out[(size_t)y * w + x] = c;
                    }
                }
            }
        }
    }
    return out;
}

// One-level A8R8G8B8 texture with the header of 'owner' (id, etc.)
static Texture *CreateArgbTexture(Texture *owner, unsigned int width, unsigned int height) {
    // mip levels down to 4 pixels (the renderer walks them in 16-byte units)
    unsigned int levels = 1;
    while (levels < 16 && (width >> levels) && (height >> levels) && ((width >> levels) * (height >> levels)) >= 4)
        levels++;
    size_t texelsSize = 0;
    for (unsigned int i = 0; i < levels; i++)
        texelsSize += (size_t)(width >> i) * (height >> i) * 4;
    size_t size = sizeof(Texture) + texelsSize;
    auto t = (Texture *)_aligned_malloc(size, 16);
    if (!t)
        return nullptr;
    memcpy(t, owner, sizeof(Texture));
    t->totalSize = (uint32_t)size;
    t->texelsOffset = (uint16_t)sizeof(Texture);
    t->clutOffset = 0;
    t->hasClut = 0;
    t->clutSizeQwc = 0;
    t->texelFormat = 0;
    t->platformFormat = PLATFORM_ARGB8888;
    t->platformFlags = 0;
    t->width = (uint16_t)width;
    t->height = (uint16_t)height;
    t->log2Width = Log2Ceil(width);
    t->log2Height = Log2Ceil(height);
    t->copyBufferWidth = t->width;
    t->copyBufferHeight = t->height;
    t->texelsSizeQwc = (uint32_t)((size_t)width * height * 4 / 16);
    t->vramPages_levels = (uint16_t)((levels - 1) << 12);
    memoryStats().textureCopiesSize += size;
    memoryStats().textureCopies++;
    return t;
}

// Make mip levels 1..n of an A8R8G8B8 texture from level 0 (2x2 average)
static void BuildArgbMips(Texture *t) {
    auto src = (uint32_t *)Texels(t);
    unsigned int w = t->width, h = t->height;
    for (unsigned int level = 1; level < NumLevels(t); level++) {
        uint32_t *dst = src + (size_t)w * h;
        unsigned int nw = max(1u, w / 2), nh = max(1u, h / 2);
        for (unsigned int y = 0; y < nh; y++) {
            for (unsigned int x = 0; x < nw; x++) {
                uint32_t c[4] = { src[(size_t)(2 * y) * w + 2 * x], src[(size_t)(2 * y) * w + 2 * x + 1],
                    src[(size_t)(2 * y + 1) * w + 2 * x], src[(size_t)(2 * y + 1) * w + 2 * x + 1] };
                uint32_t out = 0;
                for (int ch = 0; ch < 32; ch += 8)
                    out |= (((c[0] >> ch & 0xFF) + (c[1] >> ch & 0xFF) + (c[2] >> ch & 0xFF) + (c[3] >> ch & 0xFF) + 2) / 4) << ch;
                dst[(size_t)y * nw + x] = out;
            }
        }
        src = dst;
        w = nw;
        h = nh;
    }
}

static uint32_t BlendOver(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 255)
        return src;
    if (a == 0)
        return dst;
    uint32_t da = dst >> 24;
    uint32_t outA = a + da * (255 - a) / 255;
    if (!outA)
        return 0;
    uint32_t out = outA << 24;
    for (int ch = 0; ch < 24; ch += 8) {
        uint32_t sc = (src >> ch) & 0xFF, dc = (dst >> ch) & 0xFF;
        out |= ((sc * a + dc * da * (255 - a) / 255) / outA) << ch;
    }
    return out;
}

// A full-size layer over a texture (e.g. kit shorts and socks over the shirt), in 32 bits: every texture has its
// own palette (or none), any size/format. The result has the size of the bigger one.
static bool CompositeLayer(Texture *owner, Texture *layer) {
    Texture *base = Resolve(owner);
    vector<uint32_t> basePixels = DecodeTexture(base, false);
    vector<uint32_t> layerPixels = DecodeTexture(layer, true);
    if (basePixels.empty() || layerPixels.empty())
        return false;
    unsigned int width = max(base->width, layer->width);
    unsigned int height = max(base->height, layer->height);
    Texture *out = (base != owner && base->platformFormat == PLATFORM_ARGB8888 && base->width == width && base->height == height)
        ? base : CreateArgbTexture(owner, width, height);
    if (!out)
        return false;
    auto texels = (uint32_t *)Texels(out);
    for (unsigned int y = 0; y < height; y++) {
        unsigned int by = y * base->height / height, ly = y * layer->height / height;
        for (unsigned int x = 0; x < width; x++) {
            unsigned int bx = x * base->width / width, lx = x * layer->width / width;
            texels[(size_t)y * width + x] = BlendOver(basePixels[(size_t)by * base->width + bx],
                layerPixels[(size_t)ly * layer->width + lx]);
        }
    }
    BuildArgbMips(out);
    if (out != base)
        RegisterClone(owner, out, "layer over texture");
    else
        CallDynGlobal(gAddr.TexTBufRemove, owner->texNo);
    return true;
}

// --- boots ---
// The game puts 3 boot types side by side into one 512x256 paletted texture (1300+n, low-detail: 1303+n),
// one boot layer is 256x256 and its picture takes the first 170 columns. Boot types with a replaced texture
// (another format or bigger) get their own texture instead, with their own palette.
const int kBootTypes = 9;
const int kBootTextureId = 0x7E00;

struct BootAddresses {
    uintptr_t SetTexture;      // EntityModelSetTexture(entity, slot, texId)
    uintptr_t SetPalette;      // EntityModelSetPalette(entity, slot, texId)
    uintptr_t MoveTexMatrixUV; // EntityModelMoveTexMatrixUV(entity, slot, u, v)
};
constinit static BootAddresses gBootAddr = {};
constinit static Texture *gBootTextures[kBootTypes] = {};
constinit static float gTexMatrixScaleU = 0.0f; // set while the boot texture matrix is updated

// Original boot layers: paletted 256x256, colored by the palette of the player (the boot colors)
static bool IsCustomBootLayer(Texture *t) {
    return !IsPaletted(t) || t->width != 256 || t->height != 256;
}

static void SetBootTexture(int type, Texture *tex) {
    Texture *old = gBootTextures[type];
    gBootTextures[type] = tex;
    if (old) {
        CallAndReturnDynGlobal<int>(oTexDelOriginal(), old);
        FreeClone(old);
    }
    if (tex) {
        CallAndReturnDynGlobal<int>(oTexAddOriginal(), tex);
        LOG(LOG_LEVEL_2, "[TextureLimits] boot type %d: own texture %ux%u (format %u/%u)", type + 1,
            tex->width, tex->height, tex->texelFormat, tex->platformFormat);
    }
}

// --- hooks ------------------------------------------------------------------------------------------


// texels + palette
static void OnCopyTextureData(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    if (Texture *shadow = ShadowOf(src))
        src = shadow;
    src = Resolve(src);
    if (SameImage(dst, src))
        LimitLevels(dst, NumLevels(src));
    if (dst && src && !CanCopyTexels(dst, src) && IsReplacement(dst, src) && SetClone(dst, src, "CopyTextureData"))
        return;
    RemoveClone(dst);
    CallDynGlobal(oCopyTextureData, dst, src);
}

// texels only (palette is copied separately by the game, if needed)
static void OnCopyBaseTexels(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (SameImage(dst, src))
        LimitLevels(dst, NumLevels(src));
    if (dst && src && !CanCopyTexels(dst, src) && IsReplacement(dst, src) && SetClone(dst, src, "CopyBaseTexels"))
        return;
    RemoveClone(dst);
    CallDynGlobal(oCopyBaseTexels, dst, src);
}

static void OnSetImageFromMip(Texture *dst, Texture *src, int level) {
    dst = OwnerOf(dst);
    if (Texture *shadow = ShadowOf(src))
        src = shadow;
    src = Resolve(src);
    if (SameImage(dst, src, level))
        LimitLevels(dst, NumLevels(src) - level);
    if (dst && src && !CanCopyTexels(dst, src, level) && IsReplacement(dst, src, level) && SetClone(dst, src, "SetImageFromMip", level))
        return;
    RemoveClone(dst);
    CallDynGlobal(oSetImageFromMip, dst, src, level);
}

// Used for low-detail versions. The game can downscale a bigger paletted texture by itself.
static void OnCopyImageScaled(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (dst && src && IsPaletted(dst) && SameFormat(dst, src) && src->width >= dst->width && src->height >= dst->height) {
        RemoveClone(dst);
        LimitLevels(dst, 1);
        CallDynGlobal(oCopyImageScaled, dst, src);
        return;
    }
    // Not something the game can scale: use the mip levels of 'src', starting from the smallest one
    // which is still not smaller than 'dst' (no extra copy of the full-size texture)
    unsigned int level = 0;
    if (dst && src) {
        while (level + 1 < NumLevels(src) && (src->width >> (level + 1)) >= dst->width
            && (src->height >> (level + 1)) >= dst->height)
            level++;
    }
    if (IsReplacement(dst, src) && SetClone(dst, src, "CopyImageScaled", level))
        return;
    CallDynGlobal(oCopyImageScaled, dst, src);
}

// Re-index the texels of 't' (all mip levels), which were made for 'oldClut', to the nearest colors
// of its current palette
static void RemapTexels(Texture *t, uint32_t const *oldClut) {
    auto newClut = (uint32_t const *)TexAddressC(t);
    bool pal8 = t->texelFormat == TEX_PAL8;
    unsigned int numColors = pal8 ? 256 : 16;
    uint8_t lut[256];
    for (unsigned int v = 0; v < numColors; v++) {
        uint32_t c = oldClut[pal8 ? ClutPos(v) : v];
        unsigned int best = 0, bestDist = UINT_MAX;
        for (unsigned int w = 0; w < numColors && bestDist; w++) {
            uint32_t n = newClut[pal8 ? ClutPos(w) : w];
            unsigned int dist = 0;
            for (int ch = 0; ch < 32; ch += 8) {
                int d = (int)((c >> ch) & 0xFF) - (int)((n >> ch) & 0xFF);
                dist += d * d;
            }
            if (dist < bestDist) {
                bestDist = dist;
                best = w;
            }
        }
        lut[v] = (uint8_t)best;
    }
    uint8_t *texels = (uint8_t *)t + t->texelsOffset;
    size_t size = TexelsSize(t);
    for (size_t i = 0; i < size; i++)
        texels[i] = pal8 ? lut[texels[i]] : (uint8_t)(lut[texels[i] & 0xF] | (lut[texels[i] >> 4] << 4));
}

static void OnCopyTexturePalette(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (!dst || !src || HasClone(dst))
        return; // a clone has its own palette
    if (!dst->clutOffset || !src->clutOffset || !SameFormat(dst, src))
        return;
    // The game uses one palette for a texture and its low-detail version. A replaced (paletted) texture
    // comes with its own palette, so the low-detail version is re-indexed to it.
    bool remap = !IsUnusedLodTexture(dst->texNo) && (IsClone(src) || IsReplacedSource(src));
    uint32_t oldClut[256];
    size_t clutSize = dst->texelFormat == TEX_PAL8 ? 1024 : 64;
    if (remap)
        memcpy(oldClut, TexAddressC(dst), clutSize);
    CallDynGlobal(oCopyTexturePalette, dst, src);
    if (remap && memcmp(oldClut, TexAddressC(dst), clutSize) != 0) {
        RemapTexels(dst, oldClut);
        LOG(LOG_LEVEL_2, "[TextureLimits] texture %u: re-indexed to the palette of replaced texture %u", dst->texNo, src->texNo);
    }
}

static int OnTexAdd(Texture *t) {
    return CallAndReturnDynGlobal<int>(oTexAdd, Resolve(t));
}

static int OnTexDel(Texture *t) {
    Texture *owner = OwnerOf(t);
    auto it = Clones().find(owner);
    if (it == Clones().end())
        return CallAndReturnDynGlobal<int>(oTexDel, t);
    // Delete by the clone: its texture id is always valid, while the owner's header can be already
    // overwritten (e.g. the hair loader copies the new model into the slot before deleting the old texture).
    Texture *clone = it->second;
    Clones().erase(it);
    int result = CallAndReturnDynGlobal<int>(oTexDel, clone);
    FreeClone(clone);
    return result;
}

// Decals are drawn in texel coordinates of the original paletted texture
static void OnStampIndexedDecal(Texture *dst, Texture *src, int indexOffset, int x, int y, int16_t *rect) {
    dst = OwnerOf(dst);
    Texture *s = Resolve(src);
    if (!dst || !s || IsUnusedLodTexture(dst->texNo))
        return;
    Texture *target = Resolve(dst);
    // A full-size layer (PES5 kit: shorts and socks over the shirt): if the texture or the layer is replaced,
    // they are combined in 32 bits, so they don't need a common palette
    bool fullLayer = dst->texNo >= 4000 && dst->texNo < 4004 && x == 0 && y == 0 && rect[0] == 0 && rect[1] == 0 && rect[2] == dst->width && rect[3] == dst->height;
    bool layerReplaced = s->width != dst->width || s->height != dst->height || !SameFormat(s, dst);
    if (fullLayer && (target != dst || layerReplaced)) {
        CompositeLayer(dst, s);
        return;
    }
    if (target == dst) {
        // a bigger paletted source (e.g. an HD number font): the same in its resolution into a shadow of 'dst'
        Texture *srcOwner = OwnerOf(src);
        if (s != srcOwner && IsPaletted(s) && IsPaletted(dst) && s->texelFormat == dst->texelFormat && s->width > srcOwner->width) {
            float scale = (float)s->width / srcOwner->width;
            if (Texture *shadow = ShadowFor(dst, scale))
                ScaledStamp(shadow, scale, s, scale, indexOffset, x, y, rect);
        }
        LimitLevels(dst, NumLevels(s));
        CallDynGlobal(oStampIndexedDecal, dst, src, indexOffset, x, y, rect);
        return;
    }
    // a decal (e.g. a kit pattern) over a replaced paletted texture: in its resolution
    if (!IsPaletted(target) || !IsPaletted(s) || target->texelFormat != s->texelFormat)
        return;
    ScaledStamp(target, (float)target->width / dst->width, s, 1.0f, indexOffset, x, y, rect);
    // the mip levels are made by the renderer from the stamped picture
    target->vramPages_levels &= 0x0FFF;
    CallDynGlobal(gAddr.TexTBufRemove, dst->texNo);
}

static void OnFindAndStampRowMarker(Texture *dst, Texture *src, int x, int y, int indexOffset) {
    Texture *owner = OwnerOf(dst);
    Texture *s = Resolve(src);
    // boot layers stamped into the shared boot textures: column = x / 170 (low-detail: x / 42)
    if (owner && s && owner->texNo >= 1300 && owner->texNo < 1306) {
        bool main = owner->texNo < 1303;
        int type = (int)(owner->texNo - (main ? 1300 : 1303)) * 3 + x / (main ? 170 : 42);
        if (type >= 0 && type < kBootTypes) {
            if (IsCustomBootLayer(s)) {
                if (main)
                    SetBootTexture(type, CreateClone(s, kBootTextureId + type));
                return;
            }
            if (main && gBootTextures[type])
                SetBootTexture(type, nullptr);
        }
    }
    if (IsClone(dst) || HasClone(dst) || IsClone(src) || HasClone(src))
        return;
    if (dst && src)
        LimitLevels(dst, NumLevels(src));
    CallDynGlobal(oFindAndStampRowMarker, dst, src, x, y, indexOffset);
}

// Boot texture of a player: the game binds the shared texture and moves the texture coordinates to the column
// of the boot type. With an own texture of the boot type: bind it and scale the coordinates (512 / 256).
static void OnBootBind(uint8_t *actor) {
    gTexMatrixScaleU = 1.0f;
    CallDynGlobal(oBootBind, actor);
    gTexMatrixScaleU = 0.0f;
    int type = actor[62];
    void *entity = *(void **)(actor + 16);
    if (!entity || type >= kBootTypes || !gBootTextures[type])
        return;
    int texId = kBootTextureId + type;
    CallDynGlobal(gBootAddr.SetTexture, entity, 112, texId);
    CallDynGlobal(gBootAddr.SetPalette, entity, 112, texId);
    gTexMatrixScaleU = 512.0f / 256.0f;
    CallDynGlobal(gBootAddr.MoveTexMatrixUV, entity, 112, 0.0f, 0.0f);
    gTexMatrixScaleU = 0.0f;
}

static float *OnGetTexMatrix(int data, void *model, int slot) {
    float *m = CallAndReturnDynGlobal<float *>(oGetTexMatrix, data, model, slot);
    if (m && slot == 112 && gTexMatrixScaleU != 0.0f)
        m[0] = gTexMatrixScaleU;
    return m;
}

// Palette operations (kit colours) - a clone may have no palette
static void OnCopyClutRange(Texture *dst, int dstStart, Texture *src, int srcStart, int count) {
    dst = ResolvePalette(dst);
    src = ResolvePalette(src);
    // only paletted textures (a 32-bit texture can have a palette offset, but the game reads its palette as null)
    if (!dst || !src || !IsPaletted(dst) || !IsPaletted(src) || !TexAddressC(dst) || !TexAddressC(src))
        return;
    CallDynGlobal(oCopyClutRange, dst, dstStart, src, srcStart, count);
}

static void OnTintClutRange(Texture *dst, Texture *src, int color, int start, int count) {
    dst = ResolvePalette(dst);
    src = ResolvePalette(src);
    // only paletted textures (a 32-bit texture can have a palette offset, but the game reads its palette as null)
    if (!dst || !src || !IsPaletted(dst) || !IsPaletted(src) || !TexAddressC(dst) || !TexAddressC(src))
        return;
    CallDynGlobal(oTintClutRange, dst, src, color, start, count);
}

// Texture ids for which the game gets the replacement (while a kit name is drawn in high resolution)
constinit static int gHdTextureIds[2] = { -1, -1 };

// The game always gets its own texture by the id, never a clone: some code frees what it gets
// (TexDel + ArenaFree, e.g. the low-detail kit texture when leaving a match)
static Texture *OnGetTextureAssetIfLoaded(int texId) {
    Texture *t = OwnerOf(CallAndReturnDynGlobal<Texture *>(oGetTextureAssetIfLoaded, texId));
    if (t && (texId == gHdTextureIds[0] || texId == gHdTextureIds[1]))
        return Resolve(t);
    return t;
}

// A new texture at this address: a shadow of a previous one is not valid anymore
static uintptr_t oTextureCreate;

static Texture *OnTextureCreate(Texture *t, int texId, int format, int width, int height) {
    if (t)
        DropShadow(t);
    Texture *result = CallAndReturnDynGlobal<Texture *>(oTextureCreate, t, texId, format, width, height);
    DropShadow(result);
    return result;
}

// --- kit names ---
// The name is drawn letter by letter (on an arc) from the name font into the name texture of the player
// (256x64, 4-bit), with the letter rectangles and the kerning from tables in pixels of the original font.
// With a bigger font the game does the same in its resolution: the tables, the name texture and the width
// limit are scaled for the time of drawing.
const int kNumGlyphs = 39;
struct GlyphTables { uintptr_t glyphs; uintptr_t kerning; };
constinit static GlyphTables gGlyphTables = {};
static uintptr_t oDrawKitName;

static Texture *TextureOfId(int texId) {
    auto pt = AcquirePlatformTexture(texId);
    Texture *t = pt->pTexture;
    ReleasePlatformTexture(pt);
    return t;
}

static int OnDrawKitName(int levels, int targetId, char *name, int fontId, int arc, int maxWidth) {
    Texture *fontOwner = OwnerOf(TextureOfId(fontId));
    Texture *font = fontOwner ? Resolve(fontOwner) : nullptr;
    Texture *target = OwnerOf(TextureOfId(targetId));
    bool hd = font && target && font != fontOwner && IsPaletted(font) && font->texelFormat == fontOwner->texelFormat
        && IsPaletted(target) && font->width > fontOwner->width;
    if (!hd) {
        if (target)
            RemoveClone(target); // an HD name of a previous kit
        return CallAndReturnDynGlobal<int>(oDrawKitName, levels, targetId, name, fontId, arc, maxWidth);
    }
    float scale = (float)font->width / fontOwner->width;
    Texture *hdTarget = CreatePalettedTexture(target, (unsigned int)lround(target->width * scale),
        (unsigned int)lround(target->height * scale));
    if (!hdTarget)
        return CallAndReturnDynGlobal<int>(oDrawKitName, levels, targetId, name, fontId, arc, maxWidth);
    RegisterClone(target, hdTarget, "kit name");
    // scale the tables
    auto glyphs = (int16_t *)gGlyphTables.glyphs;
    auto kerning = (uint8_t *)gGlyphTables.kerning;
    int16_t savedGlyphs[kNumGlyphs * 4];
    uint8_t savedKerning[256];
    memcpy(savedGlyphs, glyphs, sizeof(savedGlyphs));
    size_t kerningSize = 0;
    while (kerningSize + 3 < sizeof(savedKerning) && kerning[kerningSize])
        kerningSize += 3;
    memcpy(savedKerning, kerning, kerningSize);
    for (int i = 0; i < kNumGlyphs * 4; i++)
        glyphs[i] = (int16_t)lround(glyphs[i] * scale);
    for (size_t i = 0; i < kerningSize; i += 3)
        kerning[i + 2] = (uint8_t)min(255L, lround(kerning[i + 2] * scale));
    gHdTextureIds[0] = fontId;
    gHdTextureIds[1] = targetId;
    int result = CallAndReturnDynGlobal<int>(oDrawKitName, levels, targetId, name, fontId, arc, (int)lround(maxWidth * scale));
    gHdTextureIds[0] = gHdTextureIds[1] = -1;
    memcpy(glyphs, savedGlyphs, sizeof(savedGlyphs));
    memcpy(kerning, savedKerning, kerningSize);
    MirrorToOwner(target, hdTarget);
    return result;
}

// Model texture slot assignment: SetTextureNoPalette (the palette follows the texture) and ModelChangeTexture
static uintptr_t oModelSetTexture, oModelChangeTexture;

static void OnModelSetTexture(int data, void *model, int slot, int texId) {
    CallDynGlobal(oModelSetTexture, data, model, slot, MainTextureId(texId));
}

static void OnModelChangeTexture(int data, void *model, int slot, int texId) {
    CallDynGlobal(oModelChangeTexture, data, model, slot, MainTextureId(texId));
}

// Mip level bias: the game uses -3 by default (much sharper than normal, mip levels are almost never used),
// and some models set their own value. MIPMAP_LOD_BIAS replaces the default and shifts the model values.
constinit static float gMipBiasShift = 0.0f;

static HRESULT __stdcall OnSetModelMipBias(void *device, DWORD stage, DWORD state, DWORD value) {
    float bias = *(float *)&value + gMipBiasShift;
    using Fn = HRESULT(__stdcall *)(void *, DWORD, DWORD, DWORD);
    auto setTextureStageState = (Fn)(*(void ***)device)[63];
    return setTextureStageState(device, stage, state, *(DWORD *)&bias);
}

static void InstallMipBias(uintptr_t defaultBias, uintptr_t modelBiasCall) {
    string const &value = config().MIPMAP_LOD_BIAS;
    if (value.empty())
        return;
    float bias;
    try {
        bias = std::stof(value);
    }
    catch (...) {
        LOG(LOG_LEVEL_1, "[TextureLimits] invalid MIPMAP_LOD_BIAS \"%s\"", value.c_str());
        return;
    }
    // call dword ptr [ecx + 0FCh] (IDirect3DDevice8::SetTextureStageState)
    if (patch::GetBytes(modelBiasCall, 6) != "FF 91 FC 00 00 00") {
        LOG(LOG_LEVEL_1, "[TextureLimits] unexpected code at %X, MIPMAP_LOD_BIAS is not applied", modelBiasCall);
        return;
    }
    gMipBiasShift = bias - *(float *)defaultBias;
    patch::SetFloat(defaultBias, bias);
    patch::Nop(modelBiasCall, 6);
    patch::RedirectCall(modelBiasCall, OnSetModelMipBias);
    LOG(LOG_LEVEL_1, "[TextureLimits] mip level bias: %.2f (models: %+.2f)", bias, gMipBiasShift);
}

// --- install ----------------------------------------------------------------------------------------

// Verify the prologues and create trampolines; a hook is installed only if all of them are fine
static uintptr_t Detour(uintptr_t addr, char const *prologue, bool &ok) {
    if (!ok)
        return 0;
    uintptr_t tramp = MakeTrampoline(addr, prologue);
    if (!tramp) {
        LOG(LOG_LEVEL_1, "[TextureLimits] unexpected code at %X, texture hooks are not installed", addr);
        ok = false;
    }
    return tramp;
}

void Install(PES::Version v) {
    // function addresses, in the order of 'hooks' below
    uintptr_t const kPES5[] = { 0x828F30, 0x828F10, 0x828DD0, 0x826C80, 0x828DB0, 0x8D57A0, 0x8D57D0, 0x825D50, 0x826660, 0x825C50, 0x8262A0, 0x8D5600, 0x8D9AE0, 0x8D9B40, 0xACADA0, 0x8D7C80, 0x825B40, 0x852350 };
    uintptr_t const kPES6[] = { 0x8D1A60, 0x8D1A40, 0x8D1900, 0x8CF9E0, 0x8D18E0, 0x87A810, 0x87A840, 0x8CEAA0, 0x8CF3B0, 0x8CE9A0, 0x8CEFF0, 0x87A670, 0xB41D70, 0xB41DD0, 0xB64320, 0x87C100, 0x8CE890, 0x8B7030 };
    uintptr_t const *a;
    switch (v.id()) {
    case ID_PES5:
        gAddr = { 0x429600, 0x429370, 0x4271C0, 0x4270B0, 0x428A50 };
        a = kPES5;
        gBootAddr = { 0x8545E0, 0x854600, 0x8547B0 };
        gGlyphTables = { 0xC2E640, 0xC2E5E0 };
        InstallMipBias(0xB0E0C0, 0x41AA7D);
        break;
    case ID_PES6:
        gAddr = { 0x406740, 0x4064B0, 0x409F80, 0x409E70, 0x405BD0 };
        a = kPES6;
        gBootAddr = { 0x8AE130, 0x8AE150, 0x8AE320 };
        gGlyphTables = { 0xD3C2A0, 0xD3C240 };
        InstallMipBias(0xBC4920, 0x87B77E);
        break;
    default:
        return;
    }
    struct Hook { uintptr_t addr; void *hook; char const *prologue; uintptr_t *orig; };
    Hook hooks[] = {
        { a[0],  OnCopyTextureData,       "56 8B 74 24 0C",             &oCopyTextureData },
        { a[1],  OnCopyBaseTexels,        "8B 44 24 08 50",             &oCopyBaseTexels },
        { a[2],  OnSetImageFromMip,       "53 8B 5C 24 0C",             &oSetImageFromMip },
        { a[3],  OnCopyImageScaled,       "83 EC 34 53 8B 5C 24 3C",    &oCopyImageScaled },
        { a[4],  OnCopyTexturePalette,    "8B 44 24 08 50",             &oCopyTexturePalette },
        { a[5],  OnTexAdd,                "8B 44 24 04 8B 48 0C",       &oTexAdd },
        { a[6],  OnTexDel,                "8B 44 24 04 85 C0",          &oTexDel },
        { a[7],  OnStampIndexedDecal,     "83 EC 34 8B 44 24 4C",       &oStampIndexedDecal },
        { a[8],  OnFindAndStampRowMarker, "83 EC 2C 53 55",             &oFindAndStampRowMarker },
        { a[9],  OnCopyClutRange,         "8B 4C 24 14 83 EC 08",       &oCopyClutRange },
        { a[10], OnTintClutRange,         "8B 44 24 14 53 8B 5C 24 14", &oTintClutRange },
        { a[11], OnGetTextureAssetIfLoaded, "8B 44 24 04 50",           &oGetTextureAssetIfLoaded },
        { a[12], OnModelSetTexture,       "8B 44 24 0C 8B 4C 24 04",    &oModelSetTexture },
        { a[13], OnModelChangeTexture,    "8B 44 24 0C 8B 4C 24 04",    &oModelChangeTexture },
        { a[14], OnBootBind,              "83 EC 08 53 55 56 57",       &oBootBind },
        { a[15], OnGetTexMatrix,          "8B 44 24 0C 8B 4C 24 04",    &oGetTexMatrix },
        { a[16], OnTextureCreate,         "8B 44 24 14 8B 4C 24 08 8B 54 24 04", &oTextureCreate },
        { a[17], OnDrawKitName,           "55 8B EC 83 E4 F0 81 EC A4 00 00 00", &oDrawKitName },
    };
    // verify everything first, so we never end up with a part of the hooks installed
    bool ok = true;
    for (auto const &h : hooks)
        *h.orig = Detour(h.addr, h.prologue, ok);
    if (!ok)
        return;
    for (auto const &h : hooks)
        patch::RedirectJump(h.addr, h.hook);
    gLodMainTexture = config().LOD_MAIN_TEXTURE != 0;
}

}

void InitTextureLimits(PES::Version v) {
    TextureLimits::Install(v);
}
