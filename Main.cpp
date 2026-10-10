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
            InitTextureReplacement(v);
            InitMemoryMonitor(v);
        }
    }
} pesAssetLoader;
