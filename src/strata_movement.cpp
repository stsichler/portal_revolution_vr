// Walking with the stick, roomscale (the player follows the head) and turning with portals.
#include "strata_hooks.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "game.h"
#include "log.h"
#include "sdk.h"
#include "vr.h"

namespace StrataHooks
{
    // ClientModeShared [24] CreateMove(sampleTime, cmd), called by CInput [3] CreateMove; returning
    // true makes cmd->viewangles the engine's view angles.
    // [24] rva 0x287530: 40 53 48 83 EC 30 0F 29 74 24 20 B9 FF FF FF FF
    constexpr int kClientMode_CreateMove = 24;

    using tCreateMove = bool (*)(void *thisptr, float flInputSampleTime, CUserCmd *cmd);
    tCreateMove oCreateMove = nullptr;

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

    PortalPassage g_PortalPassage;

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

    RoomscaleState g_Roomscale;

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
        if (follow != state.following && VerboseLog())
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
    // walk stick walks where its controller points (or the head, HeadOrientedWalking), the keyboard
    // relative to the head. The engine's view angles stay with the head (set in hkRenderView).
    bool hkCreateMove(void *thisptr, float flInputSampleTime, CUserCmd *cmd)
    {
        const bool result = oCreateMove(thisptr, flInputSampleTime, cmd);

        VR *vr = g_StrataGame->m_VR;
        if (!VRViewActive())
            g_ButtonPress.pending = false;
        if (!cmd || !cmd->command_number || !VRViewActive())
            return result;

        QAngle aim = vr->m_RightControllerAngAbs;
        // A button touched with the gun: use, aimed at it (the game's use starts at the eyes).
        const bool pressButton = g_ButtonPress.pending;
        g_ButtonPress.pending = false;
        if (pressButton)
            QAngle::VectorAngles(g_ButtonPress.target - g_EyePosition, aim);
        aim.x = std::clamp(aim.x, -89.0f, 89.0f);
        aim.z = 0;

        float forward = 0, side = 0;
        // Keyboard movement is relative to the engine's view angles, i.e. the head.
        TurnMove(cmd->forwardmove, cmd->sidemove, vr->m_HmdAngAbs.y, aim.y, forward, side);
        vr::InputAnalogActionData_t walk;
        if (vr->GetAnalogActionData(vr->m_ActionWalk, walk) && walk.bActive)
            TurnMove(walk.y * MAX_LINEAR_SPEED, walk.x * MAX_LINEAR_SPEED, vr->WalkYaw(), aim.y, forward, side);
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
        if (pressButton)
            cmd->buttons |= IN_USE;

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
        if (VerboseLog())
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
                if (VerboseLog())
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
        if (atWall != g_Roomscale.atWall && VerboseLog())
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
        if (pull != state.pullingBack && VerboseLog())
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
        if (state.passedPortal && VerboseLog())
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

    // Init's part: CreateMove. False if it isn't there.
    bool InstallMovementHooks(bool &ok)
    {
        // ClientModePortalNormal inherits CreateMove from ClientModeShared.
        void **clientMode = FindCheckedVTable("client.dll", "ClientModeShared", 64);
        if (!clientMode)
        {
            std::cout << "[Hooks] ClientModeShared vtable not found, no VR movement\n";
            return false;
        }
        ok &= InstallHook("CreateMove", clientMode[kClientMode_CreateMove], &hkCreateMove, oCreateMove);
        return true;
    }

    // Init's part: ApplyPortalTeleportation of the player's vtable.
    void InstallPortalTeleportationHook(void **player, bool &ok)
    {
        ok &= InstallHook("ApplyPortalTeleportation", player[kPlayer_ApplyPortalTeleportation], &hkApplyPortalTeleportation, oApplyPortalTeleportation);
    }
}
