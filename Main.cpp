#include "Main.hpp"

Config &config() {
    static Config cfg;
    return cfg;
}

MemoryStats &memoryStats() {
    static MemoryStats stats;
    return stats;
}

// Copy the function prologue (which must not contain relative branches) into executable memory, followed by
// a jump to the rest of the function. Returns 0 if the prologue doesn't match the expected bytes.
uintptr_t MakeTrampoline(uintptr_t addr, char const *prologue) {
    size_t len = (strlen(prologue) + 1) / 3;
    if (patch::GetBytes(addr, len) != prologue)
        return 0;
    auto tramp = (uint8_t *)VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return 0;
    memcpy(tramp, (void *)addr, len);
    tramp[len] = 0xE9;
    *(int32_t *)(tramp + len + 1) = (int32_t)(addr + len - ((uintptr_t)tramp + len + 5));
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);
    return (uintptr_t)tramp;
}

// The same for a prologue that ends with a relative call (E8 rel32 right after the expected bytes): the call is made
// again from the trampoline with its offset recalculated.
uintptr_t MakeTrampolineWithCall(uintptr_t addr, char const *prologue) {
    size_t len = (strlen(prologue) + 1) / 3;
    if (patch::GetBytes(addr, len) != prologue || patch::GetBytes(addr + len, 1) != "E8")
        return 0;
    uintptr_t target = injector::GetBranchDestination(addr + len);
    auto tramp = (uint8_t *)VirtualAlloc(nullptr, len + 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return 0;
    memcpy(tramp, (void *)addr, len);
    tramp[len] = 0xE8;
    *(int32_t *)(tramp + len + 1) = (int32_t)(target - ((uintptr_t)tramp + len + 5));
    tramp[len + 5] = 0xE9;
    *(int32_t *)(tramp + len + 6) = (int32_t)(addr + len + 5 - ((uintptr_t)tramp + len + 10));
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 10);
    return (uintptr_t)tramp;
}

class PesAssetLoader {
public:
    PesAssetLoader() {
        auto v = PES::GetAppVersion();
        if (v.id() == ID_PES5 || v.id() == ID_PES6) {
            config().Read(PES::GameDirPath(L"plugins\\PesAssetLoader.ini"));
            Log::Init(PES::GameDirPath(L"plugins\\PesAssetLoader.log"), config().LOG_LEVEL > 0, true);
            InitFileLoader(v);
            InitMemoryLimits(v);
            InitTextureLimits(v);
            InitFlagReplacer(v);
            InitTextureReplacement(v);
            InitTextLoader(v);
            InitMemoryMonitor(v);
            InitPlayerPhotos(v);
            InitUiInspector(v);
        }
    }
} pesAssetLoader;
