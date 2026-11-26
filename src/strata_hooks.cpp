// The hooks' core: the eyes' views, the HUD, the network fields and Init. The other parts:
// strata_arms, _buttons, _gun, _menu, _movement, _vtables.cpp.
#include "d3d11_hooks.h"
#include "strata_hooks.h"
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include "MinHook.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"

namespace StrataHooks
{
    // After a game update: vtables are found by RTTI class name, the log names those whose length
    // changed. rva and first bytes: Steam build 16912226; ?? = displacement, changes every build.

    // CViewRender [6] RenderView(view, hudView, nClearFlags, whatToDraw), called by [5] Render
    // ("CViewRender::Render").
    // [6] rva 0x37e5c0: 48 8B C4 44 89 48 20 4C 89 40 18 48 89 50 10 55
    constexpr int kViewRender_RenderView = 6;
    constexpr int RENDERVIEW_DRAWHUD = 1 << 1; // Render() sets it when there is a local player

    using tRenderView = void (*)(void *thisptr, const CViewSetup &view, const CViewSetup &hudView, int nClearFlags, int whatToDraw);
    tRenderView oRenderView = nullptr;

    // C_Prop_Portal fields; forward of the angles is the portal's normal. The matrix isn't a network
    // field (kPortal_MatrixToLinked); those below are looked up in FindNetworkFields and keep these
    // values if that fails.
    int g_Portal_Origin = 0x33e8;
    int g_Portal_Angles = 0x3418;
    int g_Portal_LinkedPortal = 0x3448;
    int g_Portal_Activated = 0x5d80;
    int g_Portal_HalfWidth = 0x5d84;
    int g_Portal_HalfHeight = 0x5d88;
    void **g_PropPortalVTable = nullptr;

    void **g_PortalPlayerVTable = nullptr;
    // The game's eye this frame, as hkCalcViewModelView sees it (the eyes' renders change it): if
    // it's through a portal, the arms are made on the player's side (moved back through the exit).
    bool g_GameEyeThrough = false;
    const uint8_t *g_GameEyeExit = nullptr;

    Game *g_StrataGame = nullptr;
    void *g_ViewModelOwner = nullptr; // the player, as last seen by hkCalcViewModelView

    // CVRenderView (engine.dll) [41] VGui_Paint(mode), calls EngineVGui()->Paint(mode). From
    // RenderView with PAINT_INGAMEPANELS: the HUD, into a texture. From Render with PAINT_UIPANELS
    // (captions among them): to the HUD overlay in the game, to the back buffer in the menus.
    // [41] rva 0x019710: 40 53 48 83 EC 20 8B DA E8 ?? ?? ?? ?? 8B D3 48
    constexpr int kRenderView_VGuiPaint = 41;
    constexpr int PAINT_UIPANELS = 1 << 0;
    constexpr int PAINT_INGAMEPANELS = 1 << 1;
    using tVGuiPaint = void (*)(void *thisptr, int mode);
    tVGuiPaint oVGuiPaint = nullptr;
    bool g_HudPass = false; // the eye that draws the HUD is rendering
    bool g_HudPainting = false; // the HUD is drawn for the HUD overlay

    // CModAppSystemGroup (engine.dll) [8] Shutdown, after the main loop: ends the VR connection.
    // [8] rva 0x22fbc0: 40 53 48 83 EC 20 80 B9 A0 00 00 00 00 48 8B D9
    constexpr int kModAppSystemGroup_Shutdown = 8;
    using tAppSystemGroupShutdown = void (*)(void *thisptr);
    tAppSystemGroupShutdown oAppSystemGroupShutdown = nullptr;

    void hkAppSystemGroupShutdown(void *thisptr)
    {
        g_StrataGame->m_VR->Shutdown();
        oAppSystemGroupShutdown(thisptr);
    }

    bool VRViewActive();

    void hkVGuiPaint(void *thisptr, int mode)
    {
        static int loggedModes = 0;
        if (!(loggedModes & (1 << (mode & 7))))
        {
            loggedModes |= 1 << (mode & 7);
            std::cout << "[Hooks] VGui_Paint mode " << mode << (g_HudPass ? " in the HUD pass" : "") << "\n";
        }
        if (!g_HudPass && (mode & PAINT_UIPANELS) && VRViewActive())
        {
            VR *vr = g_StrataGame->m_VR;
            if (!vr->BeginHudPaint(true))
                return oVGuiPaint(thisptr, mode);
            g_HudPainting = true;
            oVGuiPaint(thisptr, mode);
            g_HudPainting = false;
            vr->EndHudPaint(true);
            return;
        }
        if (!g_HudPass || mode != PAINT_INGAMEPANELS)
            return oVGuiPaint(thisptr, mode);
        g_HudPass = false;
        VR *vr = g_StrataGame->m_VR;
        if (!vr->BeginHudPaint())
            return; // in the eye it would sit in a corner
        g_HudPainting = true;
        oVGuiPaint(thisptr, mode);
        g_HudPainting = false;
        vr->EndHudPaint();
    }

    // Crosshairs, replaced by the aim dot: [130] Paint of the panel at 0x60 ([89] uses
    // "crosshair_default"). rva and first bytes:
    // CHUDQuickInfo 0x3f6960: 40 55 41 54 41 57 48 8D AC 24 30 FD FF FF 48 81
    // CHudCrosshair 0x4446c0: 40 55 57 48 8D AC 24 D8 FE FF FF 48 81 EC 28 02
    // CHudPortalCrosshair 0x3f8070: 40 53 48 81 EC A0 00 00 00 48 83 B9 10 02 00 00
    constexpr uint32_t kHudElement_Panel = 0x60;
    constexpr int kPanel_HudPaint = 130;
    using tPanelPaint = void (*)(void *thisptr);
    tPanelPaint oQuickInfoPaint = nullptr;
    tPanelPaint oCrosshairPaint = nullptr;
    tPanelPaint oPortalCrosshairPaint = nullptr;

    void hkQuickInfoPaint(void *thisptr)
    {
        if (!g_HudPainting)
            oQuickInfoPaint(thisptr);
    }

    void hkCrosshairPaint(void *thisptr)
    {
        if (!g_HudPainting)
            oCrosshairPaint(thisptr);
    }

    void hkPortalCrosshairPaint(void *thisptr)
    {
        if (!g_HudPainting)
            oPortalCrosshairPaint(thisptr);
    }

    // The eyes are rendered and the viewmodel is in the hand. During a video the menu overlay
    // shows it from the back buffer.
    bool VRViewActive()
    {
        VR *vr = g_StrataGame->m_VR;
        return vr->m_IsInitialized && !vr->IsMenuVisible() && !StrataHooks::VideoVisible();
    }

    bool g_LoggedMemoryNear = false;

    // What MinHook has to search for its trampolines, which must be close to the target: the memory
    // within 1 GB of it. See module_placement.cpp for how a DLL ends up without any.
    void LogMemoryNear(const uint8_t *target)
    {
        constexpr uintptr_t kRange = 0x40000000, kGranularity = 0x10000;
        int regions = 0, tooSmall = 0, usable = 0, failed = 0;
        DWORD lastError = 0;
        for (uintptr_t address = (uintptr_t)target - kRange; address < (uintptr_t)target + kRange;)
        {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof(info)))
                break;
            ++regions;
            if (info.State == MEM_FREE)
            {
                const uintptr_t aligned = (address + kGranularity - 1) & ~(kGranularity - 1);
                if (aligned + 0x1000 > address + info.RegionSize)
                    ++tooSmall;
                else if (void *block = VirtualAlloc(reinterpret_cast<void *>(aligned), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                {
                    VirtualFree(block, 0, MEM_RELEASE);
                    ++usable;
                }
                else
                {
                    ++failed;
                    lastError = GetLastError();
                }
            }
            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }
        std::cout << "[Hooks] within 1 GB of " << (const void *)target << ": " << regions << " regions, free ones: " << usable << " usable, "
                  << tooSmall << " too small, " << failed << " refused (error " << lastError << ")\n";
    }

    bool InstallHookAt(const char *name, void *target, void *detour, void **original, const char *module)
    {
        const MH_STATUS created = MH_CreateHook(target, detour, original);
        if (created == MH_ERROR_MEMORY_ALLOC && !g_LoggedMemoryNear)
        {
            g_LoggedMemoryNear = true;
            LogMemoryNear(static_cast<const uint8_t *>(target));
        }
        const MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
        std::cout << "[Hooks] " << name << " " << module << "+0x" << std::hex << ((uint8_t *)target - (uint8_t *)GetModuleHandleA(module))
                  << std::dec << ": create " << created << ", enable " << enabled << "\n";
        return enabled == MH_OK;
    }

    // Panorama lays the menu out for a part of the back buffer, and the overlay shows that.
    // So the scene behind the menu gets the same format, with the view's vertical field of view.
    CViewSetup MenuView(const CViewSetup &view)
    {
        VR *vr = g_StrataGame->m_VR;
        const float aspect = static_cast<float>(vr->m_RenderWidth) / vr->MenuHeight();
        const float scale = aspect / view.m_flAspectRatio;
        auto scaleFov = [scale](float fov) { return RAD2DEG(2.0f * atanf(tanf(DEG2RAD(fov) * 0.5f) * scale)); };

        CViewSetup menuView = view;
        menuView.x = menuView.m_nUnscaledX = 0;
        menuView.y = menuView.m_nUnscaledY = 0;
        menuView.width = menuView.m_nUnscaledWidth = vr->m_RenderWidth;
        menuView.height = menuView.m_nUnscaledHeight = vr->MenuHeight();
        menuView.fov = scaleFov(view.fov);
        menuView.fovViewmodel = scaleFov(view.fovViewmodel);
        menuView.m_flAspectRatio = aspect;
        return menuView;
    }

    // The game's network tables, as in client.dll. They name the fields, so these survive a patch
    // that moves them.
    struct RecvTable;
    struct RecvProp
    {
        const char *name;
        int type, flags, stringBufferSize;
        bool insideArray;
        const void *extraData;
        RecvProp *arrayProp;
        void *arrayLengthProxy, *proxy, *dataTableProxy;
        RecvTable *dataTable;
        int offset, elementStride, elements;
        const char *parentArrayName;
    };
    static_assert(sizeof(RecvProp) == 0x60 && offsetof(RecvProp, offset) == 0x48);
    struct RecvTable
    {
        RecvProp *props;
        int propCount;
        void *decoder;
        const char *name;
    };
    struct ClientClass
    {
        void *create, *createEvent;
        const char *name;
        RecvTable *table;
        ClientClass *next;
    };
    // VClient018 is CHLClient: [8] returns the list of client classes,
    // rva 0x270a10: 48 8B 05 ?? ?? ?? ?? C3 (mov rax, [global]; ret)
    constexpr int kBaseClient_GetAllClasses = 8;

    const struct
    {
        const char *table, *name;
        int *offset;
    } kNetworkFields[] =
    {
        { "DT_Prop_Portal", "m_ptOrigin", &g_Portal_Origin },
        { "DT_Prop_Portal", "m_qAbsAngle", &g_Portal_Angles },
        { "DT_Prop_Portal", "m_hLinkedPortal", &g_Portal_LinkedPortal },
        { "DT_Prop_Portal", "m_bActivated", &g_Portal_Activated },
        { "DT_Prop_Portal", "m_fNetworkHalfWidth", &g_Portal_HalfWidth },
        { "DT_Prop_Portal", "m_fNetworkHalfHeight", &g_Portal_HalfHeight },
        { "DT_Portal_Player", "m_fFlags", &g_Player_Flags },
        { "DT_Portal_Player", "m_vecViewOffset[2]", &g_Player_ViewHeight },
        { "DT_Portal_Player", "m_hActiveWeapon", &g_Player_ActiveWeaponHandle },
        { "DT_Portal_Player", "m_bIsHoldingSomething", &g_Player_HoldingSomething },
        { "DT_WeaponPortalgun", "m_bCanFirePortal1", &g_Portalgun_CanFirePortal1 },
        { "DT_WeaponPortalgun", "m_bCanFirePortal2", &g_Portalgun_CanFirePortal2 },
        { "DT_WeaponPortalgun", "m_iLastFiredPortal", &g_Portalgun_LastFiredPortal },
        { "DT_WeaponPortalgun", "m_bOpenProngs", &g_Portalgun_OpenProngs },
        { "DT_BaseEntity", "m_nModelIndex", &g_Entity_ModelIndex },
        { "DT_BaseEntity", "m_vecMins", &g_Entity_Mins },
        { "DT_BaseEntity", "m_vecMaxs", &g_Entity_Maxs },
    };

    // From the start of the table, including its nested tables; -1 if it isn't there.
    int FindNetworkField(const RecvTable *table, const char *name)
    {
        for (int i = 0; i < table->propCount; ++i)
        {
            if (!table->props[i].dataTable && !strcmp(table->props[i].name, name))
                return table->props[i].offset;
        }
        for (int i = 0; i < table->propCount; ++i)
        {
            if (const RecvTable *nested = table->props[i].dataTable)
            {
                const int offset = FindNetworkField(nested, name);
                if (offset >= 0)
                    return table->props[i].offset + offset;
            }
        }
        return -1;
    }

    void ReadNetworkTables(void *baseClient)
    {
        using tGetAllClasses = ClientClass *(*)(void *thisptr);
        const ClientClass *classes = (*static_cast<tGetAllClasses **>(baseClient))[kBaseClient_GetAllClasses](baseClient);

        int found = 0;
        for (const auto &field : kNetworkFields)
        {
            int offset = -1;
            for (const ClientClass *c = classes; c && offset < 0; c = c->next)
            {
                if (c->table && !strcmp(c->table->name, field.table))
                    offset = FindNetworkField(c->table, field.name);
            }

            if (offset < 0)
            {
                std::cout << "[Hooks] network field " << field.name << " not found, stays at 0x" << std::hex << *field.offset << std::dec << "\n";
                continue;
            }
            ++found;
            if (offset != *field.offset)
                std::cout << "[Hooks] network field " << field.name << " moved from 0x" << std::hex << *field.offset << " to 0x" << offset << std::dec << "\n";
            *field.offset = offset;
        }
        std::cout << "[Hooks] network fields: found " << found << " of " << std::size(kNetworkFields) << "\n";
    }

    // Once client.dll has built its tables. A game whose tables look different keeps the values above.
    void FindNetworkFields()
    {
        void *baseClient = g_StrataGame->m_BaseClientDll;
        if (!baseClient)
            return;
        __try
        {
            ReadNetworkTables(baseClient);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            std::cout << "[Hooks] network tables not readable\n";
        }
    }

    // Renders both eyes into the VR textures instead of the one view into the back buffer. Menus
    // (main menu with its background map, pause menu) keep rendering to the back buffer, which
    // the menu overlay shows.

    void hkRenderView(void *thisptr, const CViewSetup &view, const CViewSetup &hudView, int nClearFlags, int whatToDraw)
    {
        static const bool networkFieldsRead = (FindNetworkFields(), true);
        UpdateArmsHooks();

        VR *vr = g_StrataGame->m_VR;
        if (!VRViewActive())
        {
            RemoveLaser();
            g_Roomscale.hasPosition = false;
            D3D11Hooks::TraceNote("flat view");
            if (vr->m_IsInitialized && vr->IsMenuVisible())
                return oRenderView(thisptr, MenuView(view), hudView, nClearFlags, whatToDraw);
            return oRenderView(thisptr, view, hudView, nClearFlags, whatToDraw);
        }

        if (!vr->m_CreatedVRTextures)
            vr->CreateVRTextures();

        if (!vr->m_LeftEye.m_Texture || !vr->m_RightEye.m_Texture)
        {
            static bool logged = false;
            if (!logged)
            {
                std::cout << "[VR] stereo: no eye textures, rendering flat\n";
                logged = true;
            }
            return oRenderView(thisptr, view, hudView, nClearFlags, whatToDraw);
        }

        const Vector position = view.origin;
        ++g_PortalPassage.frames;
        FollowPortalPassage(position, vr->m_SetupOrigin);
        // Normally done with the viewmodel, which is placed earlier in the frame.
        if (!g_Roomscale.updatedThisFrame)
            UpdateRoomscale(position);
        g_Roomscale.updatedThisFrame = false;
        vr->UpdateVignette(WalkedDistance(position, vr->m_SetupOrigin));
        vr->m_SetupOrigin = position;

        const Vector viewOrigin = vr->GetViewOrigin(position);
        if (g_AimValid && !g_ShotFromEyes && vr->m_AimMode == kAimMode_Dot && !GunCovers(viewOrigin, g_Aim))
            vr->ShowAimDot(g_Aim, viewOrigin);
        else
            vr->HideAimDot();

        // The player looks where the HMD looks (movement direction, aiming).
        const Vector hmdAngle = vr->GetViewAngle();
        QAngle inGameAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z);
        g_StrataGame->m_EngineClient->SetViewAngles(inGameAngle);

        CViewSetup eyeView = view;
        eyeView.x = eyeView.m_nUnscaledX = 0;
        eyeView.y = eyeView.m_nUnscaledY = 0;
        eyeView.width = eyeView.m_nUnscaledWidth = vr->m_RenderWidth;
        eyeView.height = eyeView.m_nUnscaledHeight = vr->m_RenderHeight;
        eyeView.fov = vr->m_Fov;
        eyeView.fovViewmodel = vr->m_Fov;
        eyeView.m_flAspectRatio = vr->m_Aspect;
        eyeView.zNear = 6;
        eyeView.zNearViewmodel = 2;
        eyeView.angles = hmdAngle;

        // The HUD is laid out for the window; drawn into the eyes it would sit in a corner. The
        // right eye draws it into a texture of its own (hkVGuiPaint), the HUD overlay shows it.
        const int eyeWhatToDraw = whatToDraw & ~RENDERVIEW_DRAWHUD;
        const int hudWhatToDraw = oVGuiPaint ? whatToDraw : eyeWhatToDraw;
        if (hudWhatToDraw & RENDERVIEW_DRAWHUD)
            vr->SetHudRect(hudView.x, hudView.y, hudView.width, hudView.height);

        // The engine renders each eye as it renders the flat view; only the result in the back
        // buffer is complete (parts like the portal views go to an internal target, which is
        // drawn into the back buffer last). So it's copied from there, no redirected target.
        //
        // An eye can be through a portal while the player isn't yet: it's beside and in front of the
        // game's eye. Behind the portal's wall it would see nothing, so it looks from the other side,
        // with the gun moved along. The game is told, or it draws its copy of the player right there.
        auto renderEye = [&](SharedTextureHolder &eye, const Vector &origin, int eyeDraws)
        {
            eyeView.origin = origin;
            eyeView.angles = hmdAngle;

            const uint8_t *portal = g_PropPortalVTable ? FindPortalBetween(position, origin) : nullptr;
            const bool moveGun = portal && g_Gun.viewModel;
            uint8_t *player = nullptr;
            bool savedThrough = false;
            uint8_t *savedExit = nullptr;
            if (portal)
            {
                player = reinterpret_cast<uint8_t *>(g_StrataGame->GetClientEntity(g_StrataGame->m_EngineClient->GetLocalPlayer()));
                if (player && *reinterpret_cast<void ***>(player) != g_PortalPlayerVTable)
                    player = nullptr;
                if (player)
                {
                    // Already set if the game's eye is through a portal: this eye is back on the player's side then.
                    uint8_t *&exit = *reinterpret_cast<uint8_t **>(player + kPlayer_ViewExitPortal);
                    savedThrough = player[kPlayer_ViewThroughPortal] != 0;
                    savedExit = exit;
                    player[kPlayer_ViewThroughPortal] = !savedThrough;
                    exit = reinterpret_cast<uint8_t *>(g_StrataGame->GetClientEntity(*reinterpret_cast<const int *>(portal + g_Portal_LinkedPortal) & 0xFFFF));
                }

                auto matrix = reinterpret_cast<const float (*)[4]>(portal + kPortal_MatrixToLinked);
                eyeView.origin = MovedByPortal(matrix, origin);
                const QAngle turned = TurnedByPortal(matrix, inGameAngle);
                eyeView.angles = Vector(turned.x, turned.y, turned.z);
                if (moveGun)
                    oCalcViewModelView(g_Gun.viewModel, g_Gun.owner, MovedByPortal(matrix, g_Gun.position), TurnedByPortal(matrix, g_Gun.angles));
            }

            g_HudPass = (eyeDraws & RENDERVIEW_DRAWHUD) != 0;
            oRenderView(thisptr, eyeView, hudView, nClearFlags, eyeDraws);
            g_HudPass = false;
            vr->CopyBackBufferToEye(eye);

            if (moveGun)
                oCalcViewModelView(g_Gun.viewModel, g_Gun.owner, g_Gun.position, g_Gun.angles);
            if (player)
            {
                player[kPlayer_ViewThroughPortal] = savedThrough;
                *reinterpret_cast<uint8_t **>(player + kPlayer_ViewExitPortal) = savedExit;
            }
        };
        D3D11Hooks::TraceNote("left eye");
        renderEye(vr->m_LeftEye, vr->GetViewOriginLeft(position), eyeWhatToDraw);
        D3D11Hooks::TraceNote("right eye");
        renderEye(vr->m_RightEye, vr->GetViewOriginRight(position), hudWhatToDraw);
        D3D11Hooks::TraceNote("eyes done");
        g_Gun.viewModel = nullptr; // placed again in the next frame, if there still is one

        static bool logged = false;
        if (!logged)
        {
            std::cout << "[VR] stereo: first frame, game view " << view.width << "x" << view.height << " fov " << view.fov
                      << " aspect " << view.m_flAspectRatio << " -> eyes " << eyeView.width << "x" << eyeView.height << " fov "
                      << eyeView.fov << " aspect " << eyeView.m_flAspectRatio << ", whatToDraw " << whatToDraw << "\n";
            logged = true;
        }

        vr->m_RenderedNewFrame = true;
    }
}

bool StrataHooks::Init(Game *game)
{
    g_StrataGame = game;
    std::cout << "[Hooks] client.dll at " << (void *)GetModuleHandleA("client.dll") << ", engine.dll at " << (void *)GetModuleHandleA("engine.dll") << "\n";

    // The D3D11 hooks have initialized MinHook already.
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        std::cout << "[Hooks] MH_Initialize failed: " << status << "\n";
        return false;
    }

    // The vtable lengths are those of Steam build 16912226, which the indices were checked with.
    void **viewRender = FindCheckedVTable("client.dll", "CViewRender", 44);
    if (!viewRender)
    {
        std::cout << "[Hooks] CViewRender vtable not found, no stereo rendering\n";
        return false;
    }

    bool ok = InstallHook("RenderView", viewRender[kViewRender_RenderView], &hkRenderView, oRenderView);

    if (!InstallMovementHooks(ok))
        return false;

    if (!InstallViewModelHooks(ok))
        return false;

    FindButtons(game);

    InstallVideoPanelHook();

    void **renderView = FindCheckedVTable("engine.dll", "CVRenderView", 57);
    if (!renderView || !InstallHook("VGui_Paint", renderView[kRenderView_VGuiPaint], &hkVGuiPaint, oVGuiPaint, "engine.dll"))
    {
        oVGuiPaint = nullptr;
        std::cout << "[Hooks] VGui_Paint not hooked, no HUD and no subtitles in the headset\n";
    }
    void **appSystemGroup = FindCheckedVTable("engine.dll", "CModAppSystemGroup", 11);
    if (!appSystemGroup || !InstallHook("CModAppSystemGroup Shutdown", appSystemGroup[kModAppSystemGroup_Shutdown], &hkAppSystemGroupShutdown, oAppSystemGroupShutdown, "engine.dll"))
        std::cout << "[Hooks] CModAppSystemGroup Shutdown not hooked, SteamVR shows the last frame until the game has quit\n";
    void **quickInfo = FindCheckedVTable("client.dll", "CHUDQuickInfo", 212, kHudElement_Panel);
    if (!quickInfo || !InstallHook("CHUDQuickInfo Paint", quickInfo[kPanel_HudPaint], &hkQuickInfoPaint, oQuickInfoPaint))
        std::cout << "[Hooks] CHUDQuickInfo not found, its crosshair is on the HUD overlay\n";
    void **crosshair = FindCheckedVTable("client.dll", "CHudCrosshair", 212, kHudElement_Panel);
    void **portalCrosshair = FindCheckedVTable("client.dll", "CHudPortalCrosshair", 212, kHudElement_Panel);
    if (!crosshair || !InstallHook("CHudCrosshair Paint", crosshair[kPanel_HudPaint], &hkCrosshairPaint, oCrosshairPaint))
        std::cout << "[Hooks] CHudCrosshair not found, its crosshair is on the HUD overlay\n";
    if (!portalCrosshair || !InstallHook("CHudPortalCrosshair Paint", portalCrosshair[kPanel_HudPaint], &hkPortalCrosshairPaint, oPortalCrosshairPaint))
        std::cout << "[Hooks] CHudPortalCrosshair not found, its crosshair is on the HUD overlay\n";

    g_PropPortalVTable = FindVTable("client.dll", "C_Prop_Portal");
    std::cout << "[Hooks] C_Prop_Portal vtable " << (g_PropPortalVTable ? "found" : "not found, an eye in a portal sees black") << "\n";

    void **player = FindCheckedVTable("client.dll", "C_Portal_Player", 420);
    g_PortalPlayerVTable = player;
    if (!player)
    {
        std::cout << "[Hooks] C_Portal_Player vtable not found, the view doesn't turn with portals\n";
        return false;
    }
    InstallPortalTeleportationHook(player, ok);

    InstallArmsHooks(player);

    if (!InstallShotHooks(game, ok))
        return false;

    InstallRumbleHooks();
    InstallHintHooks();

    return ok;
}
