#include "Ui.hpp"
#include <vector>

namespace Ui {

struct Addresses {
    uintptr_t submit;           // Submit(node, layer, size, key)
    uintptr_t layoutDraw;       // the render callback of layout objects
    uintptr_t gsX, gsY, gsZ;    // GS x / y (uint16) to screen pixels, GS depth to the z buffer
    uintptr_t getDevice;        // IDirect3DDevice8 *()
    uintptr_t setRenderMode;    // (int mode): 0 - menus, 6 - texture * vertex color
    uintptr_t setAddressMode;   // (uint64_t): texture address mode of stage 0
    uintptr_t setTexture;       // SetCurrentTexturePalette(texture id, palette id)
    uintptr_t layerLists;       // the render lists of the layers (36 bytes, +0 number of nodes)
    uintptr_t panelDraw;        // the render callback of panels
    bool pes6;                  // layout object color: PES5 +84 color, +150 alpha; PES6 +84..87 color * floats +92..104
};
constinit static Addresses gAddr = {};
constinit static uintptr_t oSubmit = 0;
constinit static bool gInstalled = false;
constinit static unsigned int gFrame = 0;

static vector<BeforeSubmit> &BeforeObservers() {
    static vector<BeforeSubmit> observers;
    return observers;
}

static vector<AfterSubmit> &AfterObservers() {
    static vector<AfterSubmit> observers;
    return observers;
}

static int __cdecl OnSubmit(RenderNode *node, int layer, int size, int key) {
    float k = *(float *)&key;
    if (node && size) {
        // all lists empty (drawn): a new frame
        int queued = 0;
        for (unsigned int i = 0; i < 4; i++)
            queued += *(int *)(gAddr.layerLists + 36 * i);
        if (!queued)
            gFrame++;
        for (BeforeSubmit b : BeforeObservers()) {
            if (!b(node, layer & 0xFF, size, k))
                return 1;
        }
    }
    int result = CallAndReturnDynGlobal<int>(oSubmit, node, layer, size, key);
    if (result) {
        for (AfterSubmit a : AfterObservers())
            a(node, layer & 0xFF, size, k);
    }
    return result;
}

bool Install(PES::Version v) {
    if (gInstalled)
        return true;
    switch (v.id()) {
    case ID_PES5:
        gAddr = { 0x59E800, 0x5ADF50, 0x4194B0, 0x419530, 0x4195B0, 0x412560, 0x4194A0, 0x42B010, 0x8D5720, 0xFE59A8,
            0x5AE960, false };
        break;
    case ID_PES6:
        gAddr = { 0x94F030, 0x95CF10, 0x87AEB0, 0x87AF30, 0x87AFB0, 0x4059A0, 0x87AEA0, 0x40D230, 0x87A790, 0x3A67BB0,
            0x95B7E0, true };
        break;
    default:
        return false;
    }
    // mov ecx, [esp+4]; test ecx, ecx
    oSubmit = MakeTrampoline(gAddr.submit, "8B 4C 24 04 85 C9");
    if (!oSubmit) {
        LOG(LOG_LEVEL_1, "[Ui] unexpected code at %X, the menu hooks are not installed", gAddr.submit);
        return false;
    }
    patch::RedirectJump(gAddr.submit, OnSubmit);
    gInstalled = true;
    return true;
}

bool IsInstalled() {
    return gInstalled;
}

unsigned int Frame() {
    return gFrame;
}

void AddBeforeSubmit(BeforeSubmit callback) {
    BeforeObservers().push_back(callback);
}

void AddAfterSubmit(AfterSubmit callback) {
    AfterObservers().push_back(callback);
}

void Submit(RenderNode *node, int layer, int size, float key) {
    CallAndReturnDynGlobal<int>(oSubmit, node, layer, size, *(int *)&key);
}

bool IsLayoutObject(RenderNode const *node) {
    return node && node->object && (uintptr_t)node->callback == gAddr.layoutDraw;
}

// Layout object: +28 the current frame of the asset (+0 number of meshes, +12 the meshes: +4 number of vertices),
// +40 the meshes as drawn (20 bytes: +4 positions: x, y, z, w (GS integers), +18 hidden), +44 the OPD file's entry
// (+8 file id), +68 the asset
uint32_t LayoutFile(uint8_t const *object) {
    auto entry = *(uint8_t const *const *)(object + 44);
    return entry ? *(uint32_t const *)(entry + 8) : 0;
}

int LayoutAsset(uint8_t const *object) {
    return *(int const *)(object + 68);
}

bool LayoutBounds(uint8_t const *object, Box &box) {
    auto frame = *(uint8_t const *const *)(object + 28);
    auto meshes = *(uint8_t const *const *)(object + 40);
    if (!frame || !meshes)
        return false;
    auto meshInfo = *(uint8_t const *const *const *)(frame + 12);
    bool any = false;
    int minX = 0, minY = 0, maxX = 0, maxY = 0, minZ = 0, maxZ = 0;
    for (unsigned int i = 0; i < frame[0]; i++) {
        uint8_t const *mesh = meshes + 20 * i;
        auto positions = *(int const *const *)(mesh + 4);
        if (mesh[18] || !positions || !meshInfo || !meshInfo[i])
            continue;
        unsigned int count = *(uint16_t const *)(meshInfo[i] + 4);
        for (unsigned int j = 0; j < count; j++) {
            int x = positions[4 * j], y = positions[4 * j + 1], z = positions[4 * j + 2];
            if (!any) {
                minX = maxX = x;
                minY = maxY = y;
                minZ = maxZ = box.z = z;
                any = true;
            }
            minX = min(minX, x);
            maxX = max(maxX, x);
            minY = min(minY, y);
            maxY = max(maxY, y);
            minZ = min(minZ, z);
            maxZ = max(maxZ, z);
        }
    }
    if (!any)
        return false;
    box.left = GsXToScreen(minX);
    box.right = GsXToScreen(maxX);
    box.top = GsYToScreen(minY);
    box.bottom = GsYToScreen(maxY);
    box.minZ = minZ;
    box.maxZ = maxZ;
    return true;
}

unsigned int LayoutAlpha(uint8_t const *object) {
    if (gAddr.pes6) {
        float a = object[87] * *(float const *)(object + 104);
        return (unsigned int)std::clamp(a, 0.0f, 128.0f);
    }
    return (*(uint32_t const *)(object + 84) >> 24) * object[150] / 255;
}

// Panel: +2 shown, +0x53..0x56 the color factor (R, G, B, A; 0..255), +0x158 the 16 vertices (x, y, z, w: GS
// integers)
bool IsPanel(RenderNode const *node) {
    return node && node->object && (uintptr_t)node->callback == gAddr.panelDraw;
}

bool PanelBounds(uint8_t const *panel, Box &box) {
    if (!panel[2])
        return false;
    auto v = (int const *)(panel + 0x158);
    int minX = v[0], maxX = v[0], minY = v[1], maxY = v[1], minZ = v[2], maxZ = v[2];
    for (unsigned int i = 1; i < 16; i++) {
        minX = min(minX, v[4 * i]);
        maxX = max(maxX, v[4 * i]);
        minY = min(minY, v[4 * i + 1]);
        maxY = max(maxY, v[4 * i + 1]);
        minZ = min(minZ, v[4 * i + 2]);
        maxZ = max(maxZ, v[4 * i + 2]);
    }
    box.left = GsXToScreen(minX);
    box.right = GsXToScreen(maxX);
    box.top = GsYToScreen(minY);
    box.bottom = GsYToScreen(maxY);
    box.z = v[2];
    box.minZ = minZ;
    box.maxZ = maxZ;
    return box.right > box.left && box.bottom > box.top;
}

unsigned int PanelAlpha(uint8_t const *panel) {
    return panel[0x56] / 2;
}

float GsXToScreen(int x) {
    return (float)CallAndReturnDynGlobal<double>(gAddr.gsX, x);
}

float GsYToScreen(int y) {
    return (float)CallAndReturnDynGlobal<double>(gAddr.gsY, y);
}

float GsDepth(int z) {
    return (float)CallAndReturnDynGlobal<double>(gAddr.gsZ, z);
}

float ScreenPixelsPerGsLine() {
    return (GsYToScreen((1824 + 448) * 16) - GsYToScreen(1824 * 16)) / 448.0f;
}

float ScreenPixelsPerGsPixel() {
    return (GsXToScreen((1792 + 512) * 16) - GsXToScreen(1792 * 16)) / 512.0f;
}

float ScreenCenterX() {
    return GsXToScreen((1792 + 256) * 16);
}

// IDirect3DDevice8 methods (vtable indices)
enum { D3D_SET_RENDER_STATE = 50, D3D_GET_RENDER_STATE = 51, D3D_DRAW_PRIMITIVE_UP = 72, D3D_SET_VERTEX_SHADER = 76 };
enum { D3DRS_ZENABLE = 7 };

static void **Device() {
    return CallAndReturnDynGlobal<void **>(gAddr.getDevice);
}

template<typename... Args>
static int DeviceCall(unsigned int index, Args... args) {
    void **device = Device();
    auto vtable = *(void ***)device;
    return ((int (__stdcall *)(void **, Args...))vtable[index])(device, args...);
}

void BeginQuads() {
    DeviceCall(D3D_SET_VERTEX_SHADER, 0x144u);     // XYZRHW | DIFFUSE | TEX1
    CallDynGlobal(gAddr.setRenderMode, 0);
    CallDynGlobal(gAddr.setRenderMode, 6);
    CallDynGlobal(gAddr.setAddressMode, (uint64_t)0);
    DeviceCall(D3D_SET_RENDER_STATE, 14u, 1u);     // D3DRS_ZWRITEENABLE
}

void DrawQuad(int texId, float left, float top, float right, float bottom, float depth, uint32_t color,
    bool depthTest, float u1, float v1)
{
    struct Vertex {
        float x, y, z, rhw;
        uint32_t color;
        float u, v;
    };
    // PS2 colors (0x80 = 1.0) to D3DCOLOR
    auto channel = [&](int shift) { unsigned int c = (color >> shift) & 0xFF; return c >= 0x80 ? 0xFFu : c * 2; };
    uint32_t c = (channel(24) << 24) | (channel(0) << 16) | (channel(8) << 8) | channel(16);
    left -= 0.5f;
    top -= 0.5f;
    right -= 0.5f;
    bottom -= 0.5f;
    Vertex v[4] = {
        { left, top, depth, 1.0f, c, 0.0f, 0.0f },
        { right, top, depth, 1.0f, c, u1, 0.0f },
        { left, bottom, depth, 1.0f, c, 0.0f, v1 },
        { right, bottom, depth, 1.0f, c, u1, v1 },
    };
    CallDynGlobal(gAddr.setTexture, texId, texId);
    DWORD zEnable = 0;
    if (!depthTest) {
        DeviceCall(D3D_GET_RENDER_STATE, (unsigned int)D3DRS_ZENABLE, &zEnable);
        DeviceCall(D3D_SET_RENDER_STATE, (unsigned int)D3DRS_ZENABLE, 0u);
    }
    DeviceCall(D3D_DRAW_PRIMITIVE_UP, 5u, 2u, (void *)v, 28u);      // D3DPT_TRIANGLESTRIP
    if (!depthTest)
        DeviceCall(D3D_SET_RENDER_STATE, (unsigned int)D3DRS_ZENABLE, (unsigned int)zEnable);
}

void DrawDepth(int texId, float left, float top, float right, float bottom, float depth) {
    // D3DRS_ZENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ZFUNC (D3DCMP_ALWAYS), D3DRS_COLORWRITEENABLE
    unsigned int const states[4] = { 7u, 15u, 23u, 168u };
    unsigned int const values[4] = { 1u, 0u, 8u, 0u };
    DWORD saved[4] = {};
    for (int i = 0; i < 4; i++) {
        DeviceCall(D3D_GET_RENDER_STATE, states[i], &saved[i]);
        DeviceCall(D3D_SET_RENDER_STATE, states[i], values[i]);
    }
    DrawQuad(texId, left, top, right, bottom, depth, 0x00808080, true);
    for (int i = 3; i >= 0; i--)
        DeviceCall(D3D_SET_RENDER_STATE, states[i], (unsigned int)saved[i]);
}

void EndQuads() {
    CallDynGlobal(gAddr.setAddressMode, (uint64_t)5);
}

}
