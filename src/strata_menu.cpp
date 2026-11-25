// The video panel, rumble, the hints' buttons and Panorama's menu size.
#include "d3d11_hooks.h"
#include "strata_hooks.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <iostream>
#include "MinHook.h"
#include "game.h"
#include "vr.h"

namespace StrataHooks
{
    // Videos (logic_playmovie) play in a VideoPanel, drawn into the back buffer the eyes don't see.
    // [133] Paint: the override of the empty Panel::Paint that draws a black rect.
    // [133] rva 0x441170: 40 55 48 83 EC 30 48 8B E9 48 8B 0D ?? ?? ?? ??
    constexpr int kVideoPanel_Paint = 133;
    constexpr ULONGLONG kVideoPanel_VisibleMs = 250; // since the last Paint
    typedef void (*tVideoPanelPaint)(void *);
    tVideoPanelPaint oVideoPanelPaint = nullptr;
    std::atomic<ULONGLONG> g_VideoPanelPainted{0};

    void hkVideoPanelPaint(void *thisptr)
    {
        g_VideoPanelPainted = GetTickCount64();
        oVideoPanelPaint(thisptr);
    }

    // Init's part: the video panel.
    void InstallVideoPanelHook()
    {
        void **videoPanel = FindCheckedVTable("client.dll", "VideoPanel", 241);
        if (videoPanel)
            InstallHook("VideoPanel Paint", videoPanel[kVideoPanel_Paint], &hkVideoPanelPaint, oVideoPanelPaint);
        else
            std::cout << "[Hooks] VideoPanel vtable not found, videos stay invisible in the headset\n";
    }
}

// Rumble: the game's gamepad rumble goes to the VR controllers. Path in client.dll: UM_Rumble ->
// RumbleEffect -> StartEffect puts the effect into one of 3 channels per user; each frame
// UpdateEffects mixes the channels and calls CInputSystem [37] SetRumble(left, right, user).
// StartEffect and UpdateEffects both ask CInput [14] (controller mode) and do nothing or zero the
// motors without a gamepad: [14] answers true for exactly these two calls. Changing the whole game
// to controller mode would show the gamepad's buttons in the hints and change ~128 other places.
namespace StrataHooks
{
    // rvas and bytes: Steam build 16912226 (client.dll PE timestamp 1736038611). Without a match
    // there is no rumble and a line in the log, nothing else changes.
    // RumbleEffect: sub rsp, 0x38; ... lea rcx, [rip + g_RumbleEffects]
    constexpr uint32_t kRumbleEffect_Rva = 0x25ea50;
    constexpr uint8_t kRumbleEffect_Start[] = { 0x48, 0x83, 0xEC, 0x38, 0x44, 0x88, 0x4C, 0x24, 0x20, 0x45, 0x0F, 0xB6, 0xC8,
                                                0x44, 0x0F, 0xB6, 0xC2, 0x8B, 0xD1, 0x48, 0x8D, 0x0D };
    // In StartEffect: call [rax+0x70] (CInput [14]); test al, al; je
    constexpr uint32_t kRumbleStartCheck_ReturnRva = 0x25eb31;
    constexpr uint8_t kRumbleStartCheck_Bytes[] = { 0xFF, 0x50, 0x70, 0x84, 0xC0, 0x0F, 0x84, 0xBC, 0x01, 0x00, 0x00 };
    // In UpdateEffects: call [rax+0x70] (CInput [14]); test al, al; jne; xorps xmm8, xmm8; xorps xmm7, xmm7
    constexpr uint32_t kRumbleUpdateCheck_ReturnRva = 0x25f266;
    constexpr uint8_t kRumbleUpdateCheck_Bytes[] = { 0xFF, 0x50, 0x70, 0x84, 0xC0, 0x75, 0x07, 0x45, 0x0F, 0x57, 0xC0, 0x0F, 0x57, 0xFF };
    constexpr int kInput_ControllerModeActive = 14;
    constexpr int kInputSystem_SetRumble = 37;

    // g_RumbleEffects: per user 0x3c bytes, 3 channels of 0x14: +4 effect index, +0xc active.
    constexpr int kRumble_UserSize = 0x3c, kRumble_Channels = 3, kRumble_ChannelSize = 0x14;
    constexpr int kRumble_ChannelEffect = 4, kRumble_ChannelActive = 0xc;
    // Effects of the portal gun (fired, fizzled on a surface without portals): only the gun hand.
    constexpr int kRumbleEffect_PortalGunFired = 25, kRumbleEffect_PortalGunFizzled = 27;

    using tControllerModeActive = bool (*)(void *thisptr);
    using tSetRumble = void (*)(void *thisptr, float left, float right, int user);
    tControllerModeActive oControllerModeActive = nullptr;
    tSetRumble oSetRumble = nullptr;
    const uint8_t *g_RumbleStartCheckReturn = nullptr;
    const uint8_t *g_RumbleUpdateCheckReturn = nullptr;
    const uint8_t *g_RumbleEffects = nullptr;

    std::atomic<float> g_RumbleAmplitude{0};
    std::atomic<bool> g_RumbleGunOnly{false};
    std::atomic<ULONGLONG> g_RumbleSetAt{0};

    bool hkControllerModeActive(void *thisptr)
    {
        const void *caller = _ReturnAddress();
        if (caller == g_RumbleStartCheckReturn || caller == g_RumbleUpdateCheckReturn)
            return true;
        return oControllerModeActive(thisptr);
    }

    // True if all active channels of the user hold portal gun effects.
    bool RumbleGunOnly(int user)
    {
        if (user < 0 || user > 1)
            return false;
        bool gun = false;
        for (int i = 0; i < kRumble_Channels; ++i)
        {
            const uint8_t *channel = g_RumbleEffects + user * kRumble_UserSize + i * kRumble_ChannelSize;
            if (!channel[kRumble_ChannelActive])
                continue;
            const int effect = *reinterpret_cast<const int *>(channel + kRumble_ChannelEffect);
            if (effect != kRumbleEffect_PortalGunFired && effect != kRumbleEffect_PortalGunFizzled)
                return false;
            gun = true;
        }
        return gun;
    }

    void hkSetRumble(void *thisptr, float left, float right, int user)
    {
        if (user == 0)
        {
            const float amplitude = (std::max)(left, right);
            static bool logged = false;
            if (!logged && amplitude > 0)
            {
                logged = true;
                std::cout << "[Rumble] first rumble from the game\n";
            }
            g_RumbleAmplitude = amplitude;
            g_RumbleGunOnly = amplitude > 0 && RumbleGunOnly(user);
            g_RumbleSetAt = GetTickCount64();
        }
        oSetRumble(thisptr, left, right, user);
    }

    void InstallRumbleHooks()
    {
        auto client = reinterpret_cast<uint8_t *>(GetModuleHandleA("client.dll"));
        const uint8_t *rumbleEffect = client + kRumbleEffect_Rva;
        const uint8_t *startCheck = client + kRumbleStartCheck_ReturnRva - 3;
        const uint8_t *updateCheck = client + kRumbleUpdateCheck_ReturnRva - 3;
        void **input = StrataHooks::FindCheckedVTable("client.dll", "CInput", 72);
        void **inputSystem = StrataHooks::FindCheckedVTable("inputsystem.dll", "CInputSystem", 92);
        if (!input || !inputSystem || memcmp(rumbleEffect, kRumbleEffect_Start, sizeof(kRumbleEffect_Start)) != 0
            || memcmp(startCheck, kRumbleStartCheck_Bytes, sizeof(kRumbleStartCheck_Bytes)) != 0
            || memcmp(updateCheck, kRumbleUpdateCheck_Bytes, sizeof(kRumbleUpdateCheck_Bytes)) != 0)
        {
            std::cout << "[Hooks] rumble not found (other game version?): no rumble in the VR controllers\n";
            return;
        }
        const uint8_t *lea = rumbleEffect + sizeof(kRumbleEffect_Start);
        g_RumbleEffects = lea + 4 + *reinterpret_cast<const int32_t *>(lea);
        g_RumbleStartCheckReturn = client + kRumbleStartCheck_ReturnRva;
        g_RumbleUpdateCheckReturn = client + kRumbleUpdateCheck_ReturnRva;
        InstallHook("Rumble SetRumble", inputSystem[kInputSystem_SetRumble], &hkSetRumble, oSetRumble, "inputsystem.dll");
        InstallHook("Rumble CInput ControllerModeActive", input[kInput_ControllerModeActive], &hkControllerModeActive, oControllerModeActive);
    }
}

bool StrataHooks::Rumble(float &amplitude, bool &gunOnly)
{
    constexpr ULONGLONG kStaleMs = 100; // SetRumble comes every frame while there is a player
    const bool player = GetTickCount64() - g_RumbleSetAt < kStaleMs;
    amplitude = player ? (float)g_RumbleAmplitude : 0.0f;
    gunOnly = player && g_RumbleGunOnly;
    return player;
}

// Hints of the game instructor: client.dll asks the engine for the key names of a lesson's command
// (CEngineClient [157] Key_LookupBindingEx(binding, slot, nth, flags), nth = 0, 1, ... until null)
// and shows them in key icons. For those two calls the mod answers with the VR button instead
// (VR::HintButtonName); key names elsewhere (e.g. the game's key settings) stay.
namespace StrataHooks
{
    // Steam build 16912226. CEngineClient [157]: a jmp to Key_LookupBindingEx with the arguments moved.
    constexpr int kEngineClient_KeyLookupBinding = 157;
    constexpr uint8_t kKeyLookupBinding_Start[] = { 0x41, 0x8B, 0xC1, 0x45, 0x8B, 0xD0, 0x44, 0x8B, 0x4C, 0x24, 0x28,
                                                    0x48, 0x8B, 0xCA, 0x44, 0x8B, 0xC0, 0x41, 0x8B, 0xD2, 0xE9 };
    // The calls in client.dll (call [rax+0x4e8]) for the first key and for the next ones.
    constexpr uint32_t kHintLookupFirst_ReturnRva = 0x3566ba;
    constexpr uint32_t kHintLookupNext_ReturnRva = 0x3566fe;
    constexpr uint8_t kHintLookup_Call[] = { 0xFF, 0x90, 0xE8, 0x04, 0x00, 0x00 };

    using tKeyLookupBinding = const char *(*)(void *thisptr, const char *binding, int slot, int nth, int flags);
    tKeyLookupBinding oKeyLookupBinding = nullptr;
    const uint8_t *g_HintLookupFirstReturn = nullptr;
    const uint8_t *g_HintLookupNextReturn = nullptr;

    const char *hkKeyLookupBinding(void *thisptr, const char *binding, int slot, int nth, int flags)
    {
        const void *caller = _ReturnAddress();
        if (binding && g_StrataGame->m_VR && (caller == g_HintLookupFirstReturn || caller == g_HintLookupNextReturn))
        {
            // One button per command: nullptr after the first ends the list.
            if (const char *button = g_StrataGame->m_VR->HintButtonName(binding))
                return nth == 0 ? button : nullptr;
        }
        return oKeyLookupBinding(thisptr, binding, slot, nth, flags);
    }

    void InstallHintHooks()
    {
        auto client = reinterpret_cast<uint8_t *>(GetModuleHandleA("client.dll"));
        const uint8_t *first = client + kHintLookupFirst_ReturnRva - sizeof(kHintLookup_Call);
        const uint8_t *next = client + kHintLookupNext_ReturnRva - sizeof(kHintLookup_Call);
        void **engineClient = StrataHooks::FindCheckedVTable("engine.dll", "CEngineClient", 207);
        auto lookup = engineClient ? static_cast<const uint8_t *>(engineClient[kEngineClient_KeyLookupBinding]) : nullptr;
        if (!lookup || memcmp(lookup, kKeyLookupBinding_Start, sizeof(kKeyLookupBinding_Start)) != 0
            || memcmp(first, kHintLookup_Call, sizeof(kHintLookup_Call)) != 0 || memcmp(next, kHintLookup_Call, sizeof(kHintLookup_Call)) != 0)
        {
            std::cout << "[Hooks] hint key lookup not found (other game version?): the hints show keyboard keys\n";
            return;
        }
        g_HintLookupFirstReturn = client + kHintLookupFirst_ReturnRva;
        g_HintLookupNextReturn = client + kHintLookupNext_ReturnRva;
        InstallHook("Hints Key_LookupBindingEx", engineClient[kEngineClient_KeyLookupBinding], &hkKeyLookupBinding, oKeyLookupBinding, "engine.dll");
    }
}

// Panorama lays out for a height of 1080 and the width that follows from its window size. For the
// back buffer (HMD format, nearly square) that's about 1030 wide, too narrow for the menu. So its
// windows get a part of the back buffer in the aspect ratio of the video mode. The size is passed
// when they are created and with SetClientSize after a mode change: changing the fields alone
// doesn't arrange the panels again.
namespace StrataHooks
{
    // RTTI names end in "@panorama". CUIEngineSource2 [13] CreateTopLevelWindow, called by client.dll;
    // CTopLevelWindowSource2 [9] SetUIScale (stores at +0x88, [10] returns it), [17] SetClientSize.
    // [13] rva 0x174030: 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48
    // [9] rva 0x0c9910: 48 89 74 24 10 57 48 83 EC 20 F3 0F 11 89 88 00
    // [17] rva 0x178320: 40 53 48 83 EC 20 48 83 B9 08 02 00 00 00 48 8B
    constexpr int kPanoramaEngine_CreateWindow = 13; // CUIEngineSource2
    constexpr int kPanoramaWindow_SetUIScale = 9;    // CTopLevelWindowSource2
    constexpr int kPanoramaWindow_SetClientSize = 17;
    constexpr int kPanoramaWindow_ClientHeightField = 0x4C;
    constexpr float kPanorama_LayoutHeight = 1080.0f;
    constexpr int kPanorama_FindRetries = 5000; // ms, the DLL may still be loading

    typedef void *(*tPanoramaCreateWindow)(void *, int, int, int, int, uint64_t, uint64_t, uint64_t, uint64_t, void *, void *);
    typedef void (*tPanoramaSetUIScale)(void *, float, uint64_t, uint64_t);
    typedef uint64_t (*tPanoramaSetClientSize)(void *, uint64_t, uint64_t, uint64_t);
    tPanoramaCreateWindow oPanoramaCreateWindow = nullptr;
    tPanoramaSetUIScale oPanoramaSetUIScale = nullptr;
    tPanoramaSetClientSize oPanoramaSetClientSize = nullptr;
    void **g_PanoramaWindowVTable = nullptr;
    std::atomic<bool> g_PanoramaHooked{false};

    // Aspect ratio of the video mode (the flat window), which the menu takes: so it fills the window
    // undistorted. 16:9 until it is known.
    std::atomic<int> g_ModeWidth{16};
    std::atomic<int> g_ModeHeight{9};

    void SetModeSize(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;
        g_ModeWidth = width;
        g_ModeHeight = height;
    }

    // The height for the full width of the back buffer in the mode's aspect ratio, at most all of it.
    int MenuHeightFor(int width, int height)
    {
        const int menuHeight = static_cast<int>(static_cast<int64_t>(width) * g_ModeHeight / g_ModeWidth);
        return menuHeight < height ? menuHeight : height;
    }

    // The windows are created with the back buffer's size; the mode's is the game window's.
    void *hkPanoramaCreateWindow(void *engine, int x, int y, int width, int height, uint64_t a, uint64_t b, uint64_t c, uint64_t d, void *e, void *f)
    {
        DXGI_SWAP_CHAIN_DESC desc;
        RECT rect;
        if (D3D11Hooks::g_SwapChain && SUCCEEDED(D3D11Hooks::g_SwapChain->GetDesc(&desc)) && GetClientRect(desc.OutputWindow, &rect))
            SetModeSize(rect.right, rect.bottom);
        const int newHeight = MenuHeightFor(width, height);
        std::cout << "[Panorama] create window " << width << "x" << height << " for " << g_ModeWidth << "x" << g_ModeHeight
                  << " -> " << width << "x" << newHeight << "\n";
        return oPanoramaCreateWindow(engine, x, y, width, newHeight, a, b, c, d, e, f);
    }

    // The engine sets height / 1080 with the real height, when a window is created and after a
    // resize of the screen.
    void hkPanoramaSetUIScale(void *window, float scale, uint64_t a, uint64_t b)
    {
        const int clientHeight = *reinterpret_cast<const int *>(static_cast<const uint8_t *>(window) + kPanoramaWindow_ClientHeightField);
        if (*static_cast<void ***>(window) == g_PanoramaWindowVTable && clientHeight > 0)
            scale = clientHeight / kPanorama_LayoutHeight;
        oPanoramaSetUIScale(window, scale, a, b);
    }

    // "Apply" in the video settings sets the video mode again, even unchanged. Then the windows get
    // the size of the mode (the flat window) instead of the back buffer's, and the menu ended up
    // in the top left corner. They always get the back buffer's width, the height for the mode's
    // aspect ratio.
    uint64_t hkPanoramaSetClientSize(void *window, uint64_t width, uint64_t height, uint64_t a)
    {
        const VR *vr = g_StrataGame ? g_StrataGame->m_VR : nullptr;
        if (vr && vr->m_RenderWidth)
            SetModeSize((int)width, (int)height);
        const int newWidth = vr && vr->m_RenderWidth ? (int)vr->m_RenderWidth : (int)width;
        const int newHeight = MenuHeightFor(newWidth, vr && vr->m_RenderHeight ? (int)vr->m_RenderHeight : (int)height);
        std::cout << "[Panorama] window size " << width << "x" << height << " -> " << newWidth << "x" << newHeight << "\n";
        return oPanoramaSetClientSize(window, newWidth, newHeight, a);
    }
}

bool StrataHooks::VideoVisible()
{
    const ULONGLONG painted = g_VideoPanelPainted;
    return painted && GetTickCount64() - painted < kVideoPanel_VisibleMs;
}

int StrataHooks::PanoramaHeight(int width, int height)
{
    return g_PanoramaHooked ? MenuHeightFor(width, height) : height;
}

unsigned long __stdcall StrataHooks::HookPanorama(void *)
{
    while (!GetModuleHandleA("panorama.dll"))
        Sleep(1);

    void **engine = nullptr;
    for (int i = 0; i < kPanorama_FindRetries && !(engine && g_PanoramaWindowVTable); ++i)
    {
        engine = FindVTable("panorama.dll", "CUIEngineSource2@panorama");
        g_PanoramaWindowVTable = FindVTable("panorama.dll", "CTopLevelWindowSource2@panorama");
        if (!engine || !g_PanoramaWindowVTable)
            Sleep(1);
    }
    if (!engine || !g_PanoramaWindowVTable)
    {
        std::cout << "[Panorama] vtables not found, the menu keeps the format of the back buffer\n";
        return 0;
    }
    // The lengths of Steam build 16912226, which the indices were checked with.
    const int engineLength = VTableLength(engine, "panorama.dll"), windowLength = VTableLength(g_PanoramaWindowVTable, "panorama.dll");
    if (engineLength != 183 || windowLength != 88)
    {
        std::cout << "[Panorama] vtables with " << engineLength << " and " << windowLength
                  << " entries instead of 183 and 88: the game has changed, the menu keeps the format of the back buffer\n";
        g_PanoramaWindowVTable = nullptr;
        return 0;
    }

    MH_Initialize(); // MH_ERROR_ALREADY_INITIALIZED is fine
    g_PanoramaHooked =
        InstallHook("Panorama SetUIScale", g_PanoramaWindowVTable[kPanoramaWindow_SetUIScale], &hkPanoramaSetUIScale, oPanoramaSetUIScale, "panorama.dll") &&
        InstallHook("Panorama SetClientSize", g_PanoramaWindowVTable[kPanoramaWindow_SetClientSize], &hkPanoramaSetClientSize, oPanoramaSetClientSize, "panorama.dll") &&
        InstallHook("Panorama CreateWindow", engine[kPanoramaEngine_CreateWindow], &hkPanoramaCreateWindow, oPanoramaCreateWindow, "panorama.dll");
    return 0;
}
