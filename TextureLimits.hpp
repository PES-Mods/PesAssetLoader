#pragma once
#include "Main.hpp"
#include <vector>

// Textures: what other features use of TextureLimits (the game's texture header, 32-bit textures, clones)
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

const uint8_t TEX_PAL8 = 19;
const uint8_t TEX_PAL4 = 20;
// Platform format 6: D3DFMT_A8R8G8B8 texels (B, G, R, A in memory, alpha 0..255), uploaded as they are
const uint8_t PLATFORM_ARGB8888 = 6;

// the texture hooks are installed (clones are freed with their textures)
bool HooksInstalled();
uint8_t *Texels(Texture *t);
// level 0 as A8R8G8B8 pixels (paletted, PS2 RGBA, DXT1/3/5, A8R8G8B8); empty if the format is not supported
std::vector<uint32_t> DecodeTexture(Texture *t, bool indexZeroTransparent);
// one-level header + all mip levels of an A8R8G8B8 texture with the header (id, ...) of 'owner'
Texture *CreateArgbTexture(Texture *owner, unsigned int width, unsigned int height);
// mip levels 1..n of an A8R8G8B8 texture from level 0
void BuildArgbMips(Texture *t);
// 'clone' is used in place of 'owner' (bound to its texture id, freed when the game deletes the texture)
void RegisterClone(Texture *owner, Texture *clone, char const *reason);
// the texture bound to the id (the clone of a texture that has one), or null
Texture *FindTexture(int texId);
// a texture of the plugin's own (allocated like a clone) added to / deleted from the game
void AddOwnTexture(Texture *t);
void DeleteOwnTexture(Texture *t);
// the game's texture reference of a texture id (PlatformTexture *: +0 texture id), counted; and its release
void *AcquireTextureRef(int texId);
void ReleaseTextureRef(void *ref);

}
