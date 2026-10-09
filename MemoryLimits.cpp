#include "Main.hpp"
#include <intrin.h>

uintptr_t gAllocateAligned = 0;
uintptr_t gInitArenas = 0;
uintptr_t gInitArena15 = 0;
size_t gArenaMemorySize[24] = {};
struct ArenaStartEnd { size_t start, end; };
ArenaStartEnd gArenaStartEnd[24] = {};
static const int LayoutOrder[] = { 5, 4, 6, 8, 9, 10, 12, 11, 0, 21, 2, 3, 13, 7, 1 };
size_t ArenaTotal = 0;

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

void *AllocateAligned(size_t size, size_t alignment) {
    void *mem = CallAndReturnDynGlobal<void *>(gAllocateAligned, size, alignment); // aligned_malloc
    memset(mem, 0, size);
    return mem;
}

void *AllocArenaTotal(size_t size, size_t alignment) {
    void *mem = AllocateAligned(ArenaTotal, alignment);
    uintptr_t base = (uintptr_t)mem;
    size_t offset = 0;
    for (int id : LayoutOrder) {
        gArenaStartEnd[id].start = base + offset;
        offset += gArenaMemorySize[id];
        gArenaStartEnd[id].end = base + offset;
    }
    gArenaStartEnd[16].start = gArenaStartEnd[8].start;
    gArenaStartEnd[16].end = gArenaStartEnd[8].start + gArenaMemorySize[8] + gArenaMemorySize[9] + gArenaMemorySize[10];
    return mem;
}

void *AllocArena23(size_t size, size_t alignment) {
    return AllocateAligned(gArenaMemorySize[23], alignment);
}

void InitArenas(ArenaStartEnd *) {
    CallDynGlobal(gInitArenas, gArenaStartEnd);
}

size_t AlignTo4096(size_t num) {
    return (num + 4095) & ~static_cast<uintptr_t>(4095);
}

void *InitArena15(int, void *, void *) {
    size_t size = AlignTo4096(gArenaMemorySize[15]);
    uint8_t *mem = (uint8_t *)AllocateAligned(gArenaMemorySize[15], 4096);
    return CallAndReturnDynGlobal<void *>(gInitArena15, 15, mem, mem + size);
}

void MyShadowRender() {
    ::Message("%X", _ReturnAddress());
    return;
}

void DummyAddToRenderList(void *, int) {}

void InitMemoryLimits(PES::Version v) {
    switch (v.id()) {
    case ID_PES5:
    {
        static const size_t kDefaultSize[24] = {
            /*0*/ 0,          /*1*/ 10'485'760, /*2*/ 3'407'872, /*3*/ 86'016,
            /*4*/ 266'240,    /*5*/ 2'940'928,  /*6*/ 2'101'248, /*7*/ 1'990'656,
            /*8*/ 102'400,    /*9*/ 1'417'216,  /*10*/ 1'548'288,/*11*/ 1'048'576,
            /*12*/ 946'176,   /*13*/ 4'587'520, /*14*/ 0,        /*15*/ 255'600,
            /*16*/ 0,         /*17*/ 0,         /*18*/ 0,        /*19*/ 0,
            /*20*/ 0,         /*21*/ 260'096,   /*22*/ 0,        /*23*/ 3'670'016,
        };

        double globalMultiplier = 1.0;
        if (!Parser::ParseGlobalMultiplier(config().ARENA_MP, globalMultiplier)) {
            if (!config().ARENA_MP.empty())
                LOG(LOG_LEVEL_1, "[MemoryLimits] Invalid ARENA_MP \"%s\", using 1x", config().ARENA_MP.c_str());
            globalMultiplier = 1.0;
        }

        gArenaMemorySize[0] = 0;
        gArenaMemorySize[1] = Parser::ComputeFinalSize(kDefaultSize[1], config().ARENA_1, globalMultiplier, "ARENA_1");
        gArenaMemorySize[2] = Parser::ComputeFinalSize(kDefaultSize[2], config().ARENA_2, globalMultiplier, "ARENA_2");
        gArenaMemorySize[3] = Parser::ComputeFinalSize(kDefaultSize[3], config().ARENA_3, globalMultiplier, "ARENA_3");
        gArenaMemorySize[4] = Parser::ComputeFinalSize(kDefaultSize[4], config().ARENA_4, globalMultiplier, "ARENA_4");
        gArenaMemorySize[5] = Parser::ComputeFinalSize(kDefaultSize[5], config().ARENA_5, globalMultiplier, "ARENA_5");
        gArenaMemorySize[6] = Parser::ComputeFinalSize(kDefaultSize[6], config().ARENA_6, globalMultiplier, "ARENA_6");
        gArenaMemorySize[7] = Parser::ComputeFinalSize(kDefaultSize[7], config().ARENA_7, globalMultiplier, "ARENA_7");
        gArenaMemorySize[8] = Parser::ComputeFinalSize(kDefaultSize[8], config().ARENA_8, globalMultiplier, "ARENA_8");
        gArenaMemorySize[9] = Parser::ComputeFinalSize(kDefaultSize[9], config().ARENA_9, globalMultiplier, "ARENA_9");
        gArenaMemorySize[10] = Parser::ComputeFinalSize(kDefaultSize[10], config().ARENA_10, globalMultiplier, "ARENA_10");
        gArenaMemorySize[11] = Parser::ComputeFinalSize(kDefaultSize[11], config().ARENA_11, globalMultiplier, "ARENA_11");
        gArenaMemorySize[12] = Parser::ComputeFinalSize(kDefaultSize[12], config().ARENA_12, globalMultiplier, "ARENA_12");
        gArenaMemorySize[13] = Parser::ComputeFinalSize(kDefaultSize[13], config().ARENA_13, globalMultiplier, "ARENA_13");
        gArenaMemorySize[14] = 0;
        gArenaMemorySize[15] = Parser::ComputeFinalSize(kDefaultSize[15], config().ARENA_15, globalMultiplier, "ARENA_15");
        gArenaMemorySize[16] = 0; // sum of 8,9,10
        gArenaMemorySize[17] = 0;
        gArenaMemorySize[18] = 0;
        gArenaMemorySize[19] = 0;
        gArenaMemorySize[20] = 0;
        gArenaMemorySize[21] = Parser::ComputeFinalSize(kDefaultSize[21], config().ARENA_21, globalMultiplier, "ARENA_21");
        gArenaMemorySize[22] = 0;
        gArenaMemorySize[23] = Parser::ComputeFinalSize(kDefaultSize[23], config().ARENA_23, globalMultiplier, "ARENA_23");
        gArenaMemorySize[16] = gArenaMemorySize[8] + gArenaMemorySize[9] + gArenaMemorySize[10];

        size_t sumSizes = 0;
        for (int id : LayoutOrder)
            sumSizes += gArenaMemorySize[id];
        ArenaTotal = sumSizes;

        // Special split for arenas 10 and 12
        patch::SetUInt(0x45C30A + 1, gArenaMemorySize[10]); // default 1548288
        patch::SetUInt(0x45C2DA + 1, gArenaMemorySize[10] + gArenaMemorySize[12]); // default 2494464
        // Arena 23 setup
        patch::Nop(0x45C1DE, 2); // rep stosd
        patch::RedirectCall(0x45C1C0, AllocArena23);
        patch::SetUInt(0x45C1C7 + 1, gArenaMemorySize[23]);
        // Arena 15 setup (own page-aligned block)
        patch::RedirectCall(0x45C162, InitArena15);
        // Main arena pool
        patch::RedirectCall(0x45C05D, InitArenas);
        patch::Nop(0x831716, 2); // rep stosd
        patch::RedirectCall(0x831706, AllocArenaTotal);
        patch::SetUInt(0x831727 + 2, ArenaTotal);

        //::Message("Total %u MB", ArenaTotal / 1024 / 1024);

        gAllocateAligned = 0x8A04AD;
        gInitArenas = 0x831210;
        gInitArena15 = 0x831100;

        // 
        patch::SetUInt(0x850669 + 1, 814336 * 10);
        patch::SetUInt(0x8506A0 + 2, 326656 * 10);
        patch::SetUInt(0x850952 + 2, 326656 * 10);
        // 
        patch::SetUInt(0x84F4E9 + 1, 61632 * 10);
        patch::SetUInt(0x84F85E + 2, 61632 * 10);
        patch::SetUInt(0x84FDB8 + 2, 61632 * 10);
        patch::SetUInt(0x84FE55 + 2, 61632 * 10);
        // 
        patch::SetUInt(0x84EC89 + 1, 21312 * 10);
        patch::SetUInt(0x84ECEB + 2, 21312 * 10);
        patch::SetUInt(0x84ED35 + 2, 21312 * 10);

        // Test
        //patch::RedirectCall (0xACAFB2, DummyAddToRenderList);

        break;
    }
    }
}
