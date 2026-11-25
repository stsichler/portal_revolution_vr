// The portal gun in the hand: viewmodel, muzzle, the shot from it, the aim dot's point and the laser.
#include "strata_hooks.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "game.h"
#include "log.h"
#include "sdk.h"
#include "vr.h"

namespace StrataHooks
{
    // C_BaseViewModel [254] CalcViewModelView(owner, eyePosition, eyeAngles) (uses the path of
    // baseviewmodel_shared.cpp): lag [253], bob [255], then places the viewmodel.
    // [254] rva 0x194c00: 48 8B C4 48 89 70 18 48 89 78 20 55 48 8D 68 E8
    // [253] rva 0x194a10: 48 89 5C 24 10 57 48 81 EC 80 00 00 00 0F 29 74
    constexpr int kViewModel_CalcViewModelLag = 253;
    constexpr int kViewModel_CalcViewModelView = 254;

    using tCalcViewModelLag = void (*)(void *thisptr, Vector &origin, QAngle &angles, QAngle &originalAngles);
    tCalcViewModelLag oCalcViewModelLag = nullptr;
    tCalcViewModelView oCalcViewModelView = nullptr;

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
    // Player m_bIsHoldingSomething: carrying an object, also without the gun; 0 = not found.
    int g_Player_HoldingSomething = 0;

    void **g_PortalgunVTable = nullptr;

    // Where the viewmodel's muzzle is in this frame, for the laser and the shot; the server's code
    // runs outside of rendering, where client bones may not be set up.
    Vector g_Muzzle;
    bool g_MuzzleValid = false;

    Vector g_EyePosition; // of the player in the game, as last seen by hkCalcViewModelView
    Vector g_ShotStart; // where the shot starts, see UpdateShotStart
    bool g_ShotFromEyes = false; // see UpdateShotStart
    Vector g_Aim; // where the shot would land, see UpdateAim
    bool g_AimValid = false;

    GunPlacement g_Gun;

    uint8_t *FindTraceFirePortal(const char *module, int callOffset)
    {
        uint8_t *blast = FindStringLea(reinterpret_cast<uint8_t *>(GetModuleHandleA(module)), "PortalBlast", 0x0D);
        uint8_t *target = blast ? CallTarget(blast + callOffset) : nullptr;
        return target && memcmp(target, kTraceFirePortal_Start, sizeof(kTraceFirePortal_Start)) == 0 ? target : nullptr;
    }

    // The viewmodel (portal gun) sits in the right hand instead of in front of the head.
    void hkCalcViewModelView(void *thisptr, void *owner, const Vector &eyePosition, const QAngle &eyeAngles)
    {
        g_ViewModelOwner = owner;
        g_EyePosition = eyePosition;
        const bool portalPlayer = owner && *static_cast<void ***>(owner) == g_PortalPlayerVTable;
        g_GameEyeThrough = portalPlayer && static_cast<uint8_t *>(owner)[kPlayer_ViewThroughPortal];
        g_GameEyeExit = g_GameEyeThrough ? *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(owner) + kPlayer_ViewExitPortal) : nullptr;
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
        if (blocked != wasBlocked && VerboseLog())
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
            if (VerboseLog())
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
            if (carrying != vr->m_ViewmodelCarrying && VerboseLog())
                std::cout << "[VR] gun carrying: " << carrying << "\n";
            vr->m_ViewmodelCarrying = carrying;

            UpdateShotStart();
            UpdateAim();
            UpdateLaser();
            UpdateButtonPress();
        }
        else
            g_LaserPrecacheFrames = 0;
        oParticleSimulate(thisptr, frameTime);
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

    // Init's part: the viewmodel and the gun's bob. False if they aren't there.
    bool InstallViewModelHooks(bool &ok)
    {
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
        return true;
    }

    // Init's part: muzzle, shot and laser. False if the shot's functions aren't there.
    bool InstallShotHooks(Game *game, bool &ok)
    {
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
        return true;
    }
}
