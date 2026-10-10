#include "Main.hpp"
#include <intrin.h>

uintptr_t gInitArenas = 0;
uintptr_t gInitArena15 = 0;
size_t gArenaMemorySize[24] = {};
struct ArenaStartEnd { size_t start, end; };
ArenaStartEnd gArenaStartEnd[24] = {};
size_t ArenaTotal = 0;

// Game-specific arena setup. Code addresses are the instructions to patch (0 - not used in this game).
// Note: all globals used while the plugin initializes must be constant-initialized (no constructors),
// because the initialization order of globals in different source files is not defined.
struct ArenaSetup {
    size_t defaultSize[24];
    int layoutOrder[24];           // arenas placed in the main memory pool, in order
    int layoutSize;
    uintptr_t initArenas;          // InitArenas(ArenaStartEnd[24])
    uintptr_t initArena;           // InitArena(id, start, end)
    uintptr_t poolAllocCall;       // main pool: call _aligned_malloc
    uintptr_t poolMemset;          // main pool: rep stosd
    uintptr_t poolSizeAdd;         // main pool: add reg, size
    uintptr_t initArenasCall;      // call InitArenas
    uintptr_t arena15InitCall;     // call InitArena(15, ...)
    uintptr_t arena10SizePush;     // push size (arena 10 alone)
    uintptr_t arena10And12SizePush;// push size (arena 10 takes the memory of arena 12)
    uintptr_t arena23AllocCall;    // own block for arena 23: call _aligned_malloc
    uintptr_t arena23Memset;       // own block for arena 23: rep stosd
    uintptr_t arena23SizePush;     // own block for arena 23: push size
    uintptr_t arena23FreeCall;     // own block for arena 23: call _aligned_free
};

static constexpr ArenaSetup kArenaSetupPES5 = {
    {
        /*0*/ 0,          /*1*/ 10'485'760, /*2*/ 3'407'872, /*3*/ 86'016,
        /*4*/ 266'240,    /*5*/ 2'940'928,  /*6*/ 2'101'248, /*7*/ 1'990'656,
        /*8*/ 102'400,    /*9*/ 1'417'216,  /*10*/ 1'548'288,/*11*/ 1'048'576,
        /*12*/ 946'176,   /*13*/ 4'587'520, /*14*/ 0,        /*15*/ 255'600,
        /*16*/ 0,         /*17*/ 0,         /*18*/ 0,        /*19*/ 0,
        /*20*/ 0,         /*21*/ 260'096,   /*22*/ 0,        /*23*/ 3'670'016,
    },
    { 5, 4, 6, 8, 9, 10, 12, 11, 0, 21, 2, 3, 13, 7, 1 }, 15,
    0x831210, 0x831100,
    0x831706, 0x831716, 0x831727,
    0x45C05D,
    0x45C162,
    0x45C30A, 0x45C2DA,
    0x45C1C0, 0x45C1DE, 0x45C1C7, 0x45C250,
};

// PES6: arena 23 is a part of the main pool, arena 21 is not used, arenas 17-20 are new
static constexpr ArenaSetup kArenaSetupPES6 = {
    {
        /*0*/ 0,          /*1*/ 10'812'416, /*2*/ 3'244'032, /*3*/ 86'016,
        /*4*/ 233'472,    /*5*/ 3'268'608,  /*6*/ 2'076'672, /*7*/ 1'978'368,
        /*8*/ 102'400,    /*9*/ 1'392'640,  /*10*/ 1'417'216,/*11*/ 1'048'576,
        /*12*/ 1'101'824, /*13*/ 5'046'272, /*14*/ 0,        /*15*/ 255'600,
        /*16*/ 0,         /*17*/ 524'288,   /*18*/ 1'441'792,/*19*/ 1'048'576,
        /*20*/ 587'776,   /*21*/ 0,         /*22*/ 0,        /*23*/ 3'538'944,
    },
    { 5, 4, 6, 8, 9, 10, 12, 11, 0, 20, 2, 3, 13, 17, 23, 7, 19, 18, 1 }, 19,
    0x8C76E0, 0x45BB60,
    0x8C7B76, 0x8C7B86, 0x8C7B97,
    0xB14688,
    0xB14772,
    0xB1491A, 0xB148EA,
    0, 0, 0, 0,
};

constinit static ArenaSetup const *gArenaSetup = nullptr;

namespace Parser {

struct ArenaOverride {
    enum class Type { Default, Multiplier, Bytes } type = Type::Default;
    double value = 0.0;
};

static std::string Trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

static bool SplitNumberSuffix(const std::string &raw, double &outNumber, std::string &outSuffix) {
    std::string s = Trim(raw);
    if (s.empty())
        return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-')
        i++;
    bool sawDigit = false, sawDot = false;
    size_t numEnd = i;
    while (numEnd < s.size() && (isdigit((unsigned char)s[numEnd]) || (s[numEnd] == '.' && !sawDot))) {
        if (s[numEnd] == '.')
            sawDot = true;
        else
            sawDigit = true;
        numEnd++;
    }
    if (!sawDigit)
        return false;
    try {
        outNumber = std::stod(s.substr(0, numEnd));
    }
    catch (...) {
        return false;
    }
    outSuffix = ToLower(Trim(s.substr(numEnd)));
    return true;
}

static bool ParseGlobalMultiplier(const std::string &raw, double &outMultiplier) {
    std::string s = Trim(raw);
    if (s.empty())
        return false;
    double num;
    std::string suffix;
    if (!SplitNumberSuffix(s, num, suffix))
        return false;
    if (!suffix.empty() && suffix != "x")
        return false;
    if (num <= 0.0)
        return false;
    outMultiplier = num;
    return true;
}

static bool ParseArenaValue(const std::string &raw, ArenaOverride &out) {
    std::string s = Trim(raw);
    if (s.empty()) {
        out.type = ArenaOverride::Type::Default;
        return true;
    }
    double num;
    std::string suffix;
    if (!SplitNumberSuffix(s, num, suffix))
        return false;
    if (suffix == "x") {
        if (num <= 0.0)
            return false;
        out.type = ArenaOverride::Type::Multiplier;
        out.value = num;
        return true;
    }
    double unit;
    if (suffix.empty() || suffix == "b" || suffix == "byte" || suffix == "bytes")
        unit = 1.0;
    else if (suffix == "kb" || suffix == "k")
        unit = 1024.0;
    else if (suffix == "mb" || suffix == "m")
        unit = 1024.0 * 1024.0;
    else if (suffix == "gb" || suffix == "g")
        unit = 1024.0 * 1024.0 * 1024.0;
    else
        return false;
    if (num < 0.0)
        return false;
    out.type = ArenaOverride::Type::Bytes;
    out.value = num * unit;
    return true;
}

static size_t ComputeFinalSize(size_t baseSize, const std::string &configValue, double globalMultiplier, const char *arenaName) {
    if (baseSize == 0)
        return 0;
    ArenaOverride ov;
    if (!ParseArenaValue(configValue, ov)) {
        if (!configValue.empty())
            LOG(LOG_LEVEL_1, "[MemoryLimits] Invalid value for %s: \"%s\", falling back to ARENA_MP", arenaName, configValue.c_str());
        ov.type = ArenaOverride::Type::Default;
    }
    size_t result;
    switch (ov.type) {
    case ArenaOverride::Type::Bytes:
        result = (size_t)std::llround(ov.value);
        break;
    case ArenaOverride::Type::Multiplier:
        result = (size_t)std::llround(baseSize * ov.value);
        break;
    default:
        result = (size_t)std::llround(baseSize * globalMultiplier);
        break;
    }
    const size_t kMinArenaSize = 4096;
    if (result < kMinArenaSize) {
        LOG(LOG_LEVEL_1, "[MemoryLimits] %s resolved to %zu bytes, clamping to minimum %zu", arenaName, result, kMinArenaSize);
        result = kMinArenaSize;
    }
    return result;
}

}

// Arena memory comes directly from VirtualAlloc: the pages are zero-filled by the system and get
// physical memory only when they are used for the first time, so big unused arenas cost no RAM.
// The blocks are 64 KB aligned, which covers the alignment the game asks for (256 / 4096).
static void *AllocArenaPages(size_t size, char const *name) {
    void *mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem)
        ::Error("Unable to allocate %u bytes for %s (error %u)", size, name, GetLastError());
    return mem;
}

void *AllocArenaTotal(size_t size, size_t alignment) {
    void *mem = AllocArenaPages(ArenaTotal, "the main memory pool");
    uintptr_t base = (uintptr_t)mem;
    size_t offset = 0;
    for (int i = 0; i < gArenaSetup->layoutSize; i++) {
        int id = gArenaSetup->layoutOrder[i];
        gArenaStartEnd[id].start = base + offset;
        offset += gArenaMemorySize[id];
        gArenaStartEnd[id].end = base + offset;
    }
    gArenaStartEnd[16].start = gArenaStartEnd[8].start;
    gArenaStartEnd[16].end = gArenaStartEnd[8].start + gArenaMemorySize[8] + gArenaMemorySize[9] + gArenaMemorySize[10];
    return mem;
}

// Arena 23 (PES5) exists only while some scenes are active (e.g. memory card): allocated on enter, freed on exit
void *AllocArena23(size_t size, size_t alignment) {
    return AllocArenaPages(gArenaMemorySize[23], "arena 23");
}

void FreeArena23(void *mem) {
    if (mem)
        VirtualFree(mem, 0, MEM_RELEASE);
}

void InitArenas(ArenaStartEnd *) {
    CallDynGlobal(gInitArenas, gArenaStartEnd);
}

size_t AlignTo4096(size_t num) {
    return (num + 4095) & ~static_cast<uintptr_t>(4095);
}

// Arena 15 is re-initialized at several scene transitions; the game uses a static buffer for it,
// so we allocate our block once and reuse it every time.
void *InitArena15(int, void *, void *) {
    static uint8_t *mem = nullptr;
    size_t size = AlignTo4096(gArenaMemorySize[15]);
    if (!mem)
        mem = (uint8_t *)AllocArenaPages(size, "arena 15");
    return CallAndReturnDynGlobal<void *>(gInitArena15, 15, mem, mem + size);
}

// Player models (bodies, faces, hairs) are stored in fixed-size slots of pre-allocated pools:
// pool + slotIndex * slotSize. A model bigger than the slot overwrites its neighbours.
// We replace the slot address calculation, so the slot can be backed by our own (bigger) buffer.
namespace ModelSlots {

struct Pool {
    const char *name;
    uintptr_t poolPtr;   // address of the game variable that holds the pool pointer
    size_t slotSize;
    size_t poolSize;
    unsigned int numSlots;
    struct Slot { uint8_t *buf = nullptr; size_t capacity = 0; } slots[23];
};

enum PoolId { POOL_BODY, POOL_FACE, POOL_HAIR, POOL_COUNT };

constinit static Pool gPools[POOL_COUNT] = {
    { "body", 0, 0, 0, 0 },
    { "face", 0, 0, 0, 0 },
    { "hair", 0, 0, 0, 0 },
};

// Patched site: call address -> pool. Return address of the call is site + 5.
struct Site { uintptr_t addr; PoolId pool; };
constinit static Site gSites[16] = {};
constinit static unsigned int gNumSites = 0;

static uint8_t *AcquireSlot(uintptr_t retAddr, unsigned int slotIndex, size_t size) {
    Pool *pool = nullptr;
    for (unsigned int i = 0; i < gNumSites; i++) {
        auto const &s = gSites[i];
        if (s.addr + 5 == retAddr) {
            pool = &gPools[s.pool];
            break;
        }
    }
    if (!pool || slotIndex >= pool->numSlots) {
        ::Error("ModelSlots: unknown call site %X (slot %u)", retAddr - 5, slotIndex);
        return nullptr;
    }
    auto &slot = pool->slots[slotIndex];
    if (slot.buf && size <= slot.capacity)
        return slot.buf;
    uint8_t *poolMem = *(uint8_t **)pool->poolPtr;
    size_t defaultCapacity = min(pool->slotSize, pool->poolSize - slotIndex * pool->slotSize);
    if (!slot.buf && size <= defaultCapacity)
        return poolMem + slotIndex * pool->slotSize;
    // Old buffer is intentionally not freed: the game may still keep a texture registered at the old
    // address (e.g. hair without texture keeps the previous one). Slots only grow, so this is bounded.
    size_t capacity = AlignTo4096(size + size / 4);
    uint8_t *buf = (uint8_t *)_aligned_malloc(capacity, 64);
    if (!buf) {
        ::Error("ModelSlots: unable to allocate %u bytes for %s slot %u", capacity, pool->name, slotIndex);
        return nullptr;
    }
    memset(buf, 0, capacity);
    slot.buf = buf;
    slot.capacity = capacity;
    memoryStats().modelBuffersSize += capacity;
    memoryStats().modelBuffers++;
    LOG(LOG_LEVEL_1, "[MemoryLimits] %s slot %u: %u bytes (default %u), allocated own buffer of %u bytes",
        pool->name, slotIndex, size, defaultCapacity, capacity);
    return buf;
}

// Called in place of "slot = pool + index * slotSize". Input: slot index and size in registers
// (differs per site), output: eax = slot memory. All other registers are preserved.
__declspec(naked) void Stub_CL_EAX() {
    __asm {
        push ecx
        push edx
        push eax                 // size
        movzx ecx, cl
        push ecx                 // slot index
        push dword ptr [esp + 16] // return address
        call AcquireSlot
        add esp, 12
        pop edx
        pop ecx
        ret
    }
}

__declspec(naked) void Stub_AL_ESI() {
    __asm {
        push ecx
        push edx
        push esi                 // size
        movzx eax, al
        push eax                 // slot index
        push dword ptr [esp + 16] // return address
        call AcquireSlot
        add esp, 12
        pop edx
        pop ecx
        ret
    }
}

__declspec(naked) void Stub_AL_ECX() {
    __asm {
        push ecx
        push edx
        push ecx                 // size
        movzx eax, al
        push eax                 // slot index
        push dword ptr [esp + 16] // return address
        call AcquireSlot
        add esp, 12
        pop edx
        pop ecx
        ret
    }
}

__declspec(naked) void Stub_AL_EDX() {
    __asm {
        push ecx
        push edx
        push edx                 // size
        movzx eax, al
        push eax                 // slot index
        push dword ptr [esp + 16] // return address
        call AcquireSlot
        add esp, 12
        pop edx
        pop ecx
        ret
    }
}

// Each site is the 17-byte slot address calculation; it becomes "call stub; jmp +10; nop..."
static bool PatchSite(uintptr_t addr, char const *expectedBytes, PoolId pool, void (*stub)()) {
    const size_t kSiteSize = 17;
    if (gNumSites >= std::size(gSites))
        return false;
    if (patch::GetBytes(addr, kSiteSize) != expectedBytes) {
        LOG(LOG_LEVEL_1, "[MemoryLimits] unexpected code at %X, %s slots are not patched", addr, gPools[pool].name);
        return false;
    }
    patch::Nop(addr, kSiteSize);
    patch::RedirectCall(addr, stub);
    patch::SetBytes(addr + 5, "EB 0A"); // jmp addr + 17
    gSites[gNumSites++] = { addr, pool };
    return true;
}

static void Install(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        gPools[POOL_BODY] = { "body", 0x1004E90, 0x4FC00, 0xC6D00, 3 };
        gPools[POOL_FACE] = { "face", 0x1004E00, 0xF0C0, 0xF0C0 * 23, 23 };
        gPools[POOL_HAIR] = { "hair", 0x10039AC, 0x5340, 0x5340 * 23, 23 };
        // movzx eax,cl / mov ecx,[pool] / imul eax,eax,slotSize / add eax,ecx
        PatchSite(0x84ECE2, "0F B6 C1 8B 0D AC 39 00 01 69 C0 40 53 00 00 03 C1", POOL_HAIR, Stub_CL_EAX); // loadPlayerHairCB (with texture)
        PatchSite(0x84ED2C, "0F B6 C1 8B 0D AC 39 00 01 69 C0 40 53 00 00 03 C1", POOL_HAIR, Stub_CL_EAX); // loadPlayerHairCB (model only)
        PatchSite(0x84FDAF, "0F B6 C1 8B 0D 00 4E 00 01 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_CL_EAX); // loadPlayerFaceCB (lod0)
        PatchSite(0x84FE4C, "0F B6 C1 8B 0D 00 4E 00 01 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_CL_EAX); // loadPlayerFaceCB (lod0 + lod1)
        // mov reg,[pool] / movzx eax,al / imul eax,eax,slotSize / add eax,reg
        PatchSite(0x84F855, "8B 0D 00 4E 00 01 0F B6 C0 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_AL_ESI); // CreateBuiltInPlayerFaceTextures
        PatchSite(0x850697, "8B 0D 90 4E 00 01 0F B6 C0 69 C0 00 FC 04 00 03 C1", POOL_BODY, Stub_AL_ECX); // LoadBodyModelCB
        PatchSite(0x850949, "8B 15 90 4E 00 01 0F B6 C0 69 C0 00 FC 04 00 03 C2", POOL_BODY, Stub_AL_EDX); // LoadBodyModel (pre-packed)
        break;
    case ID_PES6:
        gPools[POOL_BODY] = { "body", 0x3A5BE78, 0x50080, 0xC8600, 3 };
        gPools[POOL_FACE] = { "face", 0x3A5BB70, 0xF0C0, 0xF0C0 * 23, 23 };
        gPools[POOL_HAIR] = { "hair", 0x3A5BDF4, 0x69C0, 0x69C0 * 23, 23 };
        // movzx eax,cl / mov ecx,[pool] / imul eax,eax,slotSize / add eax,ecx
        PatchSite(0x8B3D88, "0F B6 C1 8B 0D F4 BD A5 03 69 C0 C0 69 00 00 03 C1", POOL_HAIR, Stub_CL_EAX); // hair loader (with texture)
        PatchSite(0x8B3DCE, "0F B6 C1 8B 0D F4 BD A5 03 69 C0 C0 69 00 00 03 C1", POOL_HAIR, Stub_CL_EAX); // hair loader (model only)
        PatchSite(0x8B447F, "0F B6 C1 8B 0D F4 BD A5 03 69 C0 C0 69 00 00 03 C1", POOL_HAIR, Stub_CL_EAX); // hair copied from another slot (with texture)
        // mov reg,[pool] / movzx eax,al / imul eax,eax,slotSize / add eax,reg
        PatchSite(0x8B4519, "8B 0D F4 BD A5 03 0F B6 C0 69 C0 C0 69 00 00 03 C1", POOL_HAIR, Stub_AL_ECX); // hair copied from another slot (model only)
        PatchSite(0x8B2EAD, "8B 0D 70 BB A5 03 0F B6 C0 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_AL_ESI); // built-in face
        PatchSite(0x8B319D, "8B 0D 70 BB A5 03 0F B6 C0 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_AL_ESI); // face loader (lod0)
        PatchSite(0x8B323C, "8B 0D 70 BB A5 03 0F B6 C0 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_AL_ESI); // face loader (lod0 + lod1)
        PatchSite(0x8B3880, "8B 0D 70 BB A5 03 0F B6 C0 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_AL_ESI); // face copied from another slot (lod0)
        PatchSite(0x8B390D, "8B 0D 70 BB A5 03 0F B6 C0 69 C0 C0 F0 00 00 03 C1", POOL_FACE, Stub_AL_ESI); // face copied from another slot (lod0 + lod1)
        PatchSite(0x8B4727, "8B 0D 78 BE A5 03 0F B6 C0 69 C0 80 00 05 00 03 C1", POOL_BODY, Stub_AL_ECX); // body loader
        PatchSite(0x8B4A7D, "8B 15 78 BE A5 03 0F B6 C0 69 C0 80 00 05 00 03 C2", POOL_BODY, Stub_AL_EDX); // body loader (pre-packed)
        break;
    }
}

}

static bool CheckArenaCode(ArenaSetup const &s) {
    struct { uintptr_t addr; char const *bytes; } checks[] = {
        { s.poolAllocCall,        "E8" },
        { s.poolMemset,           "F3 AB" },
        { s.poolSizeAdd,          "81 C2" },
        { s.initArenasCall,       "E8" },
        { s.arena15InitCall,      "E8" },
        { s.arena10SizePush,      "68" },
        { s.arena10And12SizePush, "68" },
        { s.arena23AllocCall,     "E8" },
        { s.arena23Memset,        "F3 AB" },
        { s.arena23SizePush,      "68" },
        { s.arena23FreeCall,      "E8" },
    };
    for (auto const &c : checks) {
        if (c.addr && patch::GetBytes(c.addr, (strlen(c.bytes) + 1) / 3) != c.bytes) {
            LOG(LOG_LEVEL_1, "[MemoryLimits] unexpected code at %X, arena sizes are not changed", c.addr);
            return false;
        }
    }
    return true;
}

static void InstallArenas(ArenaSetup const &s) {
    if (!CheckArenaCode(s))
        return;
    double globalMultiplier = 1.0;
    if (!Parser::ParseGlobalMultiplier(config().ARENA_MP, globalMultiplier)) {
        if (!config().ARENA_MP.empty())
            LOG(LOG_LEVEL_1, "[MemoryLimits] Invalid ARENA_MP \"%s\", using 1x", config().ARENA_MP.c_str());
        globalMultiplier = 1.0;
    }

    string const *arenaConfig[24] = {
        nullptr,             &config().ARENA_1,  &config().ARENA_2,  &config().ARENA_3,
        &config().ARENA_4,   &config().ARENA_5,  &config().ARENA_6,  &config().ARENA_7,
        &config().ARENA_8,   &config().ARENA_9,  &config().ARENA_10, &config().ARENA_11,
        &config().ARENA_12,  &config().ARENA_13, nullptr,            &config().ARENA_15,
        nullptr,             &config().ARENA_17, &config().ARENA_18, &config().ARENA_19,
        &config().ARENA_20,  &config().ARENA_21, nullptr,            &config().ARENA_23,
    };
    for (int id = 0; id < 24; id++) {
        gArenaMemorySize[id] = 0;
        if (arenaConfig[id] && s.defaultSize[id]) {
            string name = "ARENA_" + to_string(id);
            gArenaMemorySize[id] = Parser::ComputeFinalSize(s.defaultSize[id], *arenaConfig[id], globalMultiplier, name.c_str());
        }
    }
    gArenaMemorySize[16] = gArenaMemorySize[8] + gArenaMemorySize[9] + gArenaMemorySize[10];

    gArenaSetup = &s;
    ArenaTotal = 0;
    for (int i = 0; i < s.layoutSize; i++)
        ArenaTotal += gArenaMemorySize[s.layoutOrder[i]];

    gInitArenas = s.initArenas;
    gInitArena15 = s.initArena;

    // Special split for arenas 10 and 12
    patch::SetUInt(s.arena10SizePush + 1, gArenaMemorySize[10]);
    patch::SetUInt(s.arena10And12SizePush + 1, gArenaMemorySize[10] + gArenaMemorySize[12]);
    // Arena 23 setup (when it has its own block)
    if (s.arena23AllocCall) {
        patch::Nop(s.arena23Memset, 2); // rep stosd
        patch::RedirectCall(s.arena23AllocCall, AllocArena23);
        patch::SetUInt(s.arena23SizePush + 1, gArenaMemorySize[23]);
        patch::RedirectCall(s.arena23FreeCall, FreeArena23);
    }
    // Arena 15 setup (own page-aligned block)
    patch::RedirectCall(s.arena15InitCall, InitArena15);
    // Main arena pool
    patch::RedirectCall(s.initArenasCall, InitArenas);
    patch::Nop(s.poolMemset, 2); // rep stosd
    patch::RedirectCall(s.poolAllocCall, AllocArenaTotal);
    patch::SetUInt(s.poolSizeAdd + 2, ArenaTotal);

    LOG(LOG_LEVEL_1, "[MemoryLimits] main memory pool: %u bytes", ArenaTotal);
}

void InitMemoryLimits(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
        InstallArenas(kArenaSetupPES5);
        break;
    case ID_PES6:
        InstallArenas(kArenaSetupPES6);
        break;
    default:
        return;
    }
    // Player models of any size
    ModelSlots::Install(v);
}
