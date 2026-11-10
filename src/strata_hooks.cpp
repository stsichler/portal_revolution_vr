#include "d3d11_hooks.h"
#include "strata_hooks.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "MinHook.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"

namespace
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

    // ClientModeShared [24] CreateMove(sampleTime, cmd), called by CInput [3] CreateMove; returning
    // true makes cmd->viewangles the engine's view angles.
    // [24] rva 0x287530: 40 53 48 83 EC 30 0F 29 74 24 20 B9 FF FF FF FF
    constexpr int kClientMode_CreateMove = 24;

    using tCreateMove = bool (*)(void *thisptr, float flInputSampleTime, CUserCmd *cmd);
    tCreateMove oCreateMove = nullptr;

    // C_BaseViewModel [254] CalcViewModelView(owner, eyePosition, eyeAngles) (uses the path of
    // baseviewmodel_shared.cpp): lag [253], bob [255], then places the viewmodel.
    // [254] rva 0x194c00: 48 8B C4 48 89 70 18 48 89 78 20 55 48 8D 68 E8
    // [253] rva 0x194a10: 48 89 5C 24 10 57 48 81 EC 80 00 00 00 0F 29 74
    constexpr int kViewModel_CalcViewModelLag = 253;
    constexpr int kViewModel_CalcViewModelView = 254;

    using tCalcViewModelLag = void (*)(void *thisptr, Vector &origin, QAngle &angles, QAngle &originalAngles);
    tCalcViewModelLag oCalcViewModelLag = nullptr;
    using tCalcViewModelView = void (*)(void *thisptr, void *owner, const Vector &eyePosition, const QAngle &eyeAngles);
    tCalcViewModelView oCalcViewModelView = nullptr;

    // C_Portal_Player [379] ApplyPortalTeleportation(matrix, time, undo, forcedDuck): every portal
    // passage, called by [380]-[382]; a retracted prediction calls it again with undo.
    // [379] rva 0x0d6be0: 48 8B C4 55 57 48 8D A8 B8 FE FF FF 48 81 EC 38
    constexpr int kPlayer_ApplyPortalTeleportation = 379;

    using tApplyPortalTeleportation = void (*)(void *thisptr, const float (*matrix)[4], float time, bool undo, bool forcedDuck);
    tApplyPortalTeleportation oApplyPortalTeleportation = nullptr;

    // The player passes a portal before the view does: falling into a floor portal, the eyes follow
    // up to 8 frames later (measured). Until then the passage waits here. Without a view jump, it
    // is applied after this many frames, so that the player never stays turned the wrong way.
    constexpr int kPortalPassage_MaxFrames = 180;

    // C_Prop_Portal fields; forward of the angles is the portal's normal. The matrix isn't a network
    // field; those below are looked up in FindNetworkFields and keep these values if that fails.
    constexpr int kPortal_MatrixToLinked = 0x3188;
    int g_Portal_Origin = 0x33e8;
    int g_Portal_Angles = 0x3418;
    int g_Portal_LinkedPortal = 0x3448;
    int g_Portal_Activated = 0x5d80;
    int g_Portal_HalfWidth = 0x5d84;
    int g_Portal_HalfHeight = 0x5d88;
    void **g_PropPortalVTable = nullptr;

    // C_Portal_Player: the view is on the other side of a portal than the player, through the portal
    // named here. Set by [387] for its own eye; the player and its copies in portals are drawn by it.
    constexpr int kPlayer_ViewThroughPortal = 0x3aaa;
    constexpr int kPlayer_ViewExitPortal = 0x3ab0;
    void **g_PortalPlayerVTable = nullptr;

    struct PortalPassage
    {
        bool waiting = false;
        int frames = 0;
        float matrix[3][4] = {};
    } g_PortalPassage;

    // Roomscale: walking in the room moves the head away from the player. Commands then walk the
    // player after the head, so collisions, stairs and portals work as for any walking; the room
    // moves along, so the view doesn't move with the player. Distances in game units.
    int g_Player_Flags = 0x150; // network field m_fFlags
    constexpr int kPlayerFlag_OnGround = 1;
    constexpr float kRoomscale_StartDistance = 6; // about 14 cm
    constexpr float kRoomscale_StopDistance = 2;
    constexpr float kRoomscale_SpeedPerDistance = 8; // per second
    constexpr float kRoomscale_MinSpeed = 50; // the ground's friction stops the player below about 40
    constexpr float kRoomscale_StickDeadZone = MAX_LINEAR_SPEED * 0.1f;
    constexpr float kRoomscale_CoastSeconds = 0.3f; // the player slows down after the last command
    constexpr float kRoomscale_MaxStep = 16; // in a frame; more isn't from walking after the head
    constexpr float kRoomscale_WallMargin = 8; // the eyes' near plane stays in front of walls
    constexpr float kRoomscale_PullBackShare = 0.1f; // of the player's movement, estimated

    struct RoomscaleState
    {
        bool hasPosition = false; // false after a menu
        Vector position;
        bool following = false;
        Vector direction;
        std::chrono::steady_clock::time_point lastCommand;
        bool updatedThisFrame = false;
        bool atWall = false;
        bool walking = false; // with the stick or keyboard
        bool pullingBack = false;
        bool passedPortal = false; // for the log
    } g_Roomscale;

    // C_WeaponPortalgun [332] AddViewmodelBob(viewModel, origin, angles), called by
    // CalcViewModelView; [333] is CalcViewmodelBob.
    // [332] rva 0x3d65e0: 48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D
    constexpr int kPortalgun_AddViewmodelBob = 332;

    using tAddViewmodelBob = void (*)(void *thisptr, void *viewModel, Vector &origin, QAngle &angles);
    tAddViewmodelBob oAddViewmodelBob = nullptr;

    // ParticleMgr()->Simulate(frameTime) in OnRenderStart, called this far after loading its profiler
    // name: moves the particles to their control points, later changes show a frame late.
    // rva 0x2da2a0: 48 83 B9 88 00 00 00 00 C7 05 ?? ?? ?? ?? 00 00
    constexpr const char *kParticleSimulate_Name = "ParticleMgr()->Simulate";
    constexpr int kParticleSimulate_CallOffset = 0x21;

    using tParticleSimulate = void (*)(void *thisptr, float frameTime);
    tParticleSimulate oParticleSimulate = nullptr;

    // Muzzle as the gun reads it: C_Portal_Player [281] GetViewModel(index), then on its renderable
    // (+8) [32] LookupAttachment(name), [34] GetAttachment(number, origin, angles).
    // [281] rva 0x227e40: 48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48
    // C_BaseAnimating +8 (C_BaseViewModel's entries jump there):
    // [32] rva 0x1f0520: 48 89 5C 24 08 57 48 83 EC 20 48 8D 59 F8 48 8B
    // [34] rva 0x1ec690: 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57
    constexpr int kPlayer_GetViewModel = 281;
    constexpr int kEntity_RenderableOffset = 8;
    constexpr int kRenderable_LookupAttachment = 32;
    constexpr int kRenderable_GetAttachment = 34;

    // Callback of the effect "PortalBlast" (the shot), CEffectData: +0 start (from the eyes), +0x58
    // who fired it.
    // rva 0x0e0e50: 48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83
    constexpr int kPortalBlast_PlacedByOffset = 0x58;
    constexpr int PORTAL_PLACED_BY_PLAYER = 2;

    using tPortalBlast = void (*)(void *effectData);
    tPortalBlast oPortalBlast = nullptr;

    // TraceFirePortal(start, direction, portal2, placedBy, result, test), client and server: where
    // the portal lands. Only FirePortal calls it, this far from its `lea rcx, "PortalBlast"`.
    // rva client 0x3da380, server 0x0a97f0.
    constexpr int kTraceFirePortal_CallOffsetClient = -0x1E5;
    constexpr int kTraceFirePortal_CallOffsetServer = -0x1C5;
    constexpr uint8_t kTraceFirePortal_Start[] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x48, 0x20, 0x55 };

    using tTraceFirePortal = bool (*)(void *thisptr, const Vector &start, const Vector &direction, bool portal2, int placedBy, void *result, bool test);
    tTraceFirePortal oTraceFirePortalClient = nullptr;
    tTraceFirePortal oTraceFirePortalServer = nullptr;

    // DispatchParticleEffect, called this far after `lea rcx, "portalgun_muzzleflash"` in the gun's
    // attack. rva 0x1b0220: 48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55
    constexpr int kMuzzleFlash_CallOffset = 0x14;
    constexpr const char *kMuzzleFlash_WorldEffect = "portalgun_muzzleflash";

    using tDispatchParticleEffect = void *(*)(const char *name, int attachType, void *entity, const char *attachment, bool reset, int slot, void *filter);
    tDispatchParticleEffect oDispatchParticleEffect = nullptr;

    // C_BaseViewModel [243] calls the global FormatViewModelAttachment(player, origin, inverse) here.
    // rva 0x22e530: 48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57
    constexpr int kViewModel_FormatAttachment = 243;
    constexpr int kFormatAttachment_CallOffset = 0x5c;

    using tFormatViewModelAttachment = void (*)(void *player, Vector &origin, bool inverse);
    tFormatViewModelAttachment oFormatViewModelAttachment = nullptr;

    // Ping laser as aim laser: CreatePingPointer(player, destination) starts robot_point_beam at the
    // muzzle (control point 1 end, 2 color). Found by its warning; rva 0x3e6700.
    constexpr const char *kPingPointer_Warning = "Trying to create a ping laser, but we have no view model!";
    constexpr int kPingPointer_WarningOffset = 0x39;
    constexpr uint8_t kPingPointer_Start[] = { 0x40, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50 };
    constexpr int kPingPointer_DestroyCallOffset = 0x10;
    constexpr int kPingPointer_EffectLeaOffset = 0x15; // lea rsi, [r14 + effect]
    constexpr uint8_t kPingPointer_EffectLea[] = { 0x49, 0x8D, 0xB6 };
    constexpr int kPingPointer_SetDrawOnlyCallOffset = 0x279;
    constexpr int kPingPointer_SetControlPointCallOffset = 0x289;

    // The server precaches the effect only in coop: PrecacheParticleSystem(name), called after
    // loading the name, works any time; the client draws it after its next update.
    constexpr const char *kLaserEffect = "robot_point_beam";
    constexpr int kPrecacheParticle_CallOffset = 7;
    constexpr int kLaser_PrecacheWaitFrames = 30;
    int g_LaserPrecacheFrames = 0; // since the precache; starts over without a gun or in the menu
    constexpr int kSplitScreen_AllUsers = -1;

    using tCreatePingPointer = void (*)(void *player, const Vector &destination);
    using tDestroyPingPointer = void (*)(void *player);
    using tSetControlPoint = void (*)(void *effect, int point, const Vector &value);
    using tSetDrawOnlyForSplitScreenUser = void (*)(void *effect, int slot);
    using tPrecacheParticleSystem = int (*)(const char *name);
    tCreatePingPointer CreatePingPointer = nullptr;
    tDestroyPingPointer DestroyPingPointer = nullptr;
    tSetControlPoint SetControlPoint = nullptr;
    tSetDrawOnlyForSplitScreenUser SetDrawOnlyForSplitScreenUser = nullptr;
    tPrecacheParticleSystem PrecacheParticleSystem = nullptr;
    int g_PingEffectOffset = 0; // of the player

    // The shot's trace filter as TraceFirePortal builds it: CTraceFilterSimpleClassnameList's
    // constructor (rva 0x1d3a10), then the classes it passes. [3] SetPassEntity is 48 89 51 08 C3.
    constexpr int kTraceFirePortal_FilterConstructorCallOffset = 0x46;
    constexpr int kTraceFirePortal_FilterListCallOffset = 0x4F;
    constexpr int kTraceFilter_SetPassEntity = 3;
    constexpr size_t kTraceFilter_Size = 0x40;

    using tTraceFilterConstructor = void (*)(void *filter, void *passEntity, int collisionGroup);
    using tTraceFilterList = void (*)(void *filter);
    tTraceFilterConstructor TraceFilterConstructor = nullptr;
    tTraceFilterList TraceFilterList = nullptr;

    // Ray_t of Strata Source (TraceFirePortal, engine's TraceRay): the SDK's plus the fields at the end.
    struct alignas(16) AlignedVector
    {
        Vector value;
        float pad;
    };
    struct StrataRay
    {
        AlignedVector start, delta, startOffset, extents;
        const void *worldAxisTransform;
        bool isRay, isSwept, unknown4A;
        const void *unknown50;
        float unknown58[3];
    };
    static_assert(offsetof(StrataRay, isRay) == 0x48 && offsetof(StrataRay, unknown58) == 0x58);

    // EngineTraceClient004 is CEngineTraceClient (engine.dll): [5] TraceRay(ray, mask, filter, trace),
    // rva 0x1d1c80: 48 89 5C 24 10 44 89 44 24 18 48 89 4C 24 08 55
    constexpr int kEngineTrace_TraceRay = 5;
    constexpr unsigned int kShotMask = 0x2004003; // of TraceFirePortal
    constexpr int kTrace_EndPosOffset = 0x0C;
    constexpr int kTrace_FractionOffset = 0x2C;
    constexpr size_t kTrace_Size = 0x100; // the game's is 0xA0

    // Network fields: player m_hActiveWeapon, C_WeaponPortalgun m_bCanFirePortal1/2, m_iLastFiredPortal,
    // m_bOpenProngs (set with the animation ACT_VM_PICKUP, which leads to the carrying one).
    int g_Player_ActiveWeaponHandle = 0x3868;
    int g_Portalgun_CanFirePortal1 = 0x3280;
    int g_Portalgun_CanFirePortal2 = 0x3281;
    int g_Portalgun_LastFiredPortal = 0x3284;
    int g_Portalgun_OpenProngs = 0x3288;
    constexpr int kAimMode_None = 0;
    constexpr int kAimMode_Dot = 1;
    constexpr int kAimMode_Laser = 2;

    void **g_PortalgunVTable = nullptr;

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

    // Where the viewmodel's muzzle is in this frame, for the laser and the shot; the server's code
    // runs outside of rendering, where client bones may not be set up.
    Vector g_Muzzle;
    bool g_MuzzleValid = false;

    Vector g_EyePosition; // of the player in the game, as last seen by hkCalcViewModelView
    Vector g_ShotStart; // where the shot starts, see UpdateShotStart
    bool g_ShotFromEyes = false; // see UpdateShotStart
    Vector g_Aim; // where the shot would land, see UpdateAim
    bool g_AimValid = false;

    // v_portalgun.mdl: bone Base, in the model's right-handed axes; radius estimated from an image.
    constexpr float kGun_GripForward = 7.04f;
    constexpr float kGun_GripRight = 8.35f;
    constexpr float kGun_GripUp = -11.35f;
    constexpr float kGun_Radius = 4.5f;
    constexpr int kGun_Samples = 8;

    // The gun as placed for this frame.
    struct GunPlacement
    {
        void *viewModel = nullptr;
        void *owner = nullptr;
        Vector position;
        QAngle angles;
    } g_Gun;

    // The eyes are rendered and the viewmodel is in the hand. During a video the menu overlay
    // shows it from the back buffer.
    bool VRViewActive()
    {
        VR *vr = g_StrataGame->m_VR;
        return vr->m_IsInitialized && !vr->IsMenuVisible() && !StrataHooks::VideoVisible();
    }

    struct Section
    {
        uint8_t *begin;
        uint8_t *end;
    };

    // The module's sections that hold code or data (RTTI lives in .rdata).
    std::vector<Section> Sections(uint8_t *base, bool code)
    {
        std::vector<Section> sections;
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) != code)
                continue;
            uint8_t *begin = base + section->VirtualAddress;
            sections.push_back({ begin, begin + section->Misc.VirtualSize });
        }
        return sections;
    }

    // The instruction `lea <register>, [rip + text]`; modrm selects the register (0x0D rcx, 0x15 rdx).
    uint8_t *FindStringLea(uint8_t *base, const char *text, uint8_t modrm)
    {
        // With the 0 before and after it, so it isn't the end of a longer string.
        const std::string needle = std::string(1, '\0') + text + std::string(1, '\0');
        uint8_t *string = nullptr;
        for (const Section &section : Sections(base, false))
        {
            uint8_t *found = std::search(section.begin, section.end, needle.begin(), needle.end());
            if (found != section.end)
            {
                string = found + 1;
                break;
            }
        }
        if (!string)
            return nullptr;

        for (const Section &section : Sections(base, true))
        {
            for (uint8_t *p = section.begin; p + 7 <= section.end; ++p)
            {
                if (p[0] == 0x48 && p[1] == 0x8D && p[2] == modrm && p + 7 + *reinterpret_cast<int32_t *>(p + 3) == string)
                    return p;
            }
        }
        return nullptr;
    }

    // The target of the `call rel32` at this address, nullptr if there is none.
    uint8_t *CallTarget(uint8_t *call)
    {
        return *call == 0xE8 ? call + 5 + *reinterpret_cast<int32_t *>(call + 1) : nullptr;
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

    template <typename F>
    bool InstallHook(const char *name, void *target, F detour, F &original, const char *module = "client.dll")
    {
        const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void *>(detour), reinterpret_cast<void **>(&original));
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

    uint8_t *FindTraceFirePortal(const char *module, int callOffset)
    {
        uint8_t *blast = FindStringLea(reinterpret_cast<uint8_t *>(GetModuleHandleA(module)), "PortalBlast", 0x0D);
        uint8_t *target = blast ? CallTarget(blast + callOffset) : nullptr;
        return target && memcmp(target, kTraceFirePortal_Start, sizeof(kTraceFirePortal_Start)) == 0 ? target : nullptr;
    }

    // Movement (forward, side) given relative to fromYaw, expressed relative to toYaw. Source:
    // forward = (cos yaw, sin yaw), right = (sin yaw, -cos yaw).
    void TurnMove(float forward, float side, float fromYaw, float toYaw, float &outForward, float &outSide)
    {
        const float d = (fromYaw - toYaw) * 3.14159265f / 180.0f;
        outForward += forward * cosf(d) + side * sinf(d);
        outSide += -forward * sinf(d) + side * cosf(d);
    }

    bool RoomscaleEnabled()
    {
        const VR *vr = g_StrataGame->m_VR;
        return vr->m_Roomscale && vr->m_6DOF;
    }

    bool PlayerOnGround()
    {
        Game *game = g_StrataGame;
        auto player = reinterpret_cast<const uint8_t *>(game->GetClientEntity(game->m_EngineClient->GetLocalPlayer()));
        return player && (*reinterpret_cast<const int *>(player + g_Player_Flags) & kPlayerFlag_OnGround);
    }

    // Walks the player after the head, unless the player already walks with the stick or keyboard:
    // then the head is pulled back to the player instead, see PullHeadBack.
    void RoomscaleMove(float aimYaw, float &forward, float &side)
    {
        VR *vr = g_StrataGame->m_VR;
        RoomscaleState &state = g_Roomscale;

        Vector offset = vr->m_HmdPosRelative;
        offset.z = 0;
        const float distance = VectorLength(offset);
        const bool walking = std::sqrt(forward * forward + side * side) >= kRoomscale_StickDeadZone;
        const bool follow = RoomscaleEnabled() && state.hasPosition && !walking && PlayerOnGround()
            && distance >= (state.following ? kRoomscale_StopDistance : kRoomscale_StartDistance);
        if (follow != state.following)
            std::cout << "[VR] roomscale: " << (follow ? "following" : "stopped") << ", head " << distance << " away\n";
        state.following = follow;
        state.walking = walking;
        // From now on the view has to go along with the player, not stay where the head is.
        if (walking)
            state.lastCommand = {};
        if (!follow)
            return;

        state.direction = offset * (1.0f / distance);
        state.lastCommand = std::chrono::steady_clock::now();
        const float speed = std::clamp(distance * kRoomscale_SpeedPerDistance, kRoomscale_MinSpeed, (float)MAX_LINEAR_SPEED);
        TurnMove(speed, 0, RAD2DEG(std::atan2(offset.y, offset.x)), aimYaw, forward, side);
    }

    // The player aims with the right controller (view angles of the command: shooting, use). The
    // left stick walks where the left controller points, the keyboard relative to the head. The
    // engine's view angles stay with the head (set in hkRenderView).
    bool hkCreateMove(void *thisptr, float flInputSampleTime, CUserCmd *cmd)
    {
        const bool result = oCreateMove(thisptr, flInputSampleTime, cmd);

        VR *vr = g_StrataGame->m_VR;
        if (!cmd || !cmd->command_number || !VRViewActive())
            return result;

        QAngle aim = vr->m_RightControllerAngAbs;
        aim.x = std::clamp(aim.x, -89.0f, 89.0f);
        aim.z = 0;

        float forward = 0, side = 0;
        // Keyboard movement is relative to the engine's view angles, i.e. the head.
        TurnMove(cmd->forwardmove, cmd->sidemove, vr->m_HmdAngAbs.y, aim.y, forward, side);
        vr::InputAnalogActionData_t walk;
        if (vr->GetAnalogActionData(vr->m_ActionWalk, walk) && walk.bActive)
            TurnMove(walk.y * MAX_LINEAR_SPEED, walk.x * MAX_LINEAR_SPEED, vr->WalkHandYaw(), aim.y, forward, side);
        RoomscaleMove(aim.y, forward, side);
        cmd->forwardmove = forward;
        cmd->sidemove = side;
        cmd->upmove = 0;

        cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
        if (cmd->forwardmove > 0)
            cmd->buttons |= IN_FORWARD;
        else if (cmd->forwardmove < 0)
            cmd->buttons |= IN_BACK;
        if (cmd->sidemove > 0)
            cmd->buttons |= IN_MOVERIGHT;
        else if (cmd->sidemove < 0)
            cmd->buttons |= IN_MOVELEFT;

        cmd->viewangles = aim;

        static bool logged = false;
        if (!logged)
        {
            std::cout << "[VR] first VR command #" << cmd->command_number << ": aim " << aim.x << "," << aim.y << " head yaw "
                      << vr->m_HmdAngAbs.y << ", move " << cmd->forwardmove << "," << cmd->sidemove << "\n";
            logged = true;
        }

        // false: the engine keeps its view angles (the head) instead of taking the aim.
        return false;
    }

    Vector RotatedByPortal(const float (*m)[4], const Vector &v)
    {
        return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
                 m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                 m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
    }

    Vector MovedByPortal(const float (*m)[4], const Vector &v)
    {
        const Vector rotated = RotatedByPortal(m, v);
        return { rotated.x + m[0][3], rotated.y + m[1][3], rotated.z + m[2][3] };
    }

    // The angles of a pose seen through the portal.
    QAngle TurnedByPortal(const float (*m)[4], const QAngle &angles)
    {
        Vector forward, right, up;
        QAngle::AngleVectors(angles, &forward, &right, &up);
        QAngle turned;
        QAngle::VectorAngles(RotatedByPortal(m, forward), RotatedByPortal(m, up), turned);
        return turned;
    }

    // The heading of a head pose, also when it looks straight up or down: the top of the head then
    // points along the heading (looking down) or against it (looking up).
    float Heading(const Vector &forward, const Vector &up)
    {
        const float flat = std::sqrt(forward.x * forward.x + forward.y * forward.y);
        return RAD2DEG(std::atan2(forward.y * flat - up.y * forward.z, forward.x * flat - up.x * forward.z));
    }

    // The game turns its view angles with the portal, but those are overwritten with the head's
    // each frame; so the head's offset has to turn instead.
    void TurnWithPortal(const float (*matrix)[4])
    {
        VR *vr = g_StrataGame->m_VR;

        const Vector forward = RotatedByPortal(matrix, vr->m_HmdForward);
        const Vector up = RotatedByPortal(matrix, vr->m_HmdUp);
        const float yaw = Heading(forward, up) - Heading(vr->m_HmdForward, vr->m_HmdUp);
        vr->m_RotationOffset.y += yaw;
        vr->m_RotationOffset.y -= 360 * std::floor(vr->m_RotationOffset.y / 360);

        if (vr->m_ApplyPitchAndRollPortalRotationOffset)
        {
            QAngle before, after;
            QAngle::VectorAngles(vr->m_HmdForward, vr->m_HmdUp, before);
            QAngle::VectorAngles(forward, up, after);
            QAngle turn(after.x - before.x, 0, after.z - before.z);
            turn.Normalize();
            vr->m_RotationOffset.x += turn.x;
            vr->m_RotationOffset.z += turn.z;
        }

        // At once, not with the next tracking update: the gun and the eyes are placed in this frame.
        vr->UpdateHeadPose();
        vr->UpdateHandTracking();

        // Walking after the head goes on behind the portal, the view must not move in this frame either.
        g_Roomscale.position = MovedByPortal(matrix, g_Roomscale.position);
        g_Roomscale.direction = RotatedByPortal(matrix, g_Roomscale.direction);
        g_Roomscale.passedPortal = true;

        Vector offset = vr->m_HmdPosRelative;
        offset.z = 0;
        std::cout << "[VR] portal passage: yaw " << yaw << " after " << g_PortalPassage.frames << " frames, head "
            << VectorLength(offset) << " away\n";
    }

    void hkApplyPortalTeleportation(void *thisptr, const float (*matrix)[4], float time, bool undo, bool forcedDuck)
    {
        oApplyPortalTeleportation(thisptr, matrix, time, undo, forcedDuck);

        Game *game = g_StrataGame;
        if (!VRViewActive() || thisptr != game->GetClientEntity(game->m_EngineClient->GetLocalPlayer()))
            return;

        if (g_PortalPassage.waiting)
        {
            // Taken back before the view has followed: nothing to turn.
            if (undo)
            {
                std::cout << "[VR] portal passage taken back after " << g_PortalPassage.frames << " frames\n";
                g_PortalPassage.waiting = false;
                return;
            }
            TurnWithPortal(g_PortalPassage.matrix);
        }

        memcpy(g_PortalPassage.matrix, matrix, sizeof(g_PortalPassage.matrix));
        g_PortalPassage.frames = 0;
        g_PortalPassage.waiting = true;
    }

    // Turns with a waiting passage as soon as the view has jumped through the portal, i.e. it's
    // closer to where the portal takes the last position than to that position itself.
    void FollowPortalPassage(const Vector &position, const Vector &lastPosition)
    {
        if (!g_PortalPassage.waiting)
            return;

        const float (*matrix)[4] = g_PortalPassage.matrix;
        const Vector behindPortal = MovedByPortal(matrix, lastPosition);

        const auto squaredDistance = [&position](const Vector &to)
        {
            const Vector d = position - to;
            return d.x * d.x + d.y * d.y + d.z * d.z;
        };
        const bool jumped = squaredDistance(behindPortal) < squaredDistance(lastPosition);
        if (jumped || g_PortalPassage.frames >= kPortalPassage_MaxFrames)
        {
            TurnWithPortal(matrix);
            g_PortalPassage.waiting = false;
        }
    }

    // The open portal whose hole the line passes from the front, nullptr if there is none.
    const uint8_t *FindPortalBetween(const Vector &from, const Vector &to)
    {
        const auto dot = [](const Vector &a, const Vector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        Game *game = g_StrataGame;
        const int highest = game->m_ClientEntityList->GetHighestEntityIndex();
        for (int i = 1; i <= highest; ++i)
        {
            auto portal = reinterpret_cast<const uint8_t *>(game->GetClientEntity(i));
            if (!portal || *reinterpret_cast<void **const *>(portal) != g_PropPortalVTable
                || !portal[g_Portal_Activated] || *reinterpret_cast<const int *>(portal + g_Portal_LinkedPortal) == -1)
                continue;

            const Vector origin = *reinterpret_cast<const Vector *>(portal + g_Portal_Origin);
            Vector forward, right, up;
            QAngle::AngleVectors(*reinterpret_cast<const QAngle *>(portal + g_Portal_Angles), &forward, &right, &up);
            const float fromDistance = dot(from - origin, forward);
            const float toDistance = dot(to - origin, forward);
            if (fromDistance <= 0 || toDistance >= 0)
                continue;

            const float along = fromDistance / (fromDistance - toDistance);
            const Vector delta = to - from;
            const Vector hit = Vector(from.x + delta.x * along, from.y + delta.y * along, from.z + delta.z * along) - origin;
            if (std::abs(dot(hit, right)) <= *reinterpret_cast<const float *>(portal + g_Portal_HalfWidth)
                && std::abs(dot(hit, up)) <= *reinterpret_cast<const float *>(portal + g_Portal_HalfHeight))
                return portal;
        }
        return nullptr;
    }

    void UpdateRoomscale(const Vector &eyes);

    // The viewmodel (portal gun) sits in the right hand instead of in front of the head.
    void hkCalcViewModelView(void *thisptr, void *owner, const Vector &eyePosition, const QAngle &eyeAngles)
    {
        g_ViewModelOwner = owner;
        g_EyePosition = eyePosition;
        if (!VRViewActive())
            return oCalcViewModelView(thisptr, owner, eyePosition, eyeAngles);

        VR *vr = g_StrataGame->m_VR;
        FollowPortalPassage(eyePosition, vr->m_SetupOrigin);
        UpdateRoomscale(eyePosition);
        const Vector position = vr->GetRecommendedViewmodelAbsPos(eyePosition);
        const QAngle angles = vr->GetRecommendedViewmodelAbsAngle();
        oCalcViewModelView(thisptr, owner, position, angles);
        if (thisptr == CallVirtual<void *>(owner, kPlayer_GetViewModel, 0))
        {
            g_Gun.viewModel = thisptr;
            g_Gun.owner = owner;
            g_Gun.position = position;
            g_Gun.angles = angles;
        }
    }

    // The engine moves viewmodel attachments (muzzle, gun lights) to where they appear on screen,
    // as the viewmodel is drawn with a different FOV than the world. It uses the flat view for
    // that, while the eyes draw both with the same FOV: there the attachments are right as they are.
    void hkFormatViewModelAttachment(void *player, Vector &origin, bool inverse)
    {
        if (!VRViewActive())
            oFormatViewModelAttachment(player, origin, inverse);
    }

    // The game's lag lets the gun drift after the hand for about a second.
    void hkCalcViewModelLag(void *thisptr, Vector &origin, QAngle &angles, QAngle &originalAngles)
    {
        if (!VRViewActive())
            oCalcViewModelLag(thisptr, origin, angles, originalAngles);
    }

    // The gun stays in line with the controller and with the laser.
    void hkAddViewmodelBob(void *thisptr, void *viewModel, Vector &origin, QAngle &angles)
    {
        if (!VRViewActive())
            oAddViewmodelBob(thisptr, viewModel, origin, angles);
    }

    // Where the viewmodel's muzzle is now. Only with the player hkCalcViewModelView has seen, as
    // GetClientEntity's result isn't verified to be the entity pointer on Strata Source.
    bool GetMuzzle(Vector &origin)
    {
        Game *game = g_StrataGame;
        void *player = game->GetClientEntity(game->m_EngineClient->GetLocalPlayer());
        if (!player || player != g_ViewModelOwner)
            return false;
        auto viewModel = CallVirtual<uint8_t *>(player, kPlayer_GetViewModel, 0);
        if (!viewModel)
            return false;
        const void *renderable = viewModel + kEntity_RenderableOffset;
        const int attachment = CallVirtual<int>(renderable, kRenderable_LookupAttachment, "muzzle");
        QAngle angles;
        return attachment > 0 && CallVirtual<bool>(renderable, kRenderable_GetAttachment, attachment, &origin, &angles);
    }

    // The shot starts at the gun's muzzle instead of next to the head.
    void hkPortalBlast(void *effectData)
    {
        auto placedBy = *reinterpret_cast<int *>(static_cast<uint8_t *>(effectData) + kPortalBlast_PlacedByOffset);
        if (VRViewActive() && placedBy == PORTAL_PLACED_BY_PLAYER)
        {
            if (g_MuzzleValid)
                *static_cast<Vector *>(effectData) = g_Muzzle;

            static bool logged = false;
            if (!logged)
            {
                std::cout << "[VR] first portal shot: muzzle " << (g_MuzzleValid ? "found" : "not found, the shot starts at the game's position") << "\n";
                logged = true;
            }
        }
        oPortalBlast(effectData);
    }

    // The player's shot goes from the muzzle along the controller instead of from the head, so it
    // stays in line with the gun. Client (prediction) and server have to agree on this.
    bool TraceFromGun(tTraceFirePortal original, void *thisptr, const Vector &start, const Vector &direction, bool portal2, int placedBy, void *result, bool test)
    {
        if (!VRViewActive() || placedBy != PORTAL_PLACED_BY_PLAYER)
            return original(thisptr, start, direction, portal2, placedBy, result, test);

        VR *vr = g_StrataGame->m_VR;
        const Vector gunStart = g_MuzzleValid ? g_ShotStart : vr->GetRightControllerAbsPos();
        return original(thisptr, gunStart, vr->m_RightControllerForward, portal2, placedBy, result, test);
    }

    bool hkTraceFirePortalClient(void *thisptr, const Vector &start, const Vector &direction, bool portal2, int placedBy, void *result, bool test)
    {
        return TraceFromGun(oTraceFirePortalClient, thisptr, start, direction, portal2, placedBy, result, test);
    }

    bool hkTraceFirePortalServer(void *thisptr, const Vector &start, const Vector &direction, bool portal2, int placedBy, void *result, bool test)
    {
        return TraceFromGun(oTraceFirePortalServer, thisptr, start, direction, portal2, placedBy, result, test);
    }

    // The muzzle flash on the weapon presumably sits at the hidden third-person model, near the
    // head; the first-person one on the viewmodel stays.
    void *hkDispatchParticleEffect(const char *name, int attachType, void *entity, const char *attachment, bool reset, int slot, void *filter)
    {
        if (VRViewActive() && strcmp(name, kMuzzleFlash_WorldEffect) == 0)
            return nullptr;
        return oDispatchParticleEffect(name, attachType, entity, attachment, reset, slot, filter);
    }

    // The portal gun the player holds, nullptr if it's something else or the gun can't fire.
    uint8_t *ActivePortalgun(uint8_t *player)
    {
        const uint32_t handle = *reinterpret_cast<uint32_t *>(player + g_Player_ActiveWeaponHandle);
        if (handle == 0xFFFFFFFF)
            return nullptr;
        auto weapon = reinterpret_cast<uint8_t *>(g_StrataGame->GetClientEntity(handle & 0xFFFF));
        if (!weapon || *reinterpret_cast<void ***>(weapon) != g_PortalgunVTable)
            return nullptr;
        return weapon[g_Portalgun_CanFirePortal1] || weapon[g_Portalgun_CanFirePortal2] ? weapon : nullptr;
    }

    struct ShotTrace
    {
        Vector end;
        float fraction;
    };

    // How far a shot from the gun (or player) gets along delta, with the same filter and mask as the shot.
    ShotTrace TraceShot(void *gun, const Vector &start, const Vector &delta)
    {
        static uint8_t *filter = []
        {
            alignas(16) static uint8_t buffer[kTraceFilter_Size];
            TraceFilterConstructor(buffer, nullptr, 0);
            TraceFilterList(buffer);
            return buffer;
        }();
        CallVirtual<void>(filter, kTraceFilter_SetPassEntity, gun);

        // Vector's constructor doesn't initialize it, so "= {}" wouldn't clear the ray.
        StrataRay ray;
        memset(&ray, 0, sizeof(ray));
        ray.start.value = start;
        ray.delta.value = delta;
        ray.isRay = true;
        ray.isSwept = true;

        alignas(16) uint8_t trace[kTrace_Size];
        CallVirtual<void>(g_StrataGame->m_EngineTrace, kEngineTrace_TraceRay, &ray, kShotMask, filter, trace);
        return { *reinterpret_cast<Vector *>(trace + kTrace_EndPosOffset), *reinterpret_cast<float *>(trace + kTrace_FractionOffset) };
    }

    // Walking on when the player can't go further must not take the head into the wall. Checked at
    // the height of the player's eyes: that's in free space, the head's height may not be.
    void KeepHeadOutOfWalls(const Vector &eyes)
    {
        Game *game = g_StrataGame;
        VR *vr = game->m_VR;
        void *player = game->GetClientEntity(game->m_EngineClient->GetLocalPlayer());
        if (!player || !TraceFilterConstructor || !TraceFilterList || !game->m_EngineTrace)
            return;

        Vector offset = vr->m_HmdPosRelative;
        offset.z = 0;
        const float distance = VectorLength(offset);
        if (distance < 0.01f)
            return;

        // Through an open portal the eyes look from the other side, see hkRenderView.
        const Vector ray = offset * ((distance + kRoomscale_WallMargin) / distance);
        const float fraction = g_PropPortalVTable && FindPortalBetween(eyes, eyes + ray) ? 1.0f : TraceShot(player, eyes, ray).fraction;
        const bool atWall = fraction > 0 && fraction < 1;
        if (atWall != g_Roomscale.atWall)
            std::cout << "[VR] roomscale: head " << (atWall ? "at a wall" : "free") << "\n";
        g_Roomscale.atWall = atWall;
        if (!atWall)
            return;

        const float allowed = (std::max)(0.0f, fraction * (distance + kRoomscale_WallMargin) - kRoomscale_WallMargin);
        if (allowed < distance)
            vr->MoveRoom(offset * (1.0f - allowed / distance));
    }

    // With the stick or in the air the player doesn't follow the head. The head returns to the player
    // then, so that the player is where the view suggests, e.g. when aiming for a portal in a jump.
    // In proportion to the player's movement (moved): it starts as softly as the player does, and
    // the view doesn't move on its own while the player stands, e.g. against a wall.
    void PullHeadBack(const Vector &moved)
    {
        RoomscaleState &state = g_Roomscale;
        VR *vr = g_StrataGame->m_VR;

        Vector offset = vr->m_HmdPosRelative;
        offset.z = 0;
        const float distance = VectorLength(offset);
        const bool pull = (state.walking || !PlayerOnGround()) && !g_PortalPassage.waiting;
        if (pull != state.pullingBack)
            std::cout << "[VR] roomscale: pull back " << (pull ? "started" : "ended") << ", head " << distance << " away\n";
        state.pullingBack = pull;
        const float movedDistance = VectorLength(moved);
        if (pull && distance >= 0.01f && movedDistance <= kRoomscale_MaxStep)
            vr->MoveRoom(offset * (std::min)(1.0f, kRoomscale_PullBackShare * movedDistance / distance));
    }

    // Once per frame, before anything is placed relative to the head.
    void UpdateRoomscale(const Vector &eyes)
    {
        RoomscaleState &state = g_Roomscale;
        if (!RoomscaleEnabled())
        {
            state.hasPosition = false;
            return;
        }
        state.updatedThisFrame = true;

        VR *vr = g_StrataGame->m_VR;
        const Vector last = state.position;
        state.position = eyes;

        // Back from a menu, where no world can be seen: don't walk the player to where the head went.
        // Only sideways: the eye height stays as SeatedMode defines it.
        if (!state.hasPosition)
        {
            state.hasPosition = true;
            Vector offset = vr->m_HmdPosRelative;
            offset.z = 0;
            vr->MoveRoom(offset);
            std::cout << "[VR] roomscale: head centered on the player\n";
            return;
        }

        Vector step = eyes - last;
        step.z = 0;
        const float sinceCommand = std::chrono::duration<float>(std::chrono::steady_clock::now() - state.lastCommand).count();
        // Against the commanded direction or fast, something else moves the player (platform, push)
        // and the view has to go along.
        const bool roomMoves = sinceCommand <= kRoomscale_CoastSeconds && VectorLength(step) <= kRoomscale_MaxStep
            && DotProduct(step, state.direction) > 0 && PlayerOnGround();
        if (state.passedPortal)
            std::cout << "[VR] roomscale: step at the portal " << VectorLength(step) << ", room " << (roomMoves ? "moves along" : "stays") << "\n";
        state.passedPortal = false;
        if (roomMoves)
            vr->MoveRoom(step);

        PullHeadBack(eyes - last);
        KeepHeadOutOfWalls(eyes);
    }

    // What counts for the vignette is the walking one sees: none against a wall, none from walking in
    // the room, and not in the air. The jump through a portal isn't walking either.
    float WalkedDistance(const Vector &position, const Vector &last)
    {
        Vector step = position - last;
        step.z = 0;
        const float distance = VectorLength(step);
        return g_Roomscale.walking && PlayerOnGround() && distance <= kRoomscale_MaxStep ? distance : 0;
    }

    // The hand can reach through a wall. If something is between the player's eyes in the game and
    // the muzzle, the shot starts at the eyes, so that it can't land behind the wall. The laser is
    // off then, as it would start at the muzzle and not show where the shot goes.
    void UpdateShotStart()
    {
        g_ShotStart = g_Muzzle;
        uint8_t *gun = g_MuzzleValid && CreatePingPointer ? ActivePortalgun(static_cast<uint8_t *>(g_ViewModelOwner)) : nullptr;
        const bool blocked = gun && TraceShot(gun, g_EyePosition, g_Muzzle - g_EyePosition).fraction < 1.0f;
        if (blocked)
            g_ShotStart = g_EyePosition;
        g_ShotFromEyes = blocked;

        static bool wasBlocked = false;
        if (blocked != wasBlocked)
            std::cout << "[VR] shot starts at the " << (blocked ? "eyes" : "muzzle") << "\n";
        wasBlocked = blocked;
    }

    // No aim help while carrying an object: no portals can be placed then.
    void UpdateAim()
    {
        VR *vr = g_StrataGame->m_VR;
        uint8_t *gun = g_MuzzleValid && CreatePingPointer && vr->m_AimMode != kAimMode_None && !vr->m_ViewmodelCarrying ? ActivePortalgun(static_cast<uint8_t *>(g_ViewModelOwner)) : nullptr;
        g_AimValid = gun != nullptr;
        if (gun)
            g_Aim = TraceShot(gun, g_ShotStart, vr->m_RightControllerForward * static_cast<float>(MAX_TRACE_LENGTH)).end;
    }

    void RemoveLaser()
    {
        if (!CreatePingPointer)
            return;
        Game *game = g_StrataGame;
        auto player = reinterpret_cast<uint8_t *>(game->GetClientEntity(game->m_EngineClient->GetLocalPlayer()));
        if (player && player == g_ViewModelOwner && *reinterpret_cast<void **>(player + g_PingEffectOffset))
            DestroyPingPointer(player);
    }

    // The aim laser runs from the muzzle to where the shot would land, in the color of the last portal.
    void UpdateLaser()
    {
        VR *vr = g_StrataGame->m_VR;
        if (!CreatePingPointer)
            return;

        auto player = static_cast<uint8_t *>(g_ViewModelOwner);
        uint8_t *gun = g_MuzzleValid && vr->m_AimMode == kAimMode_Laser ? ActivePortalgun(player) : nullptr;
        if (!gun)
        {
            g_LaserPrecacheFrames = 0;
            return RemoveLaser();
        }
        if (g_ShotFromEyes || vr->m_ViewmodelCarrying)
            return RemoveLaser();

        const Vector aim = g_Aim;
        void *effect = *reinterpret_cast<void **>(player + g_PingEffectOffset);
        if (!effect)
        {
            if (g_LaserPrecacheFrames == 0)
                PrecacheParticleSystem(kLaserEffect);
            if (g_LaserPrecacheFrames < kLaser_PrecacheWaitFrames)
            {
                ++g_LaserPrecacheFrames;
                return;
            }

            CreatePingPointer(player, aim);
            effect = *reinterpret_cast<void **>(player + g_PingEffectOffset);
            std::cout << "[VR] laser: " << (effect ? "created" : "not created") << ", muzzle " << g_Muzzle.x << "," << g_Muzzle.y << "," << g_Muzzle.z
                      << " aim " << aim.x << "," << aim.y << "," << aim.z << "\n";
            if (!effect)
                return;
            SetDrawOnlyForSplitScreenUser(effect, kSplitScreen_AllUsers);
        }
        SetControlPoint(effect, 1, aim);
        const int lastPortal = std::clamp(*reinterpret_cast<int *>(gun + g_Portalgun_LastFiredPortal), 0, 2);
        SetControlPoint(effect, 2, g_StrataGame->m_singlePlayerPortalColors[lastPortal] * 0.5f);
    }

    // CalcViewModelView has already put the viewmodel in the hand for this frame.
    void hkParticleSimulate(void *thisptr, float frameTime)
    {
        if (VRViewActive())
        {
            g_MuzzleValid = GetMuzzle(g_Muzzle);

            VR *vr = g_StrataGame->m_VR;
            const uint8_t *gun = g_MuzzleValid ? ActivePortalgun(static_cast<uint8_t *>(g_ViewModelOwner)) : nullptr;
            const bool carrying = gun && gun[g_Portalgun_OpenProngs];
            if (carrying != vr->m_ViewmodelCarrying)
                std::cout << "[VR] gun carrying: " << carrying << "\n";
            vr->m_ViewmodelCarrying = carrying;

            UpdateShotStart();
            UpdateAim();
            UpdateLaser();
        }
        else
            g_LaserPrecacheFrames = 0;
        oParticleSimulate(thisptr, frameTime);
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

    // Whether the gun is on the line from the eye through a point. It's drawn over the world at any
    // distance. Roughly: a cylinder from the muzzle to the grip.
    bool GunCovers(const Vector &eye, const Vector &point)
    {
        if (!g_Gun.viewModel || !g_MuzzleValid)
            return false;

        const auto dot = [](const Vector &a, const Vector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        Vector line = point - eye;
        const float length = sqrtf(dot(line, line));
        if (length < 1.0f)
            return false;
        line = line * (1.0f / length);

        Vector forward, right, up;
        QAngle::AngleVectors(g_Gun.angles, &forward, &right, &up);
        const Vector grip = g_Gun.position + forward * kGun_GripForward + right * kGun_GripRight + up * kGun_GripUp;
        for (int i = 0; i <= kGun_Samples; ++i)
        {
            const Vector along = g_Muzzle + (grip - g_Muzzle) * (static_cast<float>(i) / kGun_Samples);
            const Vector toGun = along - eye;
            const float ahead = dot(toGun, line);
            if (ahead > 0 && dot(toGun, toGun) - ahead * ahead < kGun_Radius * kGun_Radius)
                return true;
        }
        return false;
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
        { "DT_Portal_Player", "m_hActiveWeapon", &g_Player_ActiveWeaponHandle },
        { "DT_WeaponPortalgun", "m_bCanFirePortal1", &g_Portalgun_CanFirePortal1 },
        { "DT_WeaponPortalgun", "m_bCanFirePortal2", &g_Portalgun_CanFirePortal2 },
        { "DT_WeaponPortalgun", "m_iLastFiredPortal", &g_Portalgun_LastFiredPortal },
        { "DT_WeaponPortalgun", "m_bOpenProngs", &g_Portalgun_OpenProngs },
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

// Rumble: the game's gamepad rumble goes to the VR controllers. Path in client.dll: UM_Rumble ->
// RumbleEffect -> StartEffect puts the effect into one of 3 channels per user; each frame
// UpdateEffects mixes the channels and calls CInputSystem [37] SetRumble(left, right, user).
// StartEffect and UpdateEffects both ask CInput [14] (controller mode) and do nothing or zero the
// motors without a gamepad: [14] answers true for exactly these two calls. Changing the whole game
// to controller mode would show the gamepad's buttons in the hints and change ~128 other places.
namespace
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

void **StrataHooks::FindVTable(const char *module, const char *className, uint32_t offset)
{
    auto base = reinterpret_cast<uint8_t *>(GetModuleHandleA(module));
    if (!base)
        return nullptr;
    const std::vector<Section> sections = Sections(base, false);

    // RTTI type descriptor: vftable pointer, spare pointer, then the decorated name.
    const std::string name = std::string(".?AV") + className + "@@";
    const char *nameBegin = name.c_str(), *nameEnd = nameBegin + name.size() + 1; // with the terminating 0
    uint8_t *typeDescriptor = nullptr;
    for (const Section &section : sections)
    {
        uint8_t *found = std::search(section.begin, section.end, nameBegin, nameEnd);
        if (found != section.end)
        {
            typeDescriptor = found - 0x10;
            break;
        }
    }
    if (!typeDescriptor)
        return nullptr;
    const uint32_t typeDescriptorRva = (uint32_t)(typeDescriptor - base);

    // Complete object locator of the vtable: signature 1, offset of the subobject, type descriptor,
    // and (x64) its own RVA.
    uint8_t *locator = nullptr;
    for (const Section &section : sections)
    {
        for (uint8_t *p = section.begin; p + 24 <= section.end && !locator; p += 4)
        {
            auto fields = reinterpret_cast<const uint32_t *>(p);
            if (fields[0] == 1 && fields[1] == offset && fields[3] == typeDescriptorRva && fields[5] == (uint32_t)(p - base))
                locator = p;
        }
        if (locator)
            break;
    }
    if (!locator)
        return nullptr;

    // The vtable follows the pointer to the locator.
    for (const Section &section : sections)
    {
        for (uint8_t *p = section.begin; p + 16 <= section.end; p += 8)
        {
            if (*reinterpret_cast<uint8_t **>(p) == locator)
                return reinterpret_cast<void **>(p + 8);
        }
    }
    return nullptr;
}

int StrataHooks::VTableLength(void *const *vtable, const char *module)
{
    auto base = reinterpret_cast<uint8_t *>(GetModuleHandleA(module));
    if (!base || !vtable)
        return 0;
    const std::vector<Section> code = Sections(base, true);
    int length = 0;
    for (;; ++length)
    {
        const auto entry = static_cast<const uint8_t *>(vtable[length]);
        if (std::none_of(code.begin(), code.end(), [entry](const Section &section) { return entry >= section.begin && entry < section.end; }))
            return length;
    }
}

void **StrataHooks::FindCheckedVTable(const char *module, const char *className, int length, uint32_t offset)
{
    void **vtable = FindVTable(module, className, offset);
    if (!vtable)
        return nullptr;
    const int found = VTableLength(vtable, module);
    if (found == length)
        return vtable;
    std::cout << "[Hooks] " << className << " in " << module << " has " << found << " vtable entries instead of " << length
              << ": the game has changed, what uses them stays off\n";
    return nullptr;
}

// Panorama lays out for a height of 1080 and the width that follows from its window size. For the
// back buffer (HMD format, nearly square) that's about 1030 wide, too narrow for the menu. So its
// windows get a part of the back buffer in the aspect ratio of the video mode. The size is passed
// when they are created and with SetClientSize after a mode change: changing the fields alone
// doesn't arrange the panels again.
namespace
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

    // ClientModePortalNormal inherits CreateMove from ClientModeShared.
    void **clientMode = FindCheckedVTable("client.dll", "ClientModeShared", 64);
    if (!clientMode)
    {
        std::cout << "[Hooks] ClientModeShared vtable not found, no VR movement\n";
        return false;
    }
    ok &= InstallHook("CreateMove", clientMode[kClientMode_CreateMove], &hkCreateMove, oCreateMove);

    void **viewModel = FindCheckedVTable("client.dll", "C_BaseViewModel", 262);
    if (!viewModel)
    {
        std::cout << "[Hooks] C_BaseViewModel vtable not found, viewmodel not in the hand\n";
        return false;
    }
    ok &= InstallHook("CalcViewModelView", viewModel[kViewModel_CalcViewModelView], &hkCalcViewModelView, oCalcViewModelView);
    ok &= InstallHook("CalcViewModelLag", viewModel[kViewModel_CalcViewModelLag], &hkCalcViewModelLag, oCalcViewModelLag);

    uint8_t *formatAttachment = CallTarget(static_cast<uint8_t *>(viewModel[kViewModel_FormatAttachment]) + kFormatAttachment_CallOffset);
    if (!formatAttachment)
    {
        std::cout << "[Hooks] FormatViewModelAttachment not found, viewmodel effects will be misplaced\n";
        return false;
    }
    ok &= InstallHook("FormatViewModelAttachment", formatAttachment, &hkFormatViewModelAttachment, oFormatViewModelAttachment);

    g_PortalgunVTable = FindCheckedVTable("client.dll", "C_WeaponPortalgun", 399);
    if (!g_PortalgunVTable)
    {
        std::cout << "[Hooks] C_WeaponPortalgun vtable not found, the gun bobs and there is no aim laser\n";
        return false;
    }
    ok &= InstallHook("AddViewmodelBob", g_PortalgunVTable[kPortalgun_AddViewmodelBob], &hkAddViewmodelBob, oAddViewmodelBob);

    void **videoPanel = FindCheckedVTable("client.dll", "VideoPanel", 241);
    if (videoPanel)
        InstallHook("VideoPanel Paint", videoPanel[kVideoPanel_Paint], &hkVideoPanelPaint, oVideoPanelPaint);
    else
        std::cout << "[Hooks] VideoPanel vtable not found, videos stay invisible in the headset\n";

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
    ok &= InstallHook("ApplyPortalTeleportation", player[kPlayer_ApplyPortalTeleportation], &hkApplyPortalTeleportation, oApplyPortalTeleportation);

    auto client = reinterpret_cast<uint8_t *>(GetModuleHandleA("client.dll"));

    uint8_t *simulateName = FindStringLea(client, kParticleSimulate_Name, 0x15);
    uint8_t *particleSimulate = simulateName ? CallTarget(simulateName + kParticleSimulate_CallOffset) : nullptr;
    if (!particleSimulate)
    {
        std::cout << "[Hooks] ParticleMgr()->Simulate not found, no muzzle position and no aim laser\n";
        return false;
    }
    ok &= InstallHook("ParticleSimulate", particleSimulate, &hkParticleSimulate, oParticleSimulate);

    // Registration of the effect: lea r8, callback; lea rdx, "PortalBlast"
    uint8_t *blastName = FindStringLea(client, "PortalBlast", 0x15);
    if (!blastName || blastName[-7] != 0x4C || blastName[-6] != 0x8D || blastName[-5] != 0x05)
    {
        std::cout << "[Hooks] PortalBlast not found, the shot won't start at the muzzle\n";
        return false;
    }
    ok &= InstallHook("PortalBlast", blastName + *reinterpret_cast<int32_t *>(blastName - 4), &hkPortalBlast, oPortalBlast);

    uint8_t *flashName = FindStringLea(client, kMuzzleFlash_WorldEffect, 0x0D);
    uint8_t *dispatchParticleEffect = flashName ? CallTarget(flashName + kMuzzleFlash_CallOffset) : nullptr;
    if (!dispatchParticleEffect)
    {
        std::cout << "[Hooks] DispatchParticleEffect not found, the muzzle flash stays at the third-person weapon\n";
        return false;
    }
    ok &= InstallHook("DispatchParticleEffect", dispatchParticleEffect, &hkDispatchParticleEffect, oDispatchParticleEffect);

    uint8_t *traceClient = FindTraceFirePortal("client.dll", kTraceFirePortal_CallOffsetClient);
    uint8_t *traceServer = FindTraceFirePortal("server.dll", kTraceFirePortal_CallOffsetServer);
    if (!traceClient || !traceServer)
    {
        std::cout << "[Hooks] TraceFirePortal not found (client " << (void *)traceClient << ", server " << (void *)traceServer
                  << "), portals are aimed from the head\n";
        return false;
    }
    ok &= InstallHook("TraceFirePortal", traceClient, &hkTraceFirePortalClient, oTraceFirePortalClient);
    ok &= InstallHook("TraceFirePortal", traceServer, &hkTraceFirePortalServer, oTraceFirePortalServer, "server.dll");

    // The laser is optional: without these functions there just isn't one.
    uint8_t *warning = FindStringLea(client, kPingPointer_Warning, 0x0D);
    uint8_t *ping = warning ? warning - kPingPointer_WarningOffset : nullptr;
    if (ping && memcmp(ping, kPingPointer_Start, sizeof(kPingPointer_Start)) == 0
        && memcmp(ping + kPingPointer_EffectLeaOffset, kPingPointer_EffectLea, sizeof(kPingPointer_EffectLea)) == 0)
    {
        g_PingEffectOffset = *reinterpret_cast<int32_t *>(ping + kPingPointer_EffectLeaOffset + sizeof(kPingPointer_EffectLea));
        DestroyPingPointer = reinterpret_cast<tDestroyPingPointer>(CallTarget(ping + kPingPointer_DestroyCallOffset));
        SetControlPoint = reinterpret_cast<tSetControlPoint>(CallTarget(ping + kPingPointer_SetControlPointCallOffset));
        SetDrawOnlyForSplitScreenUser = reinterpret_cast<tSetDrawOnlyForSplitScreenUser>(CallTarget(ping + kPingPointer_SetDrawOnlyCallOffset));
        uint8_t *effectName = FindStringLea(reinterpret_cast<uint8_t *>(GetModuleHandleA("server.dll")), kLaserEffect, 0x0D);
        PrecacheParticleSystem = effectName ? reinterpret_cast<tPrecacheParticleSystem>(CallTarget(effectName + kPrecacheParticle_CallOffset)) : nullptr;
        TraceFilterConstructor = reinterpret_cast<tTraceFilterConstructor>(CallTarget(traceClient + kTraceFirePortal_FilterConstructorCallOffset));
        TraceFilterList = reinterpret_cast<tTraceFilterList>(CallTarget(traceClient + kTraceFirePortal_FilterListCallOffset));
        if (DestroyPingPointer && SetControlPoint && SetDrawOnlyForSplitScreenUser && PrecacheParticleSystem && TraceFilterConstructor && TraceFilterList && game->m_EngineTrace)
            CreatePingPointer = reinterpret_cast<tCreatePingPointer>(ping);
    }
    std::cout << "[Hooks] aim laser " << (CreatePingPointer ? "available" : "not found") << "\n";

    InstallRumbleHooks();

    return ok;
}
