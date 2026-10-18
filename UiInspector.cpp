#include "Ui.hpp"
#include "PesImGui.h"
#include <set>
#include <vector>

// In-game window (via PesImGui.dll) that lists the items of the 2D render list (menus), to find the screens and the
// panels for the player photos. Enabled with MAIN/UI_INSPECTOR = 1.
//
// Layout objects show one asset of a layout file (.opd): they are listed by file and asset. Other items (texts and
// the like) by the address of their draw callback. The window shows the items of the last frame; a row can hide its
// items (not drawn), and the hovered row blinks. Copy puts the list and the loaded layout files on the clipboard.

namespace UiInspector {

struct Addresses {
    uintptr_t opdList;          // first entry of the loaded OPD files (+4 next, +8 file id)
    uintptr_t editPlayerList;   // Edit mode (kitserver: ISEDITPLAYERLIST, ISEDITPLAYERMODE, EDITEDPLAYER)
    uintptr_t editPlayerMode;
    uintptr_t editedPlayer;
};
constinit static Addresses gAddr = {};

struct Item {
    uintptr_t object;
    uintptr_t callback;
    uint32_t opd;       // layout objects: the file (0: other items)
    int asset;
    int layer;
    float key;
};

static vector<Item> &CurrentFrame() {
    static vector<Item> items;
    return items;
}

static vector<Item> &LastFrame() {
    static vector<Item> items;
    return items;
}

// hidden: layout objects by OPD file id << 16 | asset, others by callback | 1 << 63
static set<uint64_t> &Hidden() {
    static set<uint64_t> hidden;
    return hidden;
}

constinit static uint64_t gHovered = ~0ull;
constinit static unsigned int gFrame = 0;          // blinking
constinit static unsigned int gGameFrame = 0;
constinit static bool gPaused = false;
constinit static bool gOthers = true;

static uint64_t ItemKey(Item const &i) {
    if (i.opd)
        return ((uint64_t)i.opd << 16) | (uint16_t)i.asset;
    return (1ull << 63) | i.callback;
}

static bool OnBeforeSubmit(Ui::RenderNode *node, int layer, int size, float key) {
    // a new frame: the last one is complete
    if (Ui::Frame() != gGameFrame) {
        gGameFrame = Ui::Frame();
        if (!gPaused)
            LastFrame().swap(CurrentFrame());
        CurrentFrame().clear();
    }
    Item i = {};
    i.object = (uintptr_t)node->object;
    i.callback = (uintptr_t)node->callback;
    i.layer = layer;
    i.key = key;
    if (Ui::IsLayoutObject(node)) {
        i.opd = Ui::LayoutFile((uint8_t const *)node->object);
        i.asset = Ui::LayoutAsset((uint8_t const *)node->object);
    }
    CurrentFrame().push_back(i);
    uint64_t k = ItemKey(i);
    return !Hidden().count(k) && !(k == gHovered && (gFrame / 8) % 2);
}

static void OnCreate() {
    IMGUI_SYNC
    ImGuiCheckVersion();
}

static void OnDestroy() {
    IMGUI_SYNC
}

static string FileIdText(uint32_t id) {
    // archives: 0 0_sound, 1 0_text, 2 <language>_sound, 3 <language>_text
    static char const *const archives[] = { "0_sound", "0_text", "x_sound", "x_text" };
    unsigned int archive = (id >> 16) & 7;
    char buf[64];
    sprintf_s(buf, "%s #%u", archive < 4 ? archives[archive] : "?", id & 0xFFFF);
    return buf;
}

static string ItemText(Item const &i) {
    char buf[128];
    if (i.opd)
        sprintf_s(buf, "%s\tasset %d\tlayer %d\tkey %.3f\tobject %08X", FileIdText(i.opd).c_str(), i.asset, i.layer,
            i.key, (unsigned int)i.object);
    else
        sprintf_s(buf, "draw %08X\t\tlayer %d\tkey %.3f\tobject %08X", (unsigned int)i.callback, i.layer, i.key,
            (unsigned int)i.object);
    return buf;
}

static void CopyToClipboard() {
    string text = "Loaded OPD files:\n";
    for (auto e = *(uint8_t **)gAddr.opdList; e; e = *(uint8_t **)(e + 4))
        text += "  " + FileIdText(*(uint32_t *)(e + 8)) + "\n";
    text += "Items (drawing order: bigger keys first):\n";
    for (Item const &i : LastFrame())
        text += "  " + ItemText(i) + "\n";
    ImGui::SetClipboardText(text.c_str());
}

static void OnRender() {
    IMGUI_SYNC
    gFrame++;
    gHovered = ~0ull;

    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PesAssetLoader - UI inspector")) {
        ImGui::End();
        return;
    }
    bool list = *(uint8_t *)gAddr.editPlayerList != 0, mode = *(uint8_t *)gAddr.editPlayerMode != 0;
    ImGui::Text("Edit player: list %d, mode %d, player id %u", list, mode, *(uint16_t *)gAddr.editedPlayer);
    ImGui::Checkbox("Pause", &gPaused);
    ImGui::SameLine();
    ImGui::Checkbox("Other items", &gOthers);
    ImGui::SameLine();
    if (ImGui::Button("Show all"))
        Hidden().clear();
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
        CopyToClipboard();
    ImGui::TextDisabled("Hover a row: its items blink. Other items: by their draw function.");

    if (ImGui::CollapsingHeader("Loaded OPD files")) {
        for (auto e = *(uint8_t **)gAddr.opdList; e; e = *(uint8_t **)(e + 4)) {
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::TextUnformatted(FileIdText(*(uint32_t *)(e + 8)).c_str());
        }
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("items", 6, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Hide", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("OPD file / draw");
        ImGui::TableSetupColumn("Asset");
        ImGui::TableSetupColumn("Layer");
        ImGui::TableSetupColumn("Key");
        ImGui::TableSetupColumn("Object");
        ImGui::TableHeadersRow();
        // hidden items aren't drawn, but they stay in the list
        int row = 0;
        for (Item const &i : LastFrame()) {
            if (!i.opd && !gOthers)
                continue;
            uint64_t k = ItemKey(i);
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool hidden = Hidden().count(k) != 0;
            if (ImGui::Checkbox("##hide", &hidden)) {
                if (hidden)
                    Hidden().insert(k);
                else
                    Hidden().erase(k);
            }
            ImGui::TableNextColumn();
            // the whole row: hovered -> its items blink
            ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            if (ImGui::IsItemHovered())
                gHovered = k;
            ImGui::SameLine();
            if (i.opd)
                ImGui::TextUnformatted(FileIdText(i.opd).c_str());
            else
                ImGui::TextDisabled("draw %08X", (unsigned int)i.callback);
            ImGui::TableNextColumn();
            if (i.opd)
                ImGui::Text("%d", i.asset);
            ImGui::TableNextColumn();
            ImGui::Text("%d", i.layer);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", i.key);
            ImGui::TableNextColumn();
            ImGui::Text("%08X", (unsigned int)i.object);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Install(PES::Version v) {
    if (!config().UI_INSPECTOR)
        return;
    switch (v.id()) {
    case ID_PES5:
        gAddr = { 0xFE5AD4, 0x38F9AE0, 0x38F9AE4, 0x3937218 };
        break;
    case ID_PES6:
        gAddr = { 0x3A67CA0, 0x1108534, 0x11084E8, 0x112E24A };
        break;
    default:
        return;
    }
    if (GetFileAttributesW(PES::GameDirPath(L"plugins\\PesImGui.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG(LOG_LEVEL_1, "[UiInspector] PesImGui.dll is not installed, the UI inspector is disabled");
        return;
    }
    if (!Ui::Install(v) || !ImGuiEnsureLoaded(false))
        return;
    Ui::AddBeforeSubmit(OnBeforeSubmit);
    ImGuiOnCreate(OnCreate);
    ImGuiOnRender(OnRender);
    ImGuiOnDestroy(OnDestroy);
    ImGuiEnableMouseCursor();
    LOG(LOG_LEVEL_1, "[UiInspector] on");
}

}

void InitUiInspector(PES::Version v) {
    UiInspector::Install(v);
}
