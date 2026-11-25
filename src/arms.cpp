#include "arms.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    // studiohdr_t (version 49): bone count and offset of the bones; mstudiobone_t: 0xD8 bytes,
    // +0 offset of its name (from the bone), +4 parent.
    constexpr int kStudio_BoneCount = 0x9C;
    constexpr int kStudio_BoneIndex = 0xA0;
    constexpr int kBone_Size = 0xD8;
    constexpr int kBone_Parent = 4;

    // Body measures, from the head (HMD) in meters; estimated for an adult.
    constexpr float kShoulderBelowHead = 0.20f;
    constexpr float kShoulderBehindHead = 0.02f;
    constexpr float kShoulderHalfWidth = 0.17f;
    constexpr float kMaxStretch = 1.6f; // of the arm, to reach the hand

    // Chell's arms are short and thin for the player (Stefan: "almost starved"): scaled.
    constexpr float kArmLength = 1.2f;
    constexpr float kArmThickness = 1.25f;
    constexpr float kHandSize = 1.2f;

    // Which part of an arm a bone moves with. Shoulder: clavicle and scapula, shrunk into the
    // shoulder joint; Hidden: shrunk into the eyes, where it can't be seen.
    enum class Part : uint8_t { Hidden, Shoulder, Upper, Fore, Hand };

    struct Skeleton
    {
        const void *studioHdr = nullptr;
        bool valid = false;
        int bicep[2] = {}, elbow[2] = {}, wrist[2] = {}; // 0 = right, 1 = left
        int weapon = -1;
        std::vector<Part> part;
        std::vector<uint8_t> side;
        std::vector<int> mirrorOf; // of a bone of the left arm: its counterpart on the right, or -1
        std::vector<int> hand[2]; // the wrist and the bones hanging on it
        struct Tip
        {
            int bone, parent;
        };
        std::vector<Tip> tips[2]; // the fingers' last joints: their tips lie beyond
    } g_Skeleton;

    // Chell's right wrist in the frame of the hand (forward, left, up), at the gun's grip: how the
    // game's animation holds the gun, measured over 10 s of its idle sway with the body facing the
    // aim (2026-11-25). The measurement had the hand turned 10.3 degrees outward around up
    // (Stefan: "about 10 degrees too far apart"): without that. Fixed, as the measurement differed
    // at every start.
    const Vector kGripPosition(-0.187067f, 0.125073f, 1.50971f);
    const Vector kGripAxes[3] = { { -0.9908f, 0.0f, 0.1354f }, { -0.0031f, -0.9997f, -0.0230f }, { 0.1354f, -0.0232f, 0.9905f } };

    struct Mat3
    {
        Vector c[3]; // columns
        Vector operator*(const Vector &v) const { return c[0] * v.x + c[1] * v.y + c[2] * v.z; }
    };

    Mat3 operator*(const Mat3 &a, const Mat3 &b) { return { a * b.c[0], a * b.c[1], a * b.c[2] }; }

    Mat3 Transposed(const Mat3 &m)
    {
        return { Vector(m.c[0].x, m.c[1].x, m.c[2].x), Vector(m.c[0].y, m.c[1].y, m.c[2].y), Vector(m.c[0].z, m.c[1].z, m.c[2].z) };
    }

    Mat3 Identity() { return { Vector(1, 0, 0), Vector(0, 1, 0), Vector(0, 0, 1) }; }

    Vector Unit(const Vector &v)
    {
        const float length = VectorLength(v);
        return length > 0 ? v * (1.0f / length) : v;
    }

    Vector Column(const matrix3x4_t &m, int c) { return Vector(m[0][c], m[1][c], m[2][c]); }

    void SetColumn(matrix3x4_t &m, int c, const Vector &v)
    {
        m[0][c] = v.x;
        m[1][c] = v.y;
        m[2][c] = v.z;
    }

    Mat3 Rotation(const matrix3x4_t &m) { return { Column(m, 0), Column(m, 1), Column(m, 2) }; }

    void Set(matrix3x4_t &m, const Mat3 &linear, const Vector &position)
    {
        for (int c = 0; c < 3; ++c)
            SetColumn(m, c, linear.c[c]);
        SetColumn(m, 3, position);
    }

    // The shortest rotation from one direction to another (both of length 1).
    Mat3 RotationBetween(const Vector &from, const Vector &to)
    {
        const Vector axis = CrossProduct(from, to);
        const float s = VectorLength(axis), c = DotProduct(from, to);
        if (s < 1e-6f)
            return Identity();
        const Vector k = axis * (1.0f / s);
        const auto rotate = [&](const Vector &v) { return v * c + CrossProduct(k, v) * s + k * (DotProduct(k, v) * (1 - c)); };
        return { rotate(Vector(1, 0, 0)), rotate(Vector(0, 1, 0)), rotate(Vector(0, 0, 1)) };
    }

    // Scales by along in the direction n (length 1), by across perpendicular to it.
    Mat3 ScaleAlong(const Vector &n, float along, float across)
    {
        const float f = along - across;
        return { Vector(across, 0, 0) + n * (n.x * f), Vector(0, across, 0) + n * (n.y * f), Vector(0, 0, across) + n * (n.z * f) };
    }

    // Turns by angle (radians) around the axis n (length 1).
    Mat3 RotationAround(const Vector &n, float angle)
    {
        const float s = std::sin(angle), c = std::cos(angle);
        const auto rotate = [&](const Vector &v) { return v * c + CrossProduct(n, v) * s + n * (DotProduct(n, v) * (1 - c)); };
        return { rotate(Vector(1, 0, 0)), rotate(Vector(0, 1, 0)), rotate(Vector(0, 0, 1)) };
    }

    Mat3 Scaled(const Mat3 &m, float factor) { return { m.c[0] * factor, m.c[1] * factor, m.c[2] * factor }; }

    // Moves the bone as v -> to + linear * (v - from) moves the points of the world.
    void Move(matrix3x4_t &m, const Mat3 &linear, const Vector &from, const Vector &to)
    {
        Set(m, linear * Rotation(m), to + linear * (Column(m, 3) - from));
    }

    Mat3 Axes(const Arms::Frame &frame) { return { frame.forward, frame.left, frame.up }; }

    void Orthonormalize(Vector axes[3])
    {
        axes[0] = Unit(axes[0]);
        axes[1] = Unit(axes[1] - axes[0] * DotProduct(axes[0], axes[1]));
        axes[2] = CrossProduct(axes[0], axes[1]);
    }

    const uint8_t *Bone(const uint8_t *hdr, int index)
    {
        return hdr + *reinterpret_cast<const int *>(hdr + kStudio_BoneIndex) + index * kBone_Size;
    }

    bool Load(const void *studioHdr)
    {
        Skeleton &skeleton = g_Skeleton;
        if (skeleton.studioHdr == studioHdr)
            return skeleton.valid;
        skeleton = {};
        skeleton.studioHdr = studioHdr;

        const auto hdr = static_cast<const uint8_t *>(studioHdr);
        const int count = *reinterpret_cast<const int *>(hdr + kStudio_BoneCount);
        const auto find = [&](const char *name)
        {
            for (int i = 0; i < count; ++i)
            {
                const uint8_t *bone = Bone(hdr, i);
                if (!strcmp(reinterpret_cast<const char *>(bone + *reinterpret_cast<const int *>(bone)), name))
                    return i;
            }
            return -1;
        };
        const char *names[2][4] = { { "clavicle_R", "bicep_R", "elbow_R", "wrist_R" }, { "clavicle_L", "bicep_L", "elbow_L", "wrist_L" } };
        int clavicle[2];
        for (int s = 0; s < 2; ++s)
        {
            clavicle[s] = find(names[s][0]);
            skeleton.bicep[s] = find(names[s][1]);
            skeleton.elbow[s] = find(names[s][2]);
            skeleton.wrist[s] = find(names[s][3]);
        }
        const int weapon = skeleton.weapon = find("weapon_bone");
        for (int s = 0; s < 2; ++s)
        {
            if (clavicle[s] < 0 || skeleton.bicep[s] < 0 || skeleton.elbow[s] < 0 || skeleton.wrist[s] < 0 || weapon < 0)
            {
                std::cout << "[Arms] the player model has no bones of Chell's arms (" << count << " bones), no arms\n";
                return false;
            }
        }

        // A bone moves with the nearest of these it hangs on. The weapon bone carries the
        // game's third-person gun: hidden.
        skeleton.part.assign(count, Part::Hidden);
        skeleton.side.assign(count, 0);
        for (int i = 0; i < count; ++i)
        {
            if (i == weapon)
                continue;
            for (int j = i; j >= 0; j = *reinterpret_cast<const int *>(Bone(hdr, j) + kBone_Parent))
            {
                int s = 0;
                Part part = Part::Hidden;
                for (; s < 2 && part == Part::Hidden; ++s)
                {
                    if (j == skeleton.wrist[s])
                        part = Part::Hand;
                    else if (j == skeleton.elbow[s])
                        part = Part::Fore;
                    else if (j == skeleton.bicep[s])
                        part = Part::Upper;
                    else if (j == clavicle[s])
                        part = Part::Shoulder;
                }
                if (part != Part::Hidden)
                {
                    skeleton.part[i] = part;
                    skeleton.side[i] = static_cast<uint8_t>(s - 1);
                    break;
                }
            }
        }
        // Left bones are named like the right ones with L for R: bicep_L, lowArmL_twist0_0,
        // LupperArmTwistJNT, LscapAJNT.
        skeleton.mirrorOf.assign(count, -1);
        int mirrored = 0;
        for (int i = 0; i < count; ++i)
        {
            if (skeleton.part[i] == Part::Hidden || skeleton.side[i] != 1)
                continue;
            const uint8_t *bone = Bone(hdr, i);
            std::string name = reinterpret_cast<const char *>(bone + *reinterpret_cast<const int *>(bone));
            if (name.size() > 2 && name.compare(name.size() - 2, 2, "_L") == 0)
                name[name.size() - 1] = 'R';
            else if (name.compare(0, 6, "lowArm") == 0 && name.size() > 6 && name[6] == 'L')
                name[6] = 'R';
            else if (!name.empty() && name[0] == 'L')
                name[0] = 'R';
            const int partner = find(name.c_str());
            if (partner >= 0 && skeleton.part[partner] == skeleton.part[i] && skeleton.side[partner] == 0)
            {
                skeleton.mirrorOf[i] = partner;
                ++mirrored;
            }
        }
        for (int s = 0; s < 2; ++s)
        {
            skeleton.hand[s].clear();
            skeleton.tips[s].clear();
        }
        for (int i = 0; i < count; ++i)
        {
            if (skeleton.part[i] != Part::Hand)
                continue;
            skeleton.hand[skeleton.side[i]].push_back(i);
            bool leaf = i != skeleton.wrist[skeleton.side[i]];
            for (int j = 0; j < count && leaf; ++j)
                leaf = *reinterpret_cast<const int *>(Bone(hdr, j) + kBone_Parent) != i;
            if (leaf)
                skeleton.tips[skeleton.side[i]].push_back({ i, *reinterpret_cast<const int *>(Bone(hdr, i) + kBone_Parent) });
        }
        skeleton.valid = true;
        std::cout << "[Arms] Chell's arm bones found (" << count << " bones, " << mirrored << " on the left with a counterpart, "
                  << skeleton.hand[0].size() << " in the right hand, " << skeleton.tips[0].size() << " fingertips)\n";
        return true;
    }

    struct ArmPose
    {
        bool valid = false;
        Vector shoulder, shoulderBefore, elbow, elbowBefore, foreDirection;
        Mat3 upper, fore; // the points of the upper arm and forearm, from where they were
        float foreLength = 0;
        float twist = 0; // radians the wrist turns the forearm further, around foreDirection
        Mat3 wristAxes;
        Vector wrist;
        matrix3x4_t wristBefore;
    };

    // The component of v perpendicular to n (length 1).
    Vector Across(const Vector &v, const Vector &n) { return v - n * DotProduct(v, n); }

    // Two bones from the shoulder to the wrist, the elbow bent down, outward and back.
    ArmPose SolveArm(const matrix3x4_t *bones, int s, const Vector &shoulder, const Mat3 &wristAxes, Vector wrist,
                     const Vector &forward, const Vector &outward, const Vector &up)
    {
        const Skeleton &skeleton = g_Skeleton;
        ArmPose pose;
        const matrix3x4_t &wristBefore = bones[skeleton.wrist[s]];
        const Vector s0 = Column(bones[skeleton.bicep[s]], 3), e0 = Column(bones[skeleton.elbow[s]], 3), w0 = Column(wristBefore, 3);
        const float upper0 = VectorLength(e0 - s0), fore0 = VectorLength(w0 - e0);
        const Vector toWrist = wrist - shoulder;
        const float distance = VectorLength(toWrist);
        if (upper0 < 0.1f || fore0 < 0.1f || distance < 0.1f)
            return pose;

        const float stretch = std::clamp(distance / ((upper0 + fore0) * kArmLength), 1.0f, kMaxStretch);
        const float upper = upper0 * kArmLength * stretch, fore = fore0 * kArmLength * stretch;
        const float d = std::clamp(distance, std::fabs(upper - fore) + 0.01f, upper + fore);
        const Vector direction = toWrist * (1.0f / distance);
        wrist = shoulder + direction * d; // out of reach: at the end of the arm

        Vector bend = Across(outward * 0.6f - up - forward * 0.3f, direction);
        bend = VectorLength(bend) > 1e-3f ? Unit(bend) : -up;
        const float along = (upper * upper - fore * fore + d * d) / (2 * d);
        const Vector elbow = shoulder + direction * along + bend * std::sqrt((std::max)(0.0f, upper * upper - along * along));

        const Vector upperDirection = Unit(elbow - shoulder);
        const Mat3 upperTurn = RotationBetween(Unit(e0 - s0), upperDirection);
        pose.upper = ScaleAlong(upperDirection, upper / upper0, kArmThickness) * upperTurn;
        const Vector foreDirection = Unit(wrist - elbow);
        const Mat3 foreTurn = RotationBetween(Unit(upperTurn * (w0 - e0)), foreDirection) * upperTurn;
        pose.fore = ScaleAlong(foreDirection, fore / fore0, kArmThickness) * foreTurn;

        // The wrist turned around the forearm against how the animation had it: the axis of the
        // wrist most across the forearm, carried along with the forearm, and where the hand has it.
        const Vector foreBefore = Unit(w0 - e0);
        int across = 0;
        for (int c = 1; c < 3; ++c)
        {
            if (std::fabs(DotProduct(Column(wristBefore, c), foreBefore)) < std::fabs(DotProduct(Column(wristBefore, across), foreBefore)))
                across = c;
        }
        const Vector carried = Across(foreTurn * Column(wristBefore, across), foreDirection);
        const Vector wanted = Across(wristAxes.c[across], foreDirection);
        pose.twist = std::atan2(DotProduct(foreDirection, CrossProduct(carried, wanted)), DotProduct(carried, wanted));

        pose.valid = true;
        pose.shoulder = shoulder;
        pose.shoulderBefore = s0;
        pose.elbow = elbow;
        pose.elbowBefore = e0;
        pose.foreDirection = foreDirection;
        pose.foreLength = fore;
        pose.wrist = wrist;
        pose.wristAxes = wristAxes;
        pose.wristBefore = wristBefore;
        return pose;
    }
}

bool Arms::Apply(const void *studioHdr, matrix3x4_t *bones, const Input &input)
{
    if (!studioHdr || !bones || !Load(studioHdr))
        return false;
    const Skeleton &skeleton = g_Skeleton;

    // The left arm as the mirror image of the right one, which holds the gun, across the aim's
    // vertical plane: so the left hand holds its controller as the right one holds the gun, and
    // the left forearm's twist starts from there (the animation's left hand holds the gun from
    // below, half a turn away). Positions mirrored; axes mirrored and turned around, as in the
    // model's bind pose.
    {
        const Vector normal = input.aim.left;
        const Vector center = (Column(bones[skeleton.bicep[0]], 3) + Column(bones[skeleton.bicep[1]], 3)) * 0.5f;
        const auto mirror = [&normal](const Vector &v) { return v - normal * (2 * DotProduct(v, normal)); };
        for (size_t i = 0; i < skeleton.mirrorOf.size(); ++i)
        {
            const int partner = skeleton.mirrorOf[i];
            if (partner < 0)
                continue;
            const matrix3x4_t &right = bones[partner];
            const Mat3 axes = { -mirror(Column(right, 0)), -mirror(Column(right, 1)), -mirror(Column(right, 2)) };
            Set(bones[i], axes, center + mirror(Column(right, 3) - center));
        }
    }

    const float yaw = DEG2RAD(input.headYaw);
    const Vector forward(std::cos(yaw), std::sin(yaw), 0), left(-std::sin(yaw), std::cos(yaw), 0), up(0, 0, 1);
    const float meter = input.unitsPerMeter;

    // The grip of the right wrist, and its mirror image for the left one: positions
    // mirrored in the hand's frame (left to right), and the left bones' axes are those of the
    // right ones mirrored and turned around (as in the model's bind pose).
    static const Mat3 grip = []
    {
        Vector axes[3] = { kGripAxes[0], kGripAxes[1], kGripAxes[2] };
        Orthonormalize(axes);
        return Mat3{ axes[0], axes[1], axes[2] };
    }();
    const auto mirror = [](const Vector &v) { return Vector(v.x, -v.y, v.z); };
    const Mat3 gripLeft = { -mirror(grip.c[0]), -mirror(grip.c[1]), -mirror(grip.c[2]) };

    ArmPose poses[2];
    for (int s = 0; s < 2; ++s)
    {
        const Frame &hand = (s == 0) == input.gunInRightHand ? input.gunHand : input.otherHand;
        const Mat3 handAxes = Axes(hand);
        const Mat3 wristAxes = handAxes * (s == 0 ? grip : gripLeft);
        const Vector wrist = hand.origin + handAxes * (s == 0 ? kGripPosition : mirror(kGripPosition));
        const Vector outward = s == 0 ? -left : left;
        const Vector shoulder = input.head - up * (kShoulderBelowHead * meter) - forward * (kShoulderBehindHead * meter) + outward * (kShoulderHalfWidth * meter);
        poses[s] = SolveArm(bones, s, shoulder, wristAxes, wrist, forward, outward, up);
        if (!poses[s].valid)
            return false;
    }

    const Mat3 none = { Vector(0, 0, 0), Vector(0, 0, 0), Vector(0, 0, 0) };
    for (size_t i = 0; i < skeleton.part.size(); ++i)
    {
        matrix3x4_t &bone = bones[i];
        const ArmPose &pose = poses[skeleton.side[i]];
        switch (skeleton.part[i])
        {
        case Part::Hidden:
            Set(bone, none, input.eyes);
            break;
        case Part::Shoulder:
            Set(bone, none, pose.shoulder);
            break;
        case Part::Upper:
            Move(bone, pose.upper, pose.shoulderBefore, pose.shoulder);
            break;
        case Part::Fore:
        {
            // The twist bones take a share of the wrist's turn, the more the closer to it.
            Move(bone, pose.fore, pose.elbowBefore, pose.elbow);
            const float share = std::clamp(DotProduct(Column(bone, 3) - pose.elbow, pose.foreDirection) / pose.foreLength, 0.0f, 1.0f);
            if (static_cast<int>(i) != skeleton.elbow[skeleton.side[i]])
                Move(bone, RotationAround(pose.foreDirection, pose.twist * share), pose.elbow, pose.elbow);
            break;
        }
        case Part::Hand:
        {
            // Where it was relative to the wrist, now relative to the wrist in the hand.
            const Mat3 toWrist = Transposed(Rotation(pose.wristBefore));
            const Mat3 axes = toWrist * Rotation(bone);
            const Vector position = toWrist * (Column(bone, 3) - Column(pose.wristBefore, 3));
            Set(bone, Scaled(pose.wristAxes * axes, kHandSize), pose.wrist + pose.wristAxes * (position * kHandSize));
            break;
        }
        }
    }

    static bool logged = false;
    if (!logged)
    {
        std::cout << "[Arms] first frame with arms\n";
        logged = true;
    }
    return true;
}

bool Arms::Usable()
{
    return !g_Skeleton.studioHdr || g_Skeleton.valid;
}

void Arms::HandPoints(const matrix3x4_t *bones, size_t count, int side, std::vector<Vector> &points)
{
    const Skeleton &skeleton = g_Skeleton;
    if (!skeleton.valid || (side != 0 && side != 1) || count < skeleton.part.size())
        return;
    points.push_back(Column(bones[skeleton.wrist[side]], 3));
    for (const int bone : skeleton.hand[side])
    {
        if (bone != skeleton.wrist[side])
            points.push_back(Column(bones[bone], 3));
    }
    // A finger's last bone is as long as the one before it (Chell's: 0.7 to 1.2 units).
    for (const auto &tip : skeleton.tips[side])
    {
        const Vector joint = Column(bones[tip.bone], 3);
        points.push_back(joint + (joint - Column(bones[tip.parent], 3)));
    }
}
