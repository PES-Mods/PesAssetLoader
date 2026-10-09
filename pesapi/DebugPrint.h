#pragma once
#include "plugin.h"

class PesDebug {
    HMODULE hDebugModule = 0;
    uintptr_t gPesDebugPrint = 0;
public:
    PesDebug() {
        hDebugModule = LoadLibraryA("plugins\\PesDebug.asi");
        if (hDebugModule)
            gPesDebugPrint = (uintptr_t)GetProcAddress(hDebugModule, "DebugPrint");
    }
    ~PesDebug() {
        if (hDebugModule)
            FreeLibrary(hDebugModule);
    }
    void Print(char const *text) {
        if (gPesDebugPrint)
            plugin::CallDynGlobal(gPesDebugPrint, text);
    }

    static PesDebug &GetInstance() {
        static PesDebug fifamDebug;
        return fifamDebug;
    }
};

inline void DebugPrint(std::string const &message) {
    PesDebug::GetInstance().Print(message.c_str());
}
