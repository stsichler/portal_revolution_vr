// Chell's arms in the player's own view (see arms.cpp), her scale, and the hooks that draw her.
#include "strata_hooks.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include "MinHook.h"
#include "arms.h"
#include "game.h"
#include "log.h"
#include "sdk.h"
#include "vr.h"

namespace StrataHooks
{
    // Chell's arms (experiment, see arms.cpp). The game skips the player in the main view
    // (cl_skip_player_render_in_main_view), unless the eyes look through a portal: this check, called
    // by C_Portal_Player's DrawModel (renderable at +8, [9]) at +0x45, returns true then.
    // rva 0x3e9db0: 40 53 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 48 8B D9
    constexpr uint32_t kPlayer_Renderable = 8;
    constexpr int kRenderable_DrawModel = 9;
    using tDrawModel = int (*)(void *renderable, int flags, const void *instance);
    constexpr int kDrawModel_SkipCallOffset = 0x45;
    constexpr uint8_t kSkipPlayer_Start[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x05 };
    using tSkipPlayer = bool (*)(void *player);
    tSkipPlayer oSkipPlayer = nullptr;

    // C_Portal_Player [192] BuildTransformations(studioHdr, pos, q, cameraTransform, boneMask,
    // boneComputed), ends in C_BaseAnimating's (rva 0x1e6ba0). Then the bones (world) are where
    // +0x2DB8 points, 0x30 bytes each ("mov rdx, [rsi+2DB8h]" there); studioHdr is a CStudioHdr,
    // its first field the studiohdr_t. Bones for the vertices: BONE_USED_BY_VERTEX_LOD0..7.
    // [192] rva 0x20da80: 4C 8B DC 4D 89 4B 20 4D 89 43 18 41 55 41 57 48
    constexpr int kPlayer_BuildTransformations = 192;
    constexpr int kAnimating_Bones = 0x2DB8;
    constexpr int kStudio_BoneCount = 0x9C; // studiohdr_t
    constexpr int kStudio_BoneIndex = 0xA0; // studiohdr_t: where the bones start
    constexpr int kBone_Size = 0xD8, kBone_Flags = 0xA0; // mstudiobone_t
    constexpr int kBoneUsedByVertex = 0x3FC00;
    constexpr float kChell_Height = 76.0f; // units, her crown standing upright (bind pose)
    using tBuildTransformations = void (*)(void *thisptr, void *studioHdr, void *pos, void *q, const void *cameraTransform, int boneMask, void *boneComputed);
    tBuildTransformations oBuildTransformations = nullptr;
    bool g_ArmsHooked = false;
    bool g_ArmsUsable = true; // false once the model turned out not to have Chell's arm bones

    // Renderable [16] GetRenderBounds(mins, maxs), relative to [1] GetRenderOrigin; the player's
    // [17] adds the two for the frustum test. They are the player's: arms held up or far out
    // vanish with them when they are out of view.
    // [16] rva 0x1fa780 (C_BaseAnimating's): 40 56 41 54 41 56 41 57 48 83 EC 38 4D 8B F8 4C
    constexpr int kRenderable_GetRenderOrigin = 1;
    constexpr int kRenderable_GetRenderBounds = 16;
    using tGetRenderBounds = void (*)(void *renderable, Vector &mins, Vector &maxs);
    tGetRenderBounds oGetRenderBounds = nullptr;
    constexpr float kArms_Reach = 1.3f; // m around the head, generous: the arms are somewhere in it

    // The player's bones: the game's ones stay in its bone cache (for portals, the player's copy in
    // a portal, the third-person gun); only while the player is drawn in its own view (portal
    // depth 0) does the cache hold the arms.
    std::vector<matrix3x4_t> g_EyeBones, g_GameBones;
    std::chrono::steady_clock::time_point g_EyeBonesTime; // when g_EyeBones were made
    matrix3x4_t *g_BonesCache = nullptr; // the cache they were made for
    bool g_DrawingEyes = false; // the player is being drawn in its own view
    bool g_CacheHoldsEyes = false;
    tDrawModel oPlayerDrawModel = nullptr;

    // DrawModel flags of shadow renders (STUDIO_SHADOWDEPTHTEXTURE, STUDIO_SHADOWTEXTURE). The
    // skip check doesn't know them: the player's shadow comes from its own view too.
    constexpr int kStudio_ShadowFlags = 0x40000000 | 0x20000000;
    bool g_DrawingShadow = false; // the player's (or its gun's) shadow is being drawn: Chell's
    // CurrentViewID, called by the skip check at +0x22; only for the log.
    constexpr int kSkipPlayer_ViewIdCallOffset = 0x22;
    using tCurrentViewId = int (*)();
    tCurrentViewId CurrentViewId_ = nullptr;

    // The portal depth being rendered (0 = the view itself): g_pPortalRender (a pointer,
    // loaded by the skip check at +0xB4) and its depth (called at +0xBB; mov eax, [rcx+0FA0h]).
    constexpr int kSkipPlayer_PortalRenderOffset = 0xB4;
    constexpr int kSkipPlayer_DepthCallOffset = 0xBB;
    constexpr uint8_t kLoadRip_Rcx[] = { 0x48, 0x8B, 0x0D };
    void **g_PortalRender = nullptr;
    using tPortalDepth = int (*)(void *portalRender);
    tPortalDepth PortalDepth_ = nullptr;

    // C_WeaponPortalgun (renderable at +8, [9]) DrawModel(flags, instance): the third-person gun.
    // rva 0x3d5ef0: 40 55 56 41 56 48 83 EC 60 80 B9
    tDrawModel oGunDrawModel = nullptr;

    // C_CombatWeaponClone (renderable at +8, [9]) DrawModel: the copy of a held weapon that the
    // game draws as its third-person one. +0x3180: the weapon (UpdateClone, called at the start).
    // [9] rva 0x2377f0: 48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48
    constexpr int kWeaponClone_Weapon = 0x3180;
    tDrawModel oCloneDrawModel = nullptr;

    // C_PortalGhostRenderable (renderable at +8, [9]) DrawModel: the copy of an entity on the
    // other side of a portal it is in. Its SetupBones ([12], rva 0xf1340) takes the entity's bones
    // through the entity's SetupBones and moves them through the portal; the entity's handle is at
    // +0x3178 of the renderable ("mov edx, [rcx+3178h]" there).
    // [9] rva 0xf0120: 40 53 55 56 48 83 EC 30 48 8B F1 49 8B D8 48 83
    constexpr int kGhost_Source = 0x3178; // of the renderable
    tDrawModel oGhostDrawModel = nullptr;

    // The check the copy's DrawModel calls first (+0x14), whether to draw (entity). For the local
    // player's copy, at portal depth 1 not through its own portal: when the view portal
    // (g_pPortalRender +0x1038) is the copy's portal (+0x31E0 of the entity) +0x3180.
    // rva 0xf18d0: 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56
    constexpr int kGhostDraw_CheckCallOffset = 0x14;
    constexpr uint8_t kGhostCheck_Start[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56 };
    constexpr int kGhost_Portal = 0x31E0;
    constexpr int kPortal_ViewId = 0x3180;
    constexpr int kPortalRender_ViewPortal = 0x1038;
    using tGhostCheck = bool (*)(void *ghost);
    tGhostCheck oGhostCheck = nullptr;
    bool g_ForceGhost = false;

    // The hooks of the arms: off with the config's Arms (UpdateArmsHooks), so the game's code runs
    // as without them.
    std::vector<void *> g_ArmsHookTargets;
    bool g_ArmsHooksEnabled = true;

    template <typename F>
    bool InstallArmsHook(const char *name, void *target, F detour, F &original)
    {
        const bool installed = InstallHook(name, target, detour, original);
        if (installed)
            g_ArmsHookTargets.push_back(target);
        return installed;
    }

    Arms::Frame HandFrame(const Vector &origin, const Vector &forward, const Vector &right, const Vector &up)
    {
        return { origin, forward, -right, up };
    }

    // The hands where the controllers are, in the game eye's space: the gun hand at the grip of the
    // gun as it sits with the default offsets, the other one mirrored. The right controller is
    // the gun hand's, also when left-handed.
    void HandFrames(Arms::Frame &gunHand, Arms::Frame &otherHand)
    {
        VR *vr = g_StrataGame->m_VR;
        const Vector eyes = g_EyePosition;
        // The config's GunPos offsets move only the gun, HandPos ones the hands.
        const Vector gun = vr->GetHandGunAbsPos(eyes);
        const Vector grip = gun + vr->m_HandGunForward * kGun_GripForward + vr->m_HandGunRight * kGun_GripRight + vr->m_HandGunUp * kGun_GripUp;
        gunHand = HandFrame(grip, vr->m_RightControllerForward, vr->m_RightControllerRight, vr->m_RightControllerUp);
        const Vector offset = grip - vr->GetRightControllerAbsPos(eyes);
        const Vector local(DotProduct(offset, gunHand.forward), DotProduct(offset, gunHand.left), DotProduct(offset, gunHand.up));
        otherHand = HandFrame(vr->GetLeftControllerAbsPos(eyes), vr->m_LeftControllerForward, vr->m_LeftControllerRight, vr->m_LeftControllerUp);
        otherHand.origin = otherHand.origin + otherHand.forward * local.x - otherHand.left * local.y + otherHand.up * local.z;
    }

    // Off (config Arms, VR tab): the player as the game has it, buttons only by use.
    bool ArmsActive()
    {
        return g_ArmsHooked && g_StrataGame->m_VR->m_Arms && VRViewActive();
    }

    // The local player as hkCalcViewModelView has seen it.
    bool IsPlayerWithArms(void *entity)
    {
        Game *game = g_StrataGame;
        return ArmsActive() && entity && entity == g_ViewModelOwner
            && entity == game->GetClientEntity(game->m_EngineClient->GetLocalPlayer());
    }

    // The exit portal's matrix back to the player's side, if the game's eye is through a portal.
    const float (*ToPlayerSide())[4]
    {
        if (!g_GameEyeThrough || !g_GameEyeExit || *reinterpret_cast<void **const *>(g_GameEyeExit) != g_PropPortalVTable)
            return nullptr;
        return reinterpret_cast<const float (*)[4]>(g_GameEyeExit + kPortal_MatrixToLinked);
    }

    int PortalDepth()
    {
        return g_PortalRender && *g_PortalRender && PortalDepth_ ? PortalDepth_(*g_PortalRender) : 0;
    }

    // The portal the current portal view looks through, as the game compares it.
    const uint8_t *ViewPortal()
    {
        return g_PortalRender && *g_PortalRender ? *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(*g_PortalRender) + kPortalRender_ViewPortal) : nullptr;
    }

    // The player is seen from its own eyes: where the game skips it (cl_skip_player_render_in_main_view)
    // it is drawn with the arms. That is the view at portal depth 0 with the eye on the player's
    // side, and, with the eye through a portal, the view back through it (depth 1). Elsewhere (in
    // portals, seen from outside) Chell is as the game has her.
    bool DrawsArms(void *player)
    {
        return g_ArmsUsable && IsPlayerWithArms(player) && oSkipPlayer(player);
    }

    matrix3x4_t *BonesCache(void *player)
    {
        return *reinterpret_cast<matrix3x4_t **>(static_cast<uint8_t *>(player) + kAnimating_Bones);
    }

    // The player is drawn in its own view too. Only the player itself: particles that belong to
    // it, such as the third-person gun's lights (CNewParticleEffect, renderable at +0x10, [9]
    // DrawModel, rva 0x2ddc20), ask too and stay hidden.
    bool hkSkipPlayer(void *player)
    {
        const bool skip = oSkipPlayer(player);
        return g_DrawingEyes || g_DrawingShadow ? false : skip;
    }

    // Shadows of the player and its gun as the game would have them without the skip: Chell's.
    int DrawShadow(tDrawModel draw, void *renderable, int flags, const void *instance)
    {
        g_DrawingShadow = true;
        const int result = draw(renderable, flags, instance);
        g_DrawingShadow = false;
        return result;
    }

    // The flags (and view) the player is drawn with where it has arms, each once.
    void LogArmsDraw(int flags)
    {
        static int logged[16];
        static size_t count = 0;
        if (!VerboseLog() || count == std::size(logged) || std::find(logged, logged + count, flags) != logged + count)
            return;
        logged[count++] = flags;
        std::cout << "[Arms] player drawn with flags 0x" << std::hex << flags << std::dec << " in view " << (CurrentViewId_ ? CurrentViewId_() : -2)
                  << ((flags & kStudio_ShadowFlags) ? ": shadow, Chell's" : ": arms") << "\n";
    }

    // Draws with the arms in the player's bone cache, the game's bones back afterwards. If the
    // bones are made while drawing, hkBuildTransformations leaves the arms in the cache.
    int DrawWithArms(void *player, tDrawModel draw, void *renderable, int flags, const void *instance)
    {
        matrix3x4_t *cache = BonesCache(player);
        if (cache && cache == g_BonesCache && !g_EyeBones.empty() && !g_CacheHoldsEyes)
        {
            g_GameBones.assign(cache, cache + g_EyeBones.size());
            memcpy(cache, g_EyeBones.data(), g_EyeBones.size() * sizeof(matrix3x4_t));
            g_CacheHoldsEyes = true;
        }
        g_DrawingEyes = true;
        const int result = draw(renderable, flags, instance);
        g_DrawingEyes = false;
        cache = BonesCache(player);
        if (g_CacheHoldsEyes && cache && cache == g_BonesCache && g_GameBones.size() == g_EyeBones.size())
            memcpy(cache, g_GameBones.data(), g_GameBones.size() * sizeof(matrix3x4_t));
        g_CacheHoldsEyes = false;
        return result;
    }

    int hkPlayerDrawModel(void *renderable, int flags, const void *instance)
    {
        void *player = static_cast<uint8_t *>(renderable) - kPlayer_Renderable;
        if (!DrawsArms(player))
            return oPlayerDrawModel(renderable, flags, instance);
        LogArmsDraw(flags);
        if (flags & kStudio_ShadowFlags)
            return DrawShadow(oPlayerDrawModel, renderable, flags, instance);
        return DrawWithArms(player, oPlayerDrawModel, renderable, flags, instance);
    }

    // The player's copy in a portal it is in (the player itself is cut off at it) likewise: with
    // the arms where the game hides it as seen from the player's eyes (0xf18d0): at depth 0 with
    // the eye through the portal, at depth 1 through the copy's portal with the eye on the
    // player's side. Elsewhere as the game has it.
    int hkGhostDrawModel(void *renderable, int flags, const void *instance)
    {
        const uint32_t handle = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(renderable) + kGhost_Source);
        auto player = static_cast<uint8_t *>(g_ViewModelOwner);
        if (handle == 0xFFFFFFFF || !player || g_StrataGame->GetClientEntity(handle & 0xFFFF) != g_ViewModelOwner || !g_ArmsUsable || !IsPlayerWithArms(player))
            return oGhostDrawModel(renderable, flags, instance);
        const int depth = PortalDepth();
        const bool through = player[kPlayer_ViewThroughPortal] != 0;
        bool ownSight = depth == 0 && through;
        if (depth == 1 && !through)
        {
            auto portal = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(renderable) - kPlayer_Renderable + kGhost_Portal);
            ownSight = portal && ViewPortal() == portal + kPortal_ViewId;
        }
        if (!ownSight)
            return oGhostDrawModel(renderable, flags, instance);
        g_ForceGhost = true;
        if (flags & kStudio_ShadowFlags) // Chell's shadow, as for the player
        {
            const int shadow = oGhostDrawModel(renderable, flags, instance);
            g_ForceGhost = false;
            return shadow;
        }
        const int result = DrawWithArms(player, oGhostDrawModel, renderable, flags, instance);
        g_ForceGhost = false;
        static int loggedDepth = -1, loggedResult = -1;
        if ((depth != loggedDepth || result != loggedResult) && VerboseLog())
        {
            std::cout << "[Arms] player's copy in a portal with arms at depth " << depth << (result ? ": drawn" : ": not drawn") << "\n";
            loggedDepth = depth;
            loggedResult = result;
        }
        return result;
    }

    bool hkGhostCheck(void *ghost)
    {
        return oGhostCheck(ghost) || g_ForceGhost;
    }

    // The third-person gun doesn't show where the player has arms: they hold the first-person one.
    int hkGunDrawModel(void *renderable, int flags, const void *instance)
    {
        if (DrawsArms(g_ViewModelOwner) && g_BonesCache)
            return flags & kStudio_ShadowFlags ? DrawShadow(oGunDrawModel, renderable, flags, instance) : 0;
        return oGunDrawModel(renderable, flags, instance);
    }

    // The same for the copy of the gun the player holds.
    int hkCloneDrawModel(void *renderable, int flags, const void *instance)
    {
        void *player = g_ViewModelOwner;
        if (DrawsArms(player) && g_BonesCache)
        {
            void *weapon = *reinterpret_cast<void **>(static_cast<uint8_t *>(renderable) - kPlayer_Renderable + kWeaponClone_Weapon);
            const uint32_t handle = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(player) + g_Player_ActiveWeaponHandle);
            if (weapon && handle != 0xFFFFFFFF && weapon == g_StrataGame->GetClientEntity(handle & 0xFFFF))
                return flags & kStudio_ShadowFlags ? DrawShadow(oCloneDrawModel, renderable, flags, instance) : 0;
        }
        return oCloneDrawModel(renderable, flags, instance);
    }

    // The player's bounds include everything the arms can reach from the head.
    void hkGetRenderBounds(void *renderable, Vector &mins, Vector &maxs)
    {
        oGetRenderBounds(renderable, mins, maxs);
        void *player = static_cast<uint8_t *>(renderable) - kPlayer_Renderable;
        if (!g_ArmsUsable || !IsPlayerWithArms(player))
            return;
        VR *vr = g_StrataGame->m_VR;
        const Vector origin = *CallVirtual<const Vector *>(renderable, kRenderable_GetRenderOrigin);
        Vector head = vr->m_6DOF ? g_EyePosition + vr->m_HmdPosRelative : g_EyePosition;
        if (const auto m = ToPlayerSide())
            head = MovedByPortal(m, head);
        head = head - origin;
        const float reach = kArms_Reach * vr->m_VRScale;
        mins = Vector((std::min)(mins.x, head.x - reach), (std::min)(mins.y, head.y - reach), (std::min)(mins.z, head.z - reach));
        maxs = Vector((std::max)(maxs.x, head.x + reach), (std::max)(maxs.y, head.y + reach), (std::max)(maxs.z, head.z + reach));
    }

    // The hands where the controllers are, the head and the aim, for the arms.
    Arms::Input ArmsInput()
    {
        VR *vr = g_StrataGame->m_VR;
        const Vector eyes = g_EyePosition;
        Arms::Input input;
        input.head = vr->m_6DOF ? eyes + vr->m_HmdPosRelative : eyes;
        input.eyes = vr->GetViewOrigin(eyes);
        input.headYaw = Heading(vr->m_HmdForward, vr->m_HmdUp);
        input.unitsPerMeter = vr->m_VRScale;
        input.bodyHeight = vr->m_BodyHeight;

        QAngle aim = vr->m_RightControllerAngAbs; // as in hkCreateMove
        aim.x = std::clamp(aim.x, -89.0f, 89.0f);
        aim.z = 0;
        Vector forward, right, up;
        QAngle::AngleVectors(aim, &forward, &right, &up);
        input.aim = HandFrame(Vector(0, 0, 0), forward, right, up);

        HandFrames(input.gunHand, input.otherHand);
        input.gunInRightHand = !vr->m_LeftHanded;

        // With the game's eye through a portal, all of it back on the player's side.
        if (const auto m = ToPlayerSide())
        {
            const auto turn = [m](Arms::Frame &frame, bool move)
            {
                if (move)
                    frame.origin = MovedByPortal(m, frame.origin);
                frame.forward = RotatedByPortal(m, frame.forward);
                frame.left = RotatedByPortal(m, frame.left);
                frame.up = RotatedByPortal(m, frame.up);
            };
            input.head = MovedByPortal(m, input.head);
            input.eyes = MovedByPortal(m, input.eyes);
            input.headYaw = Heading(RotatedByPortal(m, vr->m_HmdForward), RotatedByPortal(m, vr->m_HmdUp));
            turn(input.aim, false);
            turn(input.gunHand, true);
            turn(input.otherHand, true);
        }
        return input;
    }

    // Chell as tall as the player (BodyHeight), around her feet: the game's bones, so in portals,
    // in her shadow and with the third-person gun; the arms in the player's own view stay.
    std::vector<matrix3x4_t> g_ScaledBones; // as last scaled: a bone the game didn't make anew isn't scaled twice
    void ScaleChell(void *player, const uint8_t *hdr, matrix3x4_t *bones, int count, int boneMask)
    {
        VR *vr = g_StrataGame->m_VR;
        const float scale = std::clamp(vr->m_BodyHeight / 100 * vr->m_VRScale / kChell_Height, 0.5f, 2.0f);
        // The first always, the changes (many while the slider is dragged) only in the verbose log.
        static float loggedScale = 0;
        if (scale != loggedScale && (loggedScale == 0 || VerboseLog()))
        {
            std::cout << "[Arms] Chell scaled by " << scale << " (BodyHeight " << vr->m_BodyHeight << ")\n";
            loggedScale = scale;
        }
        if (std::fabs(scale - 1) < 1e-4f)
            return;
        const Vector feet = *CallVirtual<const Vector *>(static_cast<uint8_t *>(player) + kPlayer_Renderable, kRenderable_GetRenderOrigin);
        const float origin[3] = { feet.x, feet.y, feet.z };
        const uint8_t *bone = hdr + *reinterpret_cast<const int *>(hdr + kStudio_BoneIndex);
        g_ScaledBones.resize(count);
        for (int i = 0; i < count; ++i)
        {
            if (!(*reinterpret_cast<const int *>(bone + i * kBone_Size + kBone_Flags) & boneMask) ||
                !memcmp(&bones[i], &g_ScaledBones[i], sizeof(matrix3x4_t)))
                continue;
            for (int row = 0; row < 3; ++row)
            {
                float *m = bones[i][row];
                m[0] *= scale;
                m[1] *= scale;
                m[2] *= scale;
                m[3] = origin[row] + (m[3] - origin[row]) * scale;
            }
            g_ScaledBones[i] = bones[i];
        }
    }

    // Chell scaled (also without the arms); the arms from the game's bones, for the player's own
    // view; the cache keeps the game's ones, unless the player is being drawn there.
    void hkBuildTransformations(void *thisptr, void *studioHdr, void *pos, void *q, const void *cameraTransform, int boneMask, void *boneComputed)
    {
        oBuildTransformations(thisptr, studioHdr, pos, q, cameraTransform, boneMask, boneComputed);
        Game *game = g_StrataGame;
        if (!studioHdr || !thisptr || thisptr != game->GetClientEntity(game->m_EngineClient->GetLocalPlayer()))
            return;
        matrix3x4_t *bones = BonesCache(thisptr);
        const auto hdr = *static_cast<const uint8_t **>(studioHdr);
        if (!bones || !hdr)
            return;
        const int count = *reinterpret_cast<const int *>(hdr + kStudio_BoneCount);
        // Bones only for other purposes (attachments, the gun's bone merge) stay the game's, scaled.
        if (!IsPlayerWithArms(thisptr) || !(boneMask & kBoneUsedByVertex))
        {
            if (!g_CacheHoldsEyes)
                ScaleChell(thisptr, hdr, bones, count, boneMask);
            return;
        }
        // The arms below from the game's bones as they are; the cache gets them back scaled.
        g_GameBones.assign(bones, bones + count);
        ScaleChell(thisptr, hdr, g_GameBones.data(), count, boneMask);
        const bool applied = Arms::Apply(hdr, bones, ArmsInput());
        g_ArmsUsable = Arms::Usable();
        if (applied)
        {
            g_EyeBones.assign(bones, bones + count);
            g_EyeBonesTime = std::chrono::steady_clock::now();
            g_BonesCache = bones;
        }
        else
        {
            g_EyeBones.clear();
            g_BonesCache = nullptr;
        }
        if (applied && g_DrawingEyes)
            g_CacheHoldsEyes = true;
        else
            memcpy(bones, g_GameBones.data(), count * sizeof(matrix3x4_t));
    }

    // The arms' hooks follow the config's Arms; switched here, outside of any drawing.
    void UpdateArmsHooks()
    {
        const bool wanted = g_StrataGame->m_VR->m_Arms;
        if (wanted == g_ArmsHooksEnabled || g_ArmsHookTargets.empty())
            return;
        int failed = 0;
        for (void *target : g_ArmsHookTargets)
            failed += (wanted ? MH_EnableHook(target) : MH_DisableHook(target)) != MH_OK;
        g_ArmsHooksEnabled = wanted;
        g_EyeBones.clear();
        g_BonesCache = nullptr;
        g_CacheHoldsEyes = false;
        std::cout << "[Hooks] arms " << (wanted ? "on" : "off") << ": " << g_ArmsHookTargets.size() << " hooks "
                  << (wanted ? "enabled" : "disabled") << (failed ? ", some failed" : "") << "\n";
    }

    // Init's part: the arms' hooks; player: C_Portal_Player's vtable.
    void InstallArmsHooks(void **player)
    {
        void **playerRenderable = FindCheckedVTable("client.dll", "C_Portal_Player", 45, kPlayer_Renderable);
        uint8_t *skipPlayer = playerRenderable ? CallTarget(static_cast<uint8_t *>(playerRenderable[kRenderable_DrawModel]) + kDrawModel_SkipCallOffset) : nullptr;
        uint8_t *loadPortalRender = skipPlayer ? skipPlayer + kSkipPlayer_PortalRenderOffset : nullptr;
        uint8_t *depth = skipPlayer ? CallTarget(skipPlayer + kSkipPlayer_DepthCallOffset) : nullptr;
        void **gun = FindCheckedVTable("client.dll", "C_WeaponPortalgun", 45, kPlayer_Renderable);
        if (skipPlayer && memcmp(skipPlayer, kSkipPlayer_Start, sizeof(kSkipPlayer_Start)) == 0
            && memcmp(loadPortalRender, kLoadRip_Rcx, sizeof(kLoadRip_Rcx)) == 0 && depth && gun)
        {
            g_PortalRender = reinterpret_cast<void **>(loadPortalRender + 7 + *reinterpret_cast<int32_t *>(loadPortalRender + 3));
            PortalDepth_ = reinterpret_cast<tPortalDepth>(depth);
            CurrentViewId_ = reinterpret_cast<tCurrentViewId>(CallTarget(skipPlayer + kSkipPlayer_ViewIdCallOffset));
            // Always on: it scales Chell, with or without the arms.
            g_ArmsHooked = InstallHook("BuildTransformations", player[kPlayer_BuildTransformations], &hkBuildTransformations, oBuildTransformations)
                && InstallArmsHook("SkipPlayer", skipPlayer, &hkSkipPlayer, oSkipPlayer)
                && InstallArmsHook("Player DrawModel", playerRenderable[kRenderable_DrawModel], &hkPlayerDrawModel, oPlayerDrawModel)
                && InstallArmsHook("Portalgun DrawModel", gun[kRenderable_DrawModel], &hkGunDrawModel, oGunDrawModel);
        }
        if (!g_ArmsHooked)
            std::cout << "[Hooks] player drawing not found, no arms\n";
        void **clone = g_ArmsHooked ? FindCheckedVTable("client.dll", "C_CombatWeaponClone", 45, kPlayer_Renderable) : nullptr;
        if (g_ArmsHooked && !(clone && InstallArmsHook("WeaponClone DrawModel", clone[kRenderable_DrawModel], &hkCloneDrawModel, oCloneDrawModel)))
            std::cout << "[Hooks] C_CombatWeaponClone not hooked, the third-person gun shows with the arms\n";
        void **ghost = g_ArmsHooked ? FindCheckedVTable("client.dll", "C_PortalGhostRenderable", 45, kPlayer_Renderable) : nullptr;
        if (g_ArmsHooked && !(ghost && InstallArmsHook("PortalGhost DrawModel", ghost[kRenderable_DrawModel], &hkGhostDrawModel, oGhostDrawModel)))
            std::cout << "[Hooks] C_PortalGhostRenderable not hooked, the arms end at portals\n";
        uint8_t *ghostCheck = ghost && oGhostDrawModel ? CallTarget(static_cast<uint8_t *>(ghost[kRenderable_DrawModel]) + kGhostDraw_CheckCallOffset) : nullptr;
        if (oGhostDrawModel && !(ghostCheck && memcmp(ghostCheck, kGhostCheck_Start, sizeof(kGhostCheck_Start)) == 0
                                 && InstallArmsHook("PortalGhost check", ghostCheck, &hkGhostCheck, oGhostCheck)))
            std::cout << "[Hooks] PortalGhost check not hooked, the arms end at portals one looks through\n";
        if (g_ArmsHooked && !InstallArmsHook("GetRenderBounds", playerRenderable[kRenderable_GetRenderBounds], &hkGetRenderBounds, oGetRenderBounds))
            std::cout << "[Hooks] GetRenderBounds not hooked, the arms vanish when the player is out of view\n";
    }
}
