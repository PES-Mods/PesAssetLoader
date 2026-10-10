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
#pragma pack(pop)

const uint8_t TEX_PAL8 = 19;
const uint8_t TEX_PAL4 = 20;

struct GameAddresses {
    uintptr_t GetPlatformTexture;  // PlatformTexture *(int texId), increments refCount
    uintptr_t PlatformTextureDestroy;
    uintptr_t TexTBufRemove;       // schedules re-creation of the D3D texture
    uintptr_t TexAddressC;
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

static bool IsClone(Texture *t) { return t && Owners().contains(t); }

// A clone can be passed instead of the owner when the game gets the texture by its id
static Texture *OwnerOf(Texture *t) {
    auto it = Owners().find(t);
    return it != Owners().end() ? it->second : t;
}

static Texture *Resolve(Texture *t) {
    auto it = Clones().find(t);
    return it != Clones().end() ? it->second : t;
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

static Texture *CreateClone(Texture *src, uint32_t texNo) {
    size_t size = BlockSize(src);
    auto clone = (Texture *)_aligned_malloc(size, 16);
    if (!clone)
        return nullptr;
    memcpy(clone, src, size);
    clone->texNo = texNo;
    clone->totalSize = (uint32_t)size;
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
    if (pt->bCreated && pt->pTexture == from) {
        pt->pTexture = to;
        CallDynGlobal(gAddr.TexTBufRemove, texNo);
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
static bool SetClone(Texture *owner, Texture *src, char const *reason) {
    // id 1 is shared by temporary textures (e.g. when kit patterns are applied)
    if (!owner || !src || owner->texNo <= 1)
        return false;
    Texture *clone = CreateClone(src, owner->texNo);
    if (!clone)
        return false;
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
    return true;
}

// Can the game copy texels of 'src' (starting from mip level 'level') into 'dst'
static bool CanCopyTexels(Texture *dst, Texture *src, unsigned int level = 0) {
    return dst->platformFormat == 0 && SameFormat(dst, src)
        && (src->width >> level) == dst->width && (src->height >> level) == dst->height
        && NumLevels(src) >= NumLevels(dst) + level;
}

// --- hooks ------------------------------------------------------------------------------------------

static uintptr_t oCopyTextureData, oCopyBaseTexels, oSetImageFromMip, oCopyImageScaled, oCopyTexturePalette;
static uintptr_t oTexAdd, oTexDel, oStampIndexedDecal, oFindAndStampRowMarker, oCopyClutRange, oTintClutRange;

// texels + palette
static void OnCopyTextureData(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (dst && src && !CanCopyTexels(dst, src) && SetClone(dst, src, "CopyTextureData"))
        return;
    RemoveClone(dst);
    CallDynGlobal(oCopyTextureData, dst, src);
}

// texels only (palette is copied separately by the game, if needed)
static void OnCopyBaseTexels(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (dst && src && !CanCopyTexels(dst, src) && SetClone(dst, src, "CopyBaseTexels"))
        return;
    RemoveClone(dst);
    CallDynGlobal(oCopyBaseTexels, dst, src);
}

static void OnSetImageFromMip(Texture *dst, Texture *src, int level) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (dst && src && !CanCopyTexels(dst, src, level) && SetClone(dst, src, "SetImageFromMip"))
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
        CallDynGlobal(oCopyImageScaled, dst, src);
        return;
    }
    if (SetClone(dst, src, "CopyImageScaled"))
        return;
    CallDynGlobal(oCopyImageScaled, dst, src);
}

static void OnCopyTexturePalette(Texture *dst, Texture *src) {
    dst = OwnerOf(dst);
    src = Resolve(src);
    if (!dst || !src || Clones().contains(dst))
        return; // a clone has its own palette
    if (!dst->clutOffset || !src->clutOffset || !SameFormat(dst, src))
        return;
    CallDynGlobal(oCopyTexturePalette, dst, src);
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
static void OnStampIndexedDecal(Texture *dst, Texture *src, int indexOffset, int x, int y, void *rect) {
    if (IsClone(dst) || IsClone(src))
        return;
    CallDynGlobal(oStampIndexedDecal, dst, src, indexOffset, x, y, rect);
}

static void OnFindAndStampRowMarker(Texture *dst, Texture *src, int a3, int a4, int a5) {
    if (IsClone(dst) || IsClone(src))
        return;
    CallDynGlobal(oFindAndStampRowMarker, dst, src, a3, a4, a5);
}

// Palette operations (kit colours) - a clone may have no palette
static void OnCopyClutRange(Texture *dst, int dstStart, Texture *src, int srcStart, int count) {
    if (!dst || !src || !TexAddressC(dst) || !TexAddressC(src))
        return;
    CallDynGlobal(oCopyClutRange, dst, dstStart, src, srcStart, count);
}

static void OnTintClutRange(Texture *dst, Texture *src, int color, int start, int count) {
    if (!dst || !src || !TexAddressC(dst) || !TexAddressC(src))
        return;
    CallDynGlobal(oTintClutRange, dst, src, color, start, count);
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
    uintptr_t const kPES5[] = { 0x828F30, 0x828F10, 0x828DD0, 0x826C80, 0x828DB0, 0x8D57A0, 0x8D57D0, 0x825D50, 0x826660, 0x825C50, 0x8262A0 };
    uintptr_t const kPES6[] = { 0x8D1A60, 0x8D1A40, 0x8D1900, 0x8CF9E0, 0x8D18E0, 0x87A810, 0x87A840, 0x8CEAA0, 0x8CF3B0, 0x8CE9A0, 0x8CEFF0 };
    uintptr_t const *a;
    switch (v.id()) {
    case ID_PES5:
        gAddr = { 0x429600, 0x429370, 0x4271C0, 0x4270B0 };
        a = kPES5;
        break;
    case ID_PES6:
        gAddr = { 0x406740, 0x4064B0, 0x409F80, 0x409E70 };
        a = kPES6;
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
    };
    // verify everything first, so we never end up with a part of the hooks installed
    bool ok = true;
    for (auto const &h : hooks)
        *h.orig = Detour(h.addr, h.prologue, ok);
    if (!ok)
        return;
    for (auto const &h : hooks)
        patch::RedirectJump(h.addr, h.hook);
}

}

void InitTextureLimits(PES::Version v) {
    TextureLimits::Install(v);
}
