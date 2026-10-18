#pragma once
#include "TextureLimits.hpp"

namespace FlagReplacer {

// A picture file (PNG, or DDS: DXT1/DXT3/DXT5/32-bit, used as it is) as a texture of the plugin with this id, added
// to the game (DeleteOwnTexture frees it). width, height: the size of the picture; a PNG whose sizes aren't powers
// of two is put as it is in the top left corner of the texture (the rest repeats its last column and row), so the
// picture is texels 0..width, 0..height of it (not resized: no filter).
TextureLimits::Texture *LoadPictureTexture(std::wstring const &path, uint32_t texNo, bool mips, unsigned int &width,
    unsigned int &height);

}
