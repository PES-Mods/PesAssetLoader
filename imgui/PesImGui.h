#pragma once
#include "imgui.h"
#include "plugin.h"
#include <Windows.h>

namespace PesImGuiDetail {

inline HMODULE GetModule() {
    static HMODULE hMod = []() -> HMODULE {
        std::wstring p = PES::GameDirPath(L"plugins\\PesImGui.dll");
        return LoadLibraryW(p.c_str());
    }();
    return hMod;
}

inline FARPROC GetProc(const char *name) {
    HMODULE h = GetModule();
    if (!h) return nullptr;
    return GetProcAddress(h, name);
}

}

inline ImGuiContext *ImGuiGetContext() {
    using Fn = ImGuiContext * (*)();
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiGetContext"));
    return fn ? fn() : nullptr;
}

inline void ImGuiSyncContext() {
    ImGui::SetCurrentContext(ImGuiGetContext());
}

#define IMGUI_SYNC ImGuiSyncContext();

inline void ImGuiOnCreate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiOnCreate"));
    if (fn) fn(callback);
}

inline void ImGuiOnDestroy(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiOnDestroy"));
    if (fn) fn(callback);
}

inline void ImGuiOnUpdate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiOnUpdate"));
    if (fn) fn(callback);
}

inline void ImGuiOnRender(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiOnRender"));
    if (fn) fn(callback);
}

inline void ImGuiOnLost(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiOnLost"));
    if (fn) fn(callback);
}

inline void ImGuiOnReset(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiOnReset"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnCreate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiRemoveOnCreate"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnDestroy(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiRemoveOnDestroy"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnUpdate(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiRemoveOnUpdate"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnRender(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiRemoveOnRender"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnLost(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiRemoveOnLost"));
    if (fn) fn(callback);
}

inline void ImGuiRemoveOnReset(void (*callback)()) {
    using Fn = void (*)(void (*)());
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiRemoveOnReset"));
    if (fn) fn(callback);
}

inline const char *ImGuiGetHostVersion() {
    using Fn = const char *(*)();
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiGetVersion"));
    return fn ? fn() : nullptr;
}

inline void ImGuiEnableMouseCursor() {
    using Fn = void (*)();
    static Fn fn = reinterpret_cast<Fn>(PesImGuiDetail::GetProc("ExpImGuiEnableMouseCursor"));
    if (fn) fn();
}

inline void ImGuiCheckVersion() {
    const char *hostVersion = ImGuiGetHostVersion();
    if (!hostVersion || strcmp(hostVersion, IMGUI_VERSION) != 0) {
        MessageBoxA(nullptr,
            "PesImGui version mismatch — your plugin was built against a "
            "different Dear ImGui version than PesImGui.dll. Rebuild against "
            "the matching imgui headers.",
            "PesImGui", MB_ICONERROR);
    }
}

inline bool ImGuiEnsureLoaded(bool showErrorIfMissing = true) {
    HMODULE h = PesImGuiDetail::GetModule();
    if (!h) {
        if (showErrorIfMissing) {
            MessageBoxA(nullptr,
                "PesImGui.dll was not found in the plugins folder.\n\n"
                "This plugin requires PesImGui.dll to display its UI. "
                "Please install it alongside your other .asi plugins.",
                "Missing dependency: PesImGui.dll",
                MB_ICONERROR);
        }
        return false;
    }
    return true;
}
