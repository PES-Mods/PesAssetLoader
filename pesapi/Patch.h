#pragma once
#include <Windows.h>
#include "Error.h"

#define METHOD __fastcall
#if defined(_M_X64)
#define DUMMY_ARG
#else
#define DUMMY_ARG int
#endif
#define NAKED __declspec(naked)
#define VALIDATE_SIZE(struc, size) static_assert(sizeof(struc) == size, "Invalid struct size of " #struc)
#define VALIDATE_OFFSET(struc, member, offset) static_assert(offsetof(struc, member) == offset, "The offset of " #member " in " #struc " is not " #offset)

namespace plugin {

using AddrType = uintptr_t;
using OffsetType = intptr_t;

namespace injector {

inline bool ProtectMemory(AddrType addr, size_t size, DWORD protection) {
    return VirtualProtect((LPVOID)addr, size, protection, &protection) != 0;
}

inline bool UnprotectMemory(AddrType addr, size_t size, DWORD &out_oldprotect) {
    return VirtualProtect((LPVOID)addr, size, PAGE_EXECUTE_READWRITE, &out_oldprotect) != 0;
}

inline bool UnprotectMemory(AddrType addr, size_t size) {
    DWORD out_oldprotect = 0;
    return VirtualProtect((LPVOID)addr, size, PAGE_EXECUTE_READWRITE, &out_oldprotect) != 0;
}
#if defined(_M_X64)
#pragma pack(push, 1)
struct RedirectSlot {
    uint16_t jmpInstr;
    uint32_t jmpOffset;
    uint8_t type;
    uint8_t zero;
    int64_t address;
};
#pragma pack(pop)
VALIDATE_SIZE(RedirectSlot, 16);

namespace StaticMemoryAllocator {
#include <vector>
// RedirectSlot region header (16 bytes)
// <8b SLOTREG\0> <4b totalSlots> <4b usedSlots>
// RedirectSlot slot (16 bytes each slot)
// <6b jmpInstr> <1b status> <1b pad (0)> <8b offset>
// RedirectSlot region footer (16 bytes)
// <7b ENDREG\0>

inline std::vector<std::pair<AddrType, AddrType>> GetDefaultRedirectSlotRegions() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (!base) return {};
    const PIMAGE_DOS_HEADER dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const PIMAGE_NT_HEADERS64 nt = reinterpret_cast<PIMAGE_NT_HEADERS64>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    const uintptr_t ep = base + static_cast<uintptr_t>(nt->OptionalHeader.AddressOfEntryPoint);
    // the region requires at least 48 bytes (16 header + 16 for 1 slot + 16 footer)
    // make sure addresses are 16-byte aligned!
    switch (ep) {
    case 0x151358000: // FIFA 17
        return {
            { 0x143A90790, 0x143A909E0 }
        };
    case 0x149523000: // FIFA 16
    case 0x1439FA151:
        return {
            { 0x1421556D0, 0x14216B6A0 },
            { 0x142142600, 0x142150820 },
            { 0x14216D410, 0x142178F10 },
            { 0x142178F20, 0x142181CF0 },
            { 0x142181D00, 0x142188C20 },
            { 0x142150840, 0x1421556C0 },
            { 0x14216B6B0, 0x14216D3F0 },
            { 0x142142090, 0x1421425F0 },
        };
    case 0x142E7C0E8: // FIFA 15
        return {
            { 0x141F75A98, 0x141F9157C },
            { 0x141F5BB00, 0x141F75A88 },
            { 0x141F91590, 0x141F9BAE0 },
            { 0x141F58000, 0x141F5BAF0 },
        };
    }
    return {};
}

inline std::vector<std::pair<AddrType, AddrType>> &RedirectSlotRanges() {
    static std::vector<std::pair<AddrType, AddrType>> redirectSlotRegions = GetDefaultRedirectSlotRegions();
    return redirectSlotRegions;
}
#pragma pack(push, 1)
struct RedirectSlotRegionHeader {
    char signature[8];
    uint32_t totalSlots;
    uint32_t usedSlots;
};
#pragma pack(pop)
VALIDATE_SIZE(RedirectSlotRegionHeader, 16);

class RedirectSlotRangesInitializer {
public:
    RedirectSlotRangesInitializer(AddrType addr) {
        bool initialized = !strncmp((char const *)addr, "SLOTREG", 8);
        if (!initialized) {
            for (auto const &reg : RedirectSlotRanges()) {
                uint32_t totalSize = (uint32_t)(reg.second - reg.first);
                UnprotectMemory(reg.first, totalSize);
                memset((void *)reg.first, 0, totalSize);
                RedirectSlotRegionHeader *header = (RedirectSlotRegionHeader *)reg.first;
                strcpy(header->signature, "SLOTREG");
                header->totalSlots = (totalSize - 32) / 16;
                header->usedSlots = 0;
                strcpy((char *)(reg.second - 16), "ENDREG");
            }
            initialized = true;
        }
    }
};

inline RedirectSlot *AllocateRedirectSlotFromStaticMemory() {
    if (RedirectSlotRanges().empty())
        return nullptr;
    static RedirectSlotRangesInitializer regInit(RedirectSlotRanges()[0].first);
    for (auto const &reg : RedirectSlotRanges()) {
        RedirectSlotRegionHeader *header = (RedirectSlotRegionHeader *)reg.first;
        if (header->totalSlots != header->usedSlots) {
            RedirectSlot *slot = (RedirectSlot *)(reg.first + (16 + (uint64_t)header->usedSlots * 16));
            header->usedSlots++;
            return slot;
        }
    }
    return nullptr;
}

}

namespace DynamicMemoryAllocator {
#define MEMORY_SLOT_SIZE 16
#define MEMORY_BLOCK_SIZE 0x1000
#define MAX_MEMORY_RANGE 0x40000000
#define PAGE_EXECUTE_FLAGS (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)

typedef struct _MEMORY_SLOT {
    union {
        struct _MEMORY_SLOT *pNext;
        UINT8 buffer[MEMORY_SLOT_SIZE];
    };
} MEMORY_SLOT, *PMEMORY_SLOT;

typedef struct _MEMORY_BLOCK {
    struct _MEMORY_BLOCK *pNext;
    PMEMORY_SLOT pFree;
    UINT usedCount;
} MEMORY_BLOCK, *PMEMORY_BLOCK;

inline PMEMORY_BLOCK &MemoryBlocks() {
    static PMEMORY_BLOCK g_pMemoryBlocks = 0;
    return g_pMemoryBlocks;
}

inline LPVOID FindPrevFreeRegion(LPVOID pAddress, LPVOID pMinAddr, DWORD dwAllocationGranularity) {
    ULONG_PTR tryAddr = (ULONG_PTR)pAddress;
    tryAddr -= tryAddr % dwAllocationGranularity;
    tryAddr -= dwAllocationGranularity;
    while (tryAddr >= (ULONG_PTR)pMinAddr) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPVOID)tryAddr, &mbi, sizeof(mbi)) == 0)
            break;
        if (mbi.State == MEM_FREE)
            return (LPVOID)tryAddr;
        if ((ULONG_PTR)mbi.AllocationBase < dwAllocationGranularity)
            break;
        tryAddr = (ULONG_PTR)mbi.AllocationBase - dwAllocationGranularity;
    }
    return NULL;
}

inline LPVOID FindNextFreeRegion(LPVOID pAddress, LPVOID pMaxAddr, DWORD dwAllocationGranularity) {
    ULONG_PTR tryAddr = (ULONG_PTR)pAddress;
    tryAddr -= tryAddr % dwAllocationGranularity;
    tryAddr += dwAllocationGranularity;
    while (tryAddr <= (ULONG_PTR)pMaxAddr) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPVOID)tryAddr, &mbi, sizeof(mbi)) == 0)
            break;
        if (mbi.State == MEM_FREE)
            return (LPVOID)tryAddr;
        tryAddr = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        tryAddr += dwAllocationGranularity - 1;
        tryAddr -= tryAddr % dwAllocationGranularity;
    }
    return NULL;
}

inline PMEMORY_BLOCK GetMemoryBlock(LPVOID pOrigin) {
    PMEMORY_BLOCK pBlock;
    ULONG_PTR minAddr;
    ULONG_PTR maxAddr;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    minAddr = (ULONG_PTR)si.lpMinimumApplicationAddress;
    maxAddr = (ULONG_PTR)si.lpMaximumApplicationAddress;
    if ((ULONG_PTR)pOrigin > MAX_MEMORY_RANGE && minAddr < (ULONG_PTR)pOrigin - MAX_MEMORY_RANGE)
        minAddr = (ULONG_PTR)pOrigin - MAX_MEMORY_RANGE;
    if (maxAddr > (ULONG_PTR)pOrigin + MAX_MEMORY_RANGE)
        maxAddr = (ULONG_PTR)pOrigin + MAX_MEMORY_RANGE;
    maxAddr -= MEMORY_BLOCK_SIZE - 1;
    for (pBlock = MemoryBlocks(); pBlock != NULL; pBlock = pBlock->pNext) {
        if ((ULONG_PTR)pBlock < minAddr || (ULONG_PTR)pBlock >= maxAddr)
            continue;
        if (pBlock->pFree != NULL)
            return pBlock;
    }
    {
        LPVOID pAlloc = pOrigin;
        while ((ULONG_PTR)pAlloc >= minAddr) {
            pAlloc = FindPrevFreeRegion(pAlloc, (LPVOID)minAddr, si.dwAllocationGranularity);
            if (pAlloc == NULL)
                break;
            pBlock = (PMEMORY_BLOCK)VirtualAlloc(
                pAlloc, MEMORY_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (pBlock != NULL)
                break;
        }
    }
    if (pBlock == NULL) {
        LPVOID pAlloc = pOrigin;
        while ((ULONG_PTR)pAlloc <= maxAddr) {
            pAlloc = FindNextFreeRegion(pAlloc, (LPVOID)maxAddr, si.dwAllocationGranularity);
            if (pAlloc == NULL)
                break;
            pBlock = (PMEMORY_BLOCK)VirtualAlloc(
                pAlloc, MEMORY_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (pBlock != NULL)
                break;
        }
    }
    if (pBlock != NULL) {
        PMEMORY_SLOT pSlot = (PMEMORY_SLOT)pBlock + 1;
        pBlock->pFree = NULL;
        pBlock->usedCount = 0;
        do {
            pSlot->pNext = pBlock->pFree;
            pBlock->pFree = pSlot;
            pSlot++;
        } while ((ULONG_PTR)pSlot - (ULONG_PTR)pBlock <= MEMORY_BLOCK_SIZE - MEMORY_SLOT_SIZE);

        pBlock->pNext = MemoryBlocks();
        MemoryBlocks() = pBlock;
    }
    return pBlock;
}

inline LPVOID AllocateBuffer(LPVOID pOrigin) {
    PMEMORY_SLOT  pSlot;
    PMEMORY_BLOCK pBlock = GetMemoryBlock(pOrigin);
    if (pBlock == NULL)
        return NULL;
    pSlot = pBlock->pFree;
    pBlock->pFree = pSlot->pNext;
    pBlock->usedCount++;
    return pSlot;
}

inline RedirectSlot *AllocateRedirectSlotFromDynamicMemory(AddrType pOrigin) {
    PMEMORY_SLOT pSlot = (PMEMORY_SLOT)AllocateBuffer((LPVOID)pOrigin);
    if (pSlot)
        return (RedirectSlot *)pSlot->buffer;
    return nullptr;
}

}
#endif
struct scoped_unprotect {
    AddrType addr = 0;
    size_t size = 0;
    DWORD dwOldProtect = 0;
    bool bUnprotected = false;

    scoped_unprotect(AddrType addr, size_t size) {
        if (size == 0)
            bUnprotected = false;
        else
            bUnprotected = UnprotectMemory(this->addr = addr, this->size = size, dwOldProtect);
    }

    ~scoped_unprotect() {
        if (bUnprotected)
            ProtectMemory(this->addr, this->size, this->dwOldProtect);
    }
};

inline static std::vector<uint8_t> HexStringToBytes(std::string const &str) {
    std::vector<uint8_t> result;
    uint8_t tempDigit = 0;
    bool tempFlag = false;
    auto hexVal = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return 0;
    };
    for (auto ch : str) {
        if (ch == ' ')
            continue;
        else if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) {
            uint8_t thisDigit = hexVal(ch);
            if (!tempFlag) {
                tempDigit = thisDigit << 4;
                tempFlag = true;
            }
            else {
                tempDigit |= thisDigit;
                tempFlag = false;
                result.push_back(tempDigit);
            }
        }
    }
    return result;
}

inline std::string BytesToHexString(void const *data, size_t size) {
    if (size == 0)
        return std::string{};
    unsigned char const *bytes = (unsigned char const *)data;
    std::string out;
    out.reserve(size * 3 - 1);
    static constexpr char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) {
        unsigned char b = bytes[i];
        out.push_back(HEX[b >> 4]);
        out.push_back(HEX[b & 0x0F]);
        if (i + 1 != size)
            out.push_back(' ');
    }
    return out;
}

inline std::string BytesToHexString(std::vector<uint8_t> const &vecBytes) {
    return BytesToHexString(vecBytes.data(), vecBytes.size());
}

inline void WriteMemoryRaw(AddrType addr, void *value, size_t size, bool vp) {
    scoped_unprotect xprotect(addr, vp ? size : 0);
    memcpy((void *)addr, value, size);
}

inline void ReadMemoryRaw(AddrType addr, void *ret, size_t size, bool vp) {
    scoped_unprotect xprotect(addr, vp ? size : 0);
    memcpy(ret, (void *)addr, size);
}

inline void WriteHexBytes(AddrType addr, std::string const &hexBytes, bool vp) {
    auto vecBytes = HexStringToBytes(hexBytes);
    if (!vecBytes.empty())
        WriteMemoryRaw(addr, vecBytes.data(), vecBytes.size(), vp);
}

inline std::string ReadHexBytes(AddrType addr, size_t size, bool vp) {
    if (size == 0)
        return std::string();
    std::vector<uint8_t> vecBytes(size, 0);
    ReadMemoryRaw(addr, vecBytes.data(), size, vp);
    return BytesToHexString(vecBytes);
}

inline void MemoryFill(AddrType addr, uint8_t value, size_t size, bool vp) {
    scoped_unprotect xprotect(addr, vp ? size : 0);
    memset((void *)addr, value, size);
}

template<class T>
inline T &WriteObject(AddrType addr, const T &value, bool vp = false) {
    scoped_unprotect xprotect(addr, vp ? sizeof(value) : 0);
    return (*(T *)addr = value);
}

template<class T>
inline T &ReadObject(AddrType addr, T &value, bool vp = false) {
    scoped_unprotect xprotect(addr, vp ? sizeof(value) : 0);
    return (value = *(T *)addr);
}

template<class T>
inline void WriteMemory(AddrType addr, T value, bool vp = false) {
    WriteObject(addr, value, vp);
}

template<class T>
inline T ReadMemory(AddrType addr, bool vp = false) {
    T value;
    return ReadObject(addr, value, vp);
}

inline AddrType GetAbsoluteOffset(OffsetType rel_value, AddrType end_of_instruction) {
    return end_of_instruction + rel_value;
}

inline OffsetType GetRelativeOffset(AddrType abs_value, AddrType end_of_instruction) {
    return (OffsetType)abs_value - (OffsetType)end_of_instruction;
}

inline AddrType ReadRelativeOffset(AddrType at, size_t sizeof_addr = 4, bool vp = true) {
    switch (sizeof_addr) {
    case 1: return (GetAbsoluteOffset(ReadMemory<int8_t>(at, vp), at + sizeof_addr));
    case 2: return (GetAbsoluteOffset(ReadMemory<int16_t>(at, vp), at + sizeof_addr));
    case 4: return (GetAbsoluteOffset(ReadMemory<int32_t>(at, vp), at + sizeof_addr));
    }
    return 0;
}

inline void MakeRelativeOffset(AddrType at, AddrType dest, size_t sizeof_addr = 4, bool vp = true) {
    switch (sizeof_addr) {
    case 1:
        WriteMemory<int8_t>(at, static_cast<int8_t> (GetRelativeOffset(dest, at + sizeof_addr)), vp);
        break;
    case 2:
        WriteMemory<int16_t>(at, static_cast<int16_t>(GetRelativeOffset(dest, at + sizeof_addr)), vp);
        break;
    case 4:
        WriteMemory<int32_t>(at, static_cast<int32_t>(GetRelativeOffset(dest, at + sizeof_addr)), vp);
        break;
    }
}

inline AddrType GetBranchDestination(AddrType at, bool vp = true) {
    uint8_t op = ReadMemory<uint8_t>(at, vp);
    if (op == 0xE8 || op == 0xE9)
        return ReadRelativeOffset(at + 1, 4, vp);
    if (op == 0xFF) {
        uint8_t modrm = ReadMemory<uint8_t>(at + 1, vp);
        if (modrm == 0x15 || modrm == 0x25) {
#if defined(_M_X64)
            int32_t disp = ReadMemory<int32_t>(at + 2, vp);
            AddrType pointer_addr = at + 6 + static_cast<AddrType>(disp);
            return ReadMemory<AddrType>(pointer_addr, vp);
#else
            AddrType pointer_addr = ReadMemory<AddrType>(at + 2, vp);
            return ReadMemory<AddrType>(pointer_addr, vp);
#endif
        }
    }
    return 0;
}

#if defined(_M_X64)
enum SlotType : uint8_t { SLOT_UNDEFINED = 0, SLOT_JUMP = 1, SLOT_POINTER = 2 };

inline AddrType GetRedirectSlotAddress(AddrType at, AddrType dest, uint8_t type) {
    auto slot = DynamicMemoryAllocator::AllocateRedirectSlotFromDynamicMemory(at);
    if (!slot) {
        slot = StaticMemoryAllocator::AllocateRedirectSlotFromStaticMemory();
        if (!slot) {
            plugin::Error("Unable to allocate slot for hook at address %llX", at);
            return 0;
        }
        else {
            //plugin::Warning("Unable to allocate slot for hook at address %llX. Slot from static memory will be used.", at);
        }
    }
    slot->type = type;
    slot->zero = 0;
    slot->address = dest;
    if (type == SLOT_JUMP) {
        slot->jmpOffset = 2;
        slot->jmpInstr = 0x25FF;
        FlushInstructionCache(GetCurrentProcess(), &slot->jmpInstr, 6);
        return (AddrType)&slot->jmpInstr;
    }
    return (AddrType)&slot->address;
}
#endif
inline void MakeNOP(AddrType at, size_t count = 1, bool vp = true) {
    MemoryFill(at, 0x90, count, vp);
}

inline void MakeRangedNOP(AddrType at, AddrType until, bool vp = true) {
    return MakeNOP(at, until - at, vp);
}

inline void MakeRET(AddrType at, uint16_t pop = 0, bool vp = true) {
    WriteMemory(at, pop ? 0xC2 : 0xC3, vp);
    if (pop) WriteMemory(at + 1, pop, vp);
}

inline AddrType MakeJMP(AddrType at, AddrType dest, bool vp = true) {
    auto p = GetBranchDestination(at, vp);
#if defined(_M_X64)
    dest = GetRedirectSlotAddress(at, dest, SLOT_JUMP);
    if (!dest)
        return p;
#endif
    uint8_t op = ReadMemory<uint8_t>(at, vp);
    if (op == 0xFF) {
        uint8_t modrm = ReadMemory<uint8_t>(at + 1, vp);
        if (modrm == 0x15 || modrm == 0x25)
            MakeNOP(at + 5, 1, vp);
    }
    WriteMemory<uint8_t>(at, 0xE9, vp);
    MakeRelativeOffset(at + 1, dest, 4, vp);
    return p;
}

inline AddrType MakeCALL(AddrType at, AddrType dest, bool vp = true) {
    auto p = GetBranchDestination(at, vp);
#if defined(_M_X64)
    dest = GetRedirectSlotAddress(at, dest, SLOT_JUMP);
    if (!dest)
        return p;
#endif
    uint8_t op = ReadMemory<uint8_t>(at, vp);
    if (op == 0xFF) {
        uint8_t modrm = ReadMemory<uint8_t>(at + 1, vp);
        if (modrm == 0x15 || modrm == 0x25)
            MakeNOP(at + 5, 1, vp);
    }
    WriteMemory<uint8_t>(at, 0xE8, vp);
    MakeRelativeOffset(at + 1, dest, 4, vp);
    return p;
}

inline bool MakeUnconditional(AddrType at, bool vp = true) {
    uint8_t b0 = ReadMemory<uint8_t>(at, vp);
    if ((b0 & 0xF0) == 0x70) {
        WriteMemory<uint8_t>(at, 0xEB, vp);
        return true;
    }
    if (b0 == 0x0F) {
        uint8_t b1 = ReadMemory<uint8_t>(at + 1, vp);
        if ((b1 & 0xF0) == 0x80) {
            AddrType dest = ReadRelativeOffset(at + 2, 4, vp);
            WriteMemory<uint8_t>(at, 0xE9, vp);
            MakeRelativeOffset(at + 1, dest, 4, vp);
            MakeNOP(at + 5, 1, vp);
            return true;
        }
    }
    return false;
}

inline bool InvertCondition(AddrType at, bool vp = true) {
    uint8_t b0 = ReadMemory<uint8_t>(at, vp);
    if ((b0 & 0xF0) == 0x70) {
        uint8_t newop = static_cast<uint8_t>(b0 ^ 1);
        WriteMemory<uint8_t>(at, newop, vp);
        return true;
    }
    if (b0 == 0x0F) {
        uint8_t b1 = ReadMemory<uint8_t>(at + 1, vp);
        if ((b1 & 0xF0) == 0x80) {
            uint8_t newb1 = static_cast<uint8_t>(b1 ^ 1);
            WriteMemory<uint8_t>(at + 1, newb1, vp);
            return true;
        }
    }
    return false;
}

#if defined(_M_X64)
inline static AddrType SetFunction(AddrType at, uint32_t offset, AddrType dest, bool vp = true) {
    auto p = ReadRelativeOffset(at + offset, 4, vp);
    auto slot = GetRedirectSlotAddress(at, dest, SLOT_JUMP);
    if (slot) {
        MakeRelativeOffset(at + offset, slot, 4, vp);
        FlushInstructionCache(GetCurrentProcess(), (void *)at, offset + 4);
    }
    return p;
}

inline static AddrType SetFunctionPointer(AddrType at, uint32_t offset, AddrType dest, bool vp = true) {
    auto p = ReadRelativeOffset(at + offset, 4, vp);
    auto slot = GetRedirectSlotAddress(at, dest, SLOT_POINTER);
    if (slot) {
        MakeRelativeOffset(at + offset, slot, 4, vp);
        FlushInstructionCache(GetCurrentProcess(), (void *)at, offset + 4);
    }
    return p;
}
#endif
}

template<typename RawType = void, typename Type>
RawType *raw_ptr(Type *p, OffsetType offset = 0) {
    return reinterpret_cast<RawType *>(reinterpret_cast<AddrType>(p) + offset);
}

template <AddrType address, typename... Args>
void Call(Args... args) {
    reinterpret_cast<void(__cdecl *)(Args...)>(address)(args...);
}

template <typename Ret, AddrType address, typename... Args>
Ret CallAndReturn(Args... args) {
    return reinterpret_cast<Ret(__cdecl *)(Args...)>(address)(args...);
}

template <AddrType address, typename C, typename... Args>
void CallMethod(C _this, Args... args) {
    reinterpret_cast<void(__thiscall *)(C, Args...)>(address)(_this, args...);
}

template <typename Ret, AddrType address, typename C, typename... Args>
Ret CallMethodAndReturn(C _this, Args... args) {
    return reinterpret_cast<Ret(__thiscall *)(C, Args...)>(address)(_this, args...);
}

template <unsigned int tableIndex, typename C, typename... Args>
void CallVirtualMethod(C _this, Args... args) {
    reinterpret_cast<void(__thiscall *)(C, Args...)>((*reinterpret_cast<void ***>(_this))[tableIndex])(_this, args...);
}

template <typename Ret, unsigned int tableIndex, typename C, typename... Args>
Ret CallVirtualMethodAndReturn(C _this, Args... args) {
    return reinterpret_cast<Ret(__thiscall *)(C, Args...)>((*reinterpret_cast<void ***>(_this))[tableIndex])(_this, args...);
}

template <unsigned int tableOffset, typename C, typename... Args>
void CallVirtualMethodOffset(C _this, Args... args) {
    (*raw_ptr<void(__thiscall *)(C, Args...)>(*reinterpret_cast<void **>(_this), tableOffset))(_this, args...);
}

template <typename Ret, unsigned int tableOffset, typename C, typename... Args>
Ret CallVirtualMethodOffsetAndReturn(C _this, Args... args) {
    return (*raw_ptr<Ret(__thiscall *)(C, Args...)>(*reinterpret_cast<void **>(_this), tableOffset))(_this, args...);
}

template <AddrType address, typename... Args>
void CallWinApi(Args... args) {
    reinterpret_cast<void(__stdcall *)(Args...)>(address)(args...);
}

template <typename Ret, AddrType address, typename... Args>
Ret CallWinApiAndReturn(Args... args) {
    return reinterpret_cast<Ret(__stdcall *)(Args...)>(address)(args...);
}

template <typename... Args>
void CallDynGlobal(AddrType address, Args... args) {
    reinterpret_cast<void(__cdecl *)(Args...)>(address)(args...);
}

template <typename Ret, typename... Args>
Ret CallAndReturnDynGlobal(AddrType address, Args... args) {
    return reinterpret_cast<Ret(__cdecl *)(Args...)>(address)(args...);
}

template <typename C, typename... Args>
void CallMethodDynGlobal(AddrType address, C _this, Args... args) {
    reinterpret_cast<void(__thiscall *)(C, Args...)>(address)(_this, args...);
}

template <typename Ret, typename C, typename... Args>
Ret CallMethodAndReturnDynGlobal(AddrType address, C _this, Args... args) {
    return reinterpret_cast<Ret(__thiscall *)(C, Args...)>(address)(_this, args...);
}

template<typename Ret, AddrType address, typename... Args>
Ret CallRVO(Args... args) {
    alignas(Ret) uint8_t buf[sizeof(Ret)];
    Ret *temp = reinterpret_cast<Ret *>(buf);
    Call<address>(temp, args...);
    Ret result(std::move(*temp));
    temp->~Ret();
    return result;
}

template<typename Ret, AddrType address, typename C, typename... Args>
Ret CallMethodRVO(C _this, Args... args) {
    alignas(Ret) uint8_t buf[sizeof(Ret)];
    Ret *temp = reinterpret_cast<Ret *>(buf);
    CallMethod<address>(_this, temp, args...);
    Ret result(std::move(*temp));
    temp->~Ret();
    return result;
}

template<typename Ret, typename... Args>
Ret CallRVODynGlobal(AddrType address, Args... args) {
    alignas(Ret) uint8_t buf[sizeof(Ret)];
    Ret *temp = reinterpret_cast<Ret *>(buf);
    Call(address, temp, args...);
    Ret result(std::move(*temp));
    temp->~Ret();
    return result;
}

template<typename Ret, typename C, typename... Args>
Ret CallMethodRVODynGlobal(AddrType address, C _this, Args... args) {
    alignas(Ret) uint8_t buf[sizeof(Ret)];
    Ret *temp = reinterpret_cast<Ret *>(buf);
    CallMethodDynGlobal(address, _this, temp, args...);
    Ret result(std::move(*temp));
    temp->~Ret();
    return result;
}

template <typename... Args>
void CallWinApiDynGlobal(AddrType address, Args... args) {
    reinterpret_cast<void(__stdcall *)(Args...)>(address)(args...);
}

template <typename Ret, typename... Args>
Ret CallWinApiAndReturnDynGlobal(AddrType address, Args... args) {
    return reinterpret_cast<Ret(__stdcall *)(Args...)>(address)(args...);
}

class patch {
public:
    inline static void Nop(AddrType address, unsigned int size) {
        injector::MakeNOP(address, size, true);
    }

    inline static void NopFromTo(AddrType addressFrom, AddrType addressTo) {
        injector::MakeNOP(addressFrom, addressTo - addressFrom, true);
    }

    inline static AddrType RedirectCall(AddrType address, void *func) {
        return injector::MakeCALL(address, (AddrType)func, true);
    }

    inline static AddrType RedirectCall(AddrType address, AddrType func) {
        return injector::MakeCALL(address, func, true);
    }

    inline static AddrType RedirectJump(AddrType address, void *func) {
        return injector::MakeJMP(address, (AddrType)func, true);
    }

    inline static AddrType RedirectJump(AddrType from, AddrType to) {
        return injector::MakeJMP(from, to, true);
    }

    inline static bool MakeUnconditional(AddrType address) {
        return injector::MakeUnconditional(address, true);
    }

    inline static bool InvertCondition(AddrType address) {
        return injector::InvertCondition(address, true);
    }

#if defined(_M_X64)
    inline static AddrType SetFunction(AddrType address, uint32_t offset, void *func) {
        return injector::SetFunction(address, offset, (AddrType)func, true);
    }

    inline static AddrType SetFunctionPointer(AddrType address, uint32_t offset, void *func) {
        return injector::SetFunctionPointer(address, offset, (AddrType)func, true);
    }
#endif

    inline static void SetChar(AddrType address, char value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetUChar(AddrType address, unsigned char value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetShort(AddrType address, short value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetUShort(AddrType address, unsigned short value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetInt(AddrType address, int value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetUInt(AddrType address, unsigned int value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetInt64(AddrType address, int64_t value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetUInt64(AddrType address, uint64_t value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetFloat(AddrType address, float value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static void SetDouble(AddrType address, double value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    inline static AddrType SetPointer(AddrType address, void *value, bool vp = true) {
        AddrType oldValue = injector::ReadMemory<AddrType>(address, vp);
        injector::WriteMemory(address, value, vp);
        return oldValue;
    }

    inline static AddrType SetPointer(AddrType address, char const *value, bool vp = true) {
        AddrType oldValue = injector::ReadMemory<AddrType>(address, vp);
        injector::WriteMemory(address, value, vp);
        return oldValue;
    }

    inline static AddrType SetPointer(AddrType address, wchar_t const *value, bool vp = true) {
        AddrType oldValue = injector::ReadMemory<AddrType>(address, vp);
        injector::WriteMemory(address, value, vp);
        return oldValue;
    }

    inline static char GetChar(AddrType address, bool vp = true) {
        return injector::ReadMemory<char>(address, vp);
    }

    inline static unsigned char GetUChar(AddrType address, bool vp = true) {
        return injector::ReadMemory<unsigned char>(address, vp);
    }

    inline static short GetShort(AddrType address, bool vp = true) {
        return injector::ReadMemory<short>(address, vp);
    }

    inline static unsigned short GetUShort(AddrType address, bool vp = true) {
        return injector::ReadMemory<unsigned short>(address, vp);
    }

    inline static int GetInt(AddrType address, bool vp = true) {
        return injector::ReadMemory<int>(address, vp);
    }

    inline static unsigned int GetUInt(AddrType address, bool vp = true) {
        return injector::ReadMemory<unsigned int>(address, vp);
    }

    inline static int64_t GetInt64(AddrType address, bool vp = true) {
        return injector::ReadMemory<int64_t>(address, vp);
    }

    inline static uint64_t GetUInt64(AddrType address, bool vp = true) {
        return injector::ReadMemory<uint64_t>(address, vp);
    }

    inline static float GetFloat(AddrType address, bool vp = true) {
        return injector::ReadMemory<float>(address, vp);
    }

    inline static double GetDouble(AddrType address, bool vp = true) {
        return injector::ReadMemory<double>(address, vp);
    }

    inline static void *GetPointer(AddrType address, bool vp = true) {
        return injector::ReadMemory<void *>(address, vp);
    }

    template <typename T>
    static void Set(AddrType address, T value, bool vp = true) {
        injector::WriteMemory(address, value, vp);
    }

    template <typename T>
    static T Get(AddrType address, bool vp = true) {
        return injector::ReadMemory<T>(address, vp);
    }

    inline static void SetBytes(AddrType addr, std::string const &hexBytes, bool vp = true) {
        injector::WriteHexBytes(addr, hexBytes, vp);
    }

    inline static std::string GetBytes(AddrType addr, size_t size, bool vp = true) {
        return injector::ReadHexBytes(addr, size, vp);
    }

    inline static void MemCpy(AddrType addr, void *value, size_t size, bool vp = true) {
        injector::WriteMemoryRaw(addr, value, size, vp);
    }

    inline static void MemCpy(void *addr, AddrType value, size_t size, bool vp = true) {
        injector::WriteMemoryRaw((AddrType)addr, (void *)value, size, vp);
    }

    inline static void MemCpy(AddrType addr, AddrType value, size_t size, bool vp = true) {
        injector::WriteMemoryRaw(addr, (void *)value, size, vp);
    }

    inline static void MemCpy(void *addr, void *value, size_t size, bool vp = true) {
        injector::WriteMemoryRaw((AddrType)addr, value, size, vp);
    }

    inline static void NopCall(AddrType addr, size_t numParams = 0) {
        if (numParams == 0)
            Nop(addr, 5);
        else {
            SetUChar(addr, 0x83);
            SetUChar(addr + 1, 0xC4);
            SetUChar(addr + 2, static_cast<unsigned char>(numParams * 4));
            Nop(addr + 3, 2);
        }
    }

    inline static void NopWinApiCall(AddrType addr, size_t numParams = 0) {
        if (numParams == 0)
            Nop(addr, 6);
        else {
            SetUChar(addr, 0x83);
            SetUChar(addr + 1, 0xC4);
            SetUChar(addr + 2, static_cast<unsigned char>(numParams * 4));
            Nop(addr + 3, 3);
        }
    }
};

}
