#pragma once
#include "Main.hpp"

// The game's 2D render list (menus): every frame each visible item adds a render node to the list of its layer
// (Submit(node, layer 0..3, size, key)); then the list is sorted by the key (a float, bigger first: smaller keys are
// in front, equal keys keep their order) and each node's callback draws its object.
// Layout objects show one asset of a layout file (.opd); their callback is the game's layout draw.
namespace Ui {

#pragma pack(push, 1)
struct RenderNode {
    float key;
    void *object;
    void (__cdecl *callback)(void *object);
    void *link[2];
    uint8_t layer;
    uint8_t pad[3];
    int size;
    void *sortTemp;
};
#pragma pack(pop)
static_assert(sizeof(RenderNode) == 32, "RenderNode size");

// before a node is added: false - it isn't added (not drawn)
using BeforeSubmit = bool (*)(RenderNode *node, int layer, int size, float key);
// after a node is added (more nodes can be added with Submit: drawn after it with the same key)
using AfterSubmit = void (*)(RenderNode *node, int layer, int size, float key);

bool Install(PES::Version v);
bool IsInstalled();
// the frames of the menus (counts the frames in which something is added to the lists)
unsigned int Frame();
void AddBeforeSubmit(BeforeSubmit callback);
void AddAfterSubmit(AfterSubmit callback);
// adds a node to the list (not seen by the observers)
void Submit(RenderNode *node, int layer, int size, float key);

// layout objects
bool IsLayoutObject(RenderNode const *node);
uint32_t LayoutFile(uint8_t const *object);         // OPD file id (archive << 16 | index), 0: none
int LayoutAsset(uint8_t const *object);
struct Box {
    float left, top, right, bottom;     // screen pixels
    int z;                              // GS depth of the first vertex (bigger: nearer)
    int minZ, maxZ;
};
bool LayoutBounds(uint8_t const *object, Box &box);  // the drawn meshes of the object
unsigned int LayoutAlpha(uint8_t const *object);     // 0..128 (PS2 alpha)

// panels (popup boxes: 9 quads of a 4 x 4 grid of vertices, drawn with a color factor)
bool IsPanel(RenderNode const *node);
bool PanelBounds(uint8_t const *panel, Box &box);
unsigned int PanelAlpha(uint8_t const *panel);       // 0..128

// GS coordinates (1/16 pixel, the 512x448 screen at 1792, 1824) to screen pixels, as the game draws them
float GsXToScreen(int x);
float GsYToScreen(int y);
float GsDepth(int z);
float ScreenPixelsPerGsLine();      // vertical scale
float ScreenPixelsPerGsPixel();     // horizontal scale
float ScreenCenterX();

// drawing in a render node callback: textured quads like the game's menus
void BeginQuads();
// color: R, G, B, A in PS2 range (0x80 = 1.0)
// depthTest false: drawn over what is there (e.g. 3D models in front of the menus); u1, v1: the right / bottom edge of
// the part of the texture drawn (from its top left corner)
void DrawQuad(int texId, float left, float top, float right, float bottom, float depth, uint32_t color,
    bool depthTest = true, float u1 = 1.0f, float v1 = 1.0f);
void EndQuads();
// the depth of a rectangle written (nothing drawn: no color, transparent): what is drawn later in front of that depth
// covers what is there (3D)
void DrawDepth(int texId, float left, float top, float right, float bottom, float depth);

}
