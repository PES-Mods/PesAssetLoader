#include "Main.hpp"
#include "PesImGui.h"

// In-game window (via PesImGui.dll, if it's installed) that shows how the game memory arenas are used.
// Enabled with MAIN/MEMORY_MONITOR = 1.
//
// Arenas are dlmalloc-like heaps. Each one gets its whole region on the first allocation and then hands out
// memory from the "top" chunk; everything below the top chunk has been used at some point (it can be in use
// or freed and waiting for reuse). We show this "footprint" and its peak, and count failed allocations.

extern size_t gArenaMemorySize[24];
extern size_t ArenaTotal;

namespace MemoryMonitor {

const int kNumArenas = 24;

struct ArenaHeader {
    uintptr_t begin;
    uintptr_t end;
    // ... dlmalloc state starts at +0x18, its top chunk pointer is at +0x48
};
const size_t kTopChunkOffset = 0x48;

struct ArenaStats {
    size_t peak = 0;            // max footprint
    unsigned int failures = 0;  // failed allocations
    size_t lastFailedSize = 0;
    size_t largestFailedSize = 0;
};

static uintptr_t gArenaTable = 0;        // ArenaHeader *gArena[24]
static uintptr_t oArenaAllocCore = 0;
constinit static ArenaStats gStats[kNumArenas] = {};

static ArenaHeader *GetArena(int id) {
    return ((ArenaHeader **)gArenaTable)[id];
}

static size_t Capacity(ArenaHeader *a) {
    return a->end > a->begin ? a->end - a->begin : 0;
}

static size_t Footprint(ArenaHeader *a) {
    uintptr_t top = *(uintptr_t *)((uintptr_t)a + kTopChunkOffset);
    if (top < a->begin || top > a->end)
        return 0; // nothing allocated yet
    return top - a->begin;
}

static void UpdatePeak(int id) {
    if (ArenaHeader *a = GetArena(id)) {
        size_t footprint = Footprint(a);
        if (footprint > gStats[id].peak)
            gStats[id].peak = footprint;
    }
}

static void *OnArenaAllocCore(int id, size_t size) {
    void *result = CallAndReturnDynGlobal<void *>(oArenaAllocCore, id, size);
    if (id >= 0 && id < kNumArenas) {
        if (result)
            UpdatePeak(id);
        else {
            auto &s = gStats[id];
            s.failures++;
            s.lastFailedSize = size;
            if (size > s.largestFailedSize)
                s.largestFailedSize = size;
            ArenaHeader *a = GetArena(id);
            LOG(LOG_LEVEL_1, "[MemoryMonitor] arena %d: failed to allocate %u bytes (size %u, used %u)",
                id, size, a ? Capacity(a) : 0, a ? Footprint(a) : 0);
        }
    }
    return result;
}

// --- UI -------------------------------------------------------------------------------------------

static string FormatSize(size_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024)
        sprintf_s(buf, "%.2f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024)
        sprintf_s(buf, "%.1f KB", bytes / 1024.0);
    else
        sprintf_s(buf, "%u B", (unsigned int)bytes);
    return buf;
}

static void OnCreate() {
    IMGUI_SYNC
    ImGuiCheckVersion();
}

static void OnDestroy() {
    IMGUI_SYNC
}

static void OnRender() {
    IMGUI_SYNC
    for (int id = 0; id < kNumArenas; id++)
        UpdatePeak(id);

    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PesAssetLoader - Memory")) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Reset peaks")) {
        for (auto &s : gStats)
            s = ArenaStats();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(used = memory touched by allocations, incl. freed blocks waiting for reuse)");

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("arenas", 6, flags)) {
        ImGui::TableSetupColumn("Arena", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Used");
        ImGui::TableSetupColumn("Peak");
        ImGui::TableSetupColumn("Peak %", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Failed");
        ImGui::TableHeadersRow();
        for (int id = 0; id < kNumArenas; id++) {
            ArenaHeader *a = GetArena(id);
            auto const &s = gStats[id];
            if (!a && !s.peak && !s.failures)
                continue;
            size_t capacity = a ? Capacity(a) : 0;
            size_t footprint = a ? Footprint(a) : 0;
            float peakRatio = capacity ? (float)s.peak / (float)capacity : 0.0f;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (id == 16)
                ImGui::Text("16 (8-10)");
            else
                ImGui::Text("%d", id);
            ImGui::TableNextColumn();
            if (a)
                ImGui::TextUnformatted(FormatSize(capacity).c_str());
            else
                ImGui::TextDisabled("inactive");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a ? FormatSize(footprint).c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(s.peak).c_str());
            ImGui::TableNextColumn();
            ImVec4 color = peakRatio >= 0.9f ? ImVec4(0.85f, 0.25f, 0.25f, 1.0f)
                : peakRatio >= 0.7f ? ImVec4(0.85f, 0.65f, 0.2f, 1.0f) : ImVec4(0.3f, 0.65f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            char label[16];
            sprintf_s(label, "%.0f%%", peakRatio * 100.0f);
            ImGui::ProgressBar(peakRatio > 1.0f ? 1.0f : peakRatio, ImVec2(-FLT_MIN, 0), capacity ? label : "");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            if (s.failures) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%u", s.failures);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Failed allocations: %u\nLast: %s\nLargest: %s", s.failures,
                        FormatSize(s.lastFailedSize).c_str(), FormatSize(s.largestFailedSize).c_str());
            }
            else
                ImGui::TextDisabled("0");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("A failed allocation can be retried by the game in another arena.");

    ImGui::Separator();
    ImGui::Text("Plugin memory");
    auto const &m = memoryStats();
    if (ArenaTotal)
        ImGui::BulletText("Main arena pool: %s (RAM is used only for touched pages)", FormatSize(ArenaTotal).c_str());
    ImGui::BulletText("Enlarged model slots: %u buffers, %s", m.modelBuffers, FormatSize(m.modelBuffersSize).c_str());
    ImGui::BulletText("Replaced textures: %u, %s", m.textureCopies, FormatSize(m.textureCopiesSize).c_str());
    ImGui::End();
}

void Install(PES::Version v) {
    if (!config().MEMORY_MONITOR)
        return;
    uintptr_t allocCore;
    char const *prologue;
    switch (v.id()) {
    case ID_PES5:
        gArenaTable = 0x38B65D8;
        allocCore = 0x8DA600;
        prologue = "8B 44 24 08 83 EC 08";
        break;
    case ID_PES6:
        gArenaTable = 0x12458F8;
        allocCore = 0x884130;
        prologue = "8B 44 24 04 83 EC 08";
        break;
    default:
        return;
    }
    // only if PesImGui.dll is installed
    if (GetFileAttributesW(PES::GameDirPath(L"plugins\\PesImGui.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG(LOG_LEVEL_1, "[MemoryMonitor] PesImGui.dll is not installed, the memory monitor is disabled");
        return;
    }
    if (!ImGuiEnsureLoaded(false))
        return;
    oArenaAllocCore = MakeTrampoline(allocCore, prologue);
    if (oArenaAllocCore)
        patch::RedirectJump(allocCore, OnArenaAllocCore);
    else
        LOG(LOG_LEVEL_1, "[MemoryMonitor] unexpected code at %X, peaks between frames and failed allocations are not tracked", allocCore);
    ImGuiOnCreate(OnCreate);
    ImGuiOnRender(OnRender);
    ImGuiOnDestroy(OnDestroy);
    ImGuiEnableMouseCursor();
}

}

void InitMemoryMonitor(PES::Version v) {
    MemoryMonitor::Install(v);
}
