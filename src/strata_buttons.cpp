// Pressing buttons (brushes, pedestals) with the hands and the portal gun.
#include "strata_hooks.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>
#include "arms.h"
#include "game.h"
#include "log.h"
#include "sdk.h"
#include "vr.h"

namespace StrataHooks
{
    // VModelInfoClient004 is CModelInfoClient (engine.dll): [2] GetModelIndex(name), -1 if the
    // model isn't loaded. [1] GetModel(index) and [3] GetModelName(model) next to it.
    // [2] rva 0x1f06d0: 48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9
    constexpr int kModelInfo_GetModelIndex = 2;
    void *g_ModelInfo = nullptr;

    // Network fields of every entity; 0 = not found. Mins and maxs: of the collision box, in the
    // entity's axes (x forward, y left, z up).
    int g_Entity_ModelIndex = 0;
    int g_Entity_Mins = 0;
    int g_Entity_Maxs = 0;

    // C_BaseEntity [10] GetAbsOrigin, [11] GetAbsAngles, the same in every entity's vtable: also
    // with a parent. Taken from C_BaseButton's vtable and compared with the entity's before a call.
    // [10] rva 0x202c50: 40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 83 E8 00 00 00
    // [11] rva 0x202c30: 40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8D 83 0C 01 00 00
    constexpr int kEntity_GetAbsOrigin = 10;
    constexpr int kEntity_GetAbsAngles = 11;
    void *g_GetAbsOrigin = nullptr;
    void *g_GetAbsAngles = nullptr;

    // Brush buttons (func_button, func_rot_button: also levers and valves) are C_BaseButton on the
    // client. In every map of the game they activate on use.
    void **g_BaseButtonVTable = nullptr;
    constexpr float kButton_BrushMargin = 1; // around the collision box

    // Buttons, pressed with the hands and the gun: when one touches a brush button or the cap of a
    // pedestal button, the next command has the game's use (IN_USE), aimed from the eyes at it (a
    // brush's center, a cap's top). The cap is a disc that stands out of the housing: from base
    // along axis (in the model's axes: x forward, y left, z up) by height, with radius. From the
    // models' vertices (2026-11-25):
    // switch001's face is tilted by 30.2 degrees (its button bone by 31.1), the cap's edge bevelled
    // by 1; underground_testchamber_button's switch has a rounded edge.
    struct ButtonModel
    {
        const char *name;
        Vector base, axis;
        float height, radius;
    };
    const ButtonModel kButtonModels[] = {
        { "models/props/switch001.mdl", { 0, -0.36f, 42.40f }, { 0, -0.504f, 0.864f }, 2.75f, 4.6f }, // prop_button
        { "models/props_underground/underground_testchamber_button.mdl", { 0, 0, 43.9f }, { 0, 0, 1 }, 4.0f, 4.5f }, // prop_under_button
    };
    // So far out of the button before it presses again; 3 felt too far (Stefan, 2026-11-25).
    constexpr float kButton_ReleaseMargin = 1;
    // A hand presses with a ball around its grip, a bit towards the knuckles; the gun with balls
    // along its length (both estimated).
    constexpr float kHand_PressForward = 1.5f;
    constexpr float kHand_PressRadius = 2.0f;
    // And balls around Chell's wrists and the joints and tips of her fingers, while the arms are
    // drawn. Fingers of 1.0 pressed too early (Stefan, 2026-11-25).
    constexpr float kHand_WristRadius = 1.0f;
    constexpr float kHand_FingerRadius = 0.5f;
    constexpr auto kHand_BonesMaxAge = std::chrono::milliseconds(100);
    constexpr float kGun_PressRadius = 2.5f;

    ButtonPress g_ButtonPress;

    // Whether the ball (center, radius) is in the button (brush or pedestal cap), with margin added
    // all around; center: of the button, in the world.
    // at, for the log: of a pedestal, the ball's height above the cap's base and distance from its
    // axis; of a brush, its distance from the box.
    bool BallInButton(const uint8_t *entity, const ButtonModel *model, const Vector &ball, float margin, Vector &center, Vector &at)
    {
        const Vector origin = *CallVirtual<const Vector *>(entity, kEntity_GetAbsOrigin);
        Vector forward, right, up;
        QAngle::AngleVectors(*CallVirtual<const QAngle *>(entity, kEntity_GetAbsAngles), &forward, &right, &up);
        const Vector local = ball - origin;

        if (model)
        {
            const auto toWorld = [&](const Vector &v) { return forward * v.x - right * v.y + up * v.z; };
            const Vector axis = toWorld(model->axis);
            const Vector fromBase = local - toWorld(model->base);
            const float height = DotProduct(fromBase, axis);
            const float radial = VectorLength(fromBase - axis * height);
            center = origin + toWorld(model->base + model->axis * model->height);
            at = Vector(height, radial, 0);
            // The ball's distance from the disc.
            const float along = (std::max)({ 0.0f, height - model->height, -height });
            const float across = (std::max)(0.0f, radial - model->radius);
            return along * along + across * across <= margin * margin;
        }

        const Vector mins = *reinterpret_cast<const Vector *>(entity + g_Entity_Mins);
        const Vector maxs = *reinterpret_cast<const Vector *>(entity + g_Entity_Maxs);
        const Vector inEntity(DotProduct(local, forward), -DotProduct(local, right), DotProduct(local, up));
        const Vector middle = (mins + maxs) * 0.5f;
        center = origin + forward * middle.x - right * middle.y + up * middle.z;
        const Vector nearest(std::clamp(inEntity.x, mins.x, maxs.x), std::clamp(inEntity.y, mins.y, maxs.y), std::clamp(inEntity.z, mins.z, maxs.z));
        const float distance = VectorLength(inEntity - nearest);
        at = Vector(distance, 0, 0);
        return distance <= margin + kButton_BrushMargin;
    }

    // Presses a button when a hand or the gun enters it: the other hand, the gun hand, and the gun
    // along its length, if the player has it. Not while carrying an object: the game's use would
    // drop it.
    void UpdateButtonPress()
    {
        ButtonPress &state = g_ButtonPress;
        Game *game = g_StrataGame;
        VR *vr = game->m_VR;
        auto player = static_cast<uint8_t *>(g_ViewModelOwner);
        if (!vr->m_Arms)
        {
            state.touching.clear();
            state.pending = false;
            return;
        }
        if (!g_GetAbsOrigin || !g_GetAbsAngles || !player || g_ViewModelOwner != game->GetClientEntity(game->m_EngineClient->GetLocalPlayer()))
            return;
        // A gun without a muzzle for a moment: the button it's in mustn't press again.
        if (*reinterpret_cast<const uint32_t *>(player + g_Player_ActiveWeaponHandle) != 0xFFFFFFFF && !g_MuzzleValid)
            return;

        struct Ball
        {
            Vector center;
            float radius;
            const char *part;
            bool right; // the physical hand it belongs to, for the pulse
        };
        static std::vector<Ball> balls;
        balls.clear();
        Arms::Frame gunHand, otherHand;
        HandFrames(gunHand, otherHand);
        const bool leftHanded = vr->m_LeftHanded;
        balls.push_back({ gunHand.origin + gunHand.forward * kHand_PressForward, kHand_PressRadius, leftHanded ? "left hand" : "right hand", !leftHanded });
        balls.push_back({ otherHand.origin + otherHand.forward * kHand_PressForward, kHand_PressRadius, leftHanded ? "right hand" : "left hand", leftHanded });
        // The fingers and knuckles, as the arms have just been drawn (Chell's right hand is the
        // right controller's, also when left-handed).
        if (!g_EyeBones.empty() && std::chrono::steady_clock::now() - g_EyeBonesTime < kHand_BonesMaxAge)
        {
            static std::vector<Vector> points;
            for (int side = 0; side < 2; ++side)
            {
                points.clear();
                Arms::HandPoints(g_EyeBones.data(), g_EyeBones.size(), side, points);
                for (size_t p = 0; p < points.size(); ++p)
                {
                    if (p == 0)
                        balls.push_back({ points[p], kHand_WristRadius, side == 0 ? "right wrist" : "left wrist", side == 0 });
                    else
                        balls.push_back({ points[p], kHand_FingerRadius, side == 0 ? "right fingers" : "left fingers", side == 0 });
                }
            }
        }
        if (g_MuzzleValid)
        {
            Vector forward, right, up;
            QAngle::AngleVectors(g_Gun.angles, &forward, &right, &up);
            const Vector grip = g_Gun.position + forward * kGun_GripForward + right * kGun_GripRight + up * kGun_GripUp;
            // The front ball ends at the muzzle.
            const float length = VectorLength(grip - g_Muzzle);
            const Vector front = length > kGun_PressRadius ? g_Muzzle + (grip - g_Muzzle) * (kGun_PressRadius / length) : grip;
            for (int i = 0; i <= kGun_Samples; ++i)
                balls.push_back({ front + (grip - front) * (static_cast<float>(i) / kGun_Samples), kGun_PressRadius, "gun", !leftHanded });
        }

        int modelIndices[std::size(kButtonModels)];
        for (size_t m = 0; m < std::size(kButtonModels); ++m)
            modelIndices[m] = g_ModelInfo && g_Entity_ModelIndex ? CallVirtual<int>(g_ModelInfo, kModelInfo_GetModelIndex, kButtonModels[m].name) : -1;
        const bool brushes = g_Entity_Mins && g_Entity_Maxs;

        std::vector<int> touching;
        int pressed = 0;
        bool brush = false;
        const char *part = nullptr;
        bool rightHand = false;
        Vector target, pressedAt, size;
        const int highest = game->m_ClientEntityList->GetHighestEntityIndex();
        for (int i = 1; i <= highest; ++i)
        {
            auto entity = reinterpret_cast<const uint8_t *>(game->GetClientEntity(i));
            if (!entity)
                continue;
            void *const *vtable = *reinterpret_cast<void *const *const *>(entity);
            const ButtonModel *model = nullptr;
            if (!(brushes && vtable == g_BaseButtonVTable))
            {
                // A short or an int: the low half holds the index either way.
                const int modelIndex = g_Entity_ModelIndex ? *reinterpret_cast<const int16_t *>(entity + g_Entity_ModelIndex) : -1;
                for (size_t m = 0; m < std::size(kButtonModels) && !model; ++m)
                {
                    if (modelIndices[m] >= 0 && modelIndex == modelIndices[m])
                        model = &kButtonModels[m];
                }
                if (!model)
                    continue;
            }
            if (vtable[kEntity_GetAbsOrigin] != g_GetAbsOrigin || vtable[kEntity_GetAbsAngles] != g_GetAbsAngles)
                continue;

            const bool wasTouching = std::find(state.touching.begin(), state.touching.end(), i) != state.touching.end();
            const float margin = wasTouching ? kButton_ReleaseMargin : 0;
            for (size_t b = 0; b < balls.size(); ++b)
            {
                Vector center, at;
                if (!BallInButton(entity, model, balls[b].center, balls[b].radius + margin, center, at))
                    continue;
                touching.push_back(i);
                if (!wasTouching && !pressed)
                {
                    pressed = i;
                    brush = !model;
                    part = balls[b].part;
                    rightHand = balls[b].right;
                    target = center;
                    pressedAt = at;
                    if (brush)
                        size = *reinterpret_cast<const Vector *>(entity + g_Entity_Maxs) - *reinterpret_cast<const Vector *>(entity + g_Entity_Mins);
                }
                break;
            }
        }

        // Carrying, with or without the gun.
        const bool carrying = vr->m_ViewmodelCarrying || (g_Player_HoldingSomething && player[g_Player_HoldingSomething]);
        if (pressed && !carrying)
        {
            state.pending = true;
            state.target = target;
            vr->PulseButtonPress(rightHand);
            static bool logged = false;
            if (!logged || VerboseLog())
            {
                std::cout << "[VR] button " << pressed << " pressed with the " << part;
                if (brush)
                    std::cout << " (brush " << size.x << "x" << size.y << "x" << size.z << ", " << pressedAt.x << " from its box)\n";
                else
                    std::cout << " (pedestal, " << pressedAt.x << " above its cap's base, " << pressedAt.y << " from its axis)\n";
            }
            logged = true;
        }
        state.touching = std::move(touching);
    }

    // Init's part: what pressing buttons needs.
    void FindButtons(Game *game)
    {
        void *modelInfo = game->GetInterface("engine.dll", "VModelInfoClient004");
        void **modelInfoVTable = FindCheckedVTable("engine.dll", "CModelInfoClient", 69);
        if (modelInfo && modelInfoVTable && *static_cast<void ***>(modelInfo) == modelInfoVTable)
            g_ModelInfo = modelInfo;
        else
            std::cout << "[Hooks] model info not usable, no pressing pedestal buttons with the gun\n";
        g_BaseButtonVTable = FindCheckedVTable("client.dll", "C_BaseButton", 190);
        if (g_BaseButtonVTable)
        {
            g_GetAbsOrigin = g_BaseButtonVTable[kEntity_GetAbsOrigin];
            g_GetAbsAngles = g_BaseButtonVTable[kEntity_GetAbsAngles];
        }
        else
            std::cout << "[Hooks] C_BaseButton vtable not found, no pressing buttons with the gun\n";
    }
}
