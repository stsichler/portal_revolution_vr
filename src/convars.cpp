#include "convars.h"
#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <variant>
#include "strata_hooks.h"
#include "vr.h"

// The variables are ConVars of vstdlib.dll, the DLL with the console variable system: the mod
// fills memory of its own with that class's vtables and lets its Create set it up. Strata has the
// layout of CS:GO; indices and offsets checked against vstdlib.dll of 2025-01-05.

namespace
{
    constexpr int kCvar_RegisterConCommand = 10; // ICvar (VEngineCvar007)
    constexpr int kConVar_InternalSetValue = 14;  // (const char *value), checks and clamps
    constexpr int kConVar_Create = 17;            // see tCreate
    constexpr uint32_t kConVar_IConVar = 0x30;    // subobject with the second vtable
    constexpr size_t kConVar_Registered = 0x10;   // bool
    constexpr size_t kConVar_Flags = 0x28;        // int
    constexpr size_t kConVar_Parent = 0x38;       // ConVar *, the object itself for ours
    constexpr size_t kConVar_Float = 0x54;        // float, xor-ed with the low 32 bits of parent
    constexpr size_t kConVar_Int = 0x58;          // int, the same
    constexpr size_t kConVar_Size = 0x200;        // 0xb0 used (two callback vectors at 0x70, 0x90)
    constexpr int FCVAR_UNREGISTERED = 1;         // Create doesn't put it in vstdlib's own list

    // A change callback, 16 bytes; empty when both are 0.
    struct Callback
    {
        void *function = nullptr;
        void *context = nullptr;
    };

    typedef void(__fastcall *tCreate)(void *convar, const char *name, const char *defaultValue, int flags, const char *help,
                                      bool hasMin, float min, bool hasMax, float max, const Callback *callback1,
                                      const Callback *callback2);
    typedef void(__fastcall *tInternalSetValue)(void *convar, const char *value);
    typedef void(__fastcall *tRegisterConCommand)(void *cvar, void *command, bool fromCommandLine);

    struct Setting
    {
        const char *key; // in config.txt
        const char *name;
        const char *help;
        std::variant<bool VR::*, int VR::*, float VR::*> member;
        double scale = 1; // variable = member * scale: the menu's sliders only take whole numbers
        uint8_t *convar = nullptr;
        std::string defaultValue; // the value at the start; Create keeps the pointer
        double lastConVar = 0;
        double lastMember = 0;
        bool unwritten = false; // changed, but not in config.txt yet
        std::chrono::steady_clock::time_point changedAt;
    };

    // A slider changes a variable many times while it's dragged: written once it rests that long.
    constexpr std::chrono::seconds kWriteDelay{1};

    Setting g_Settings[] = {
        {"SnapTurning", "prvr_snap_turning", "1: the view jumps by prvr_snap_turn_angle. 0: it turns smoothly", &VR::m_SnapTurning},
        {"SnapTurnAngle", "prvr_snap_turn_angle", "Degrees", &VR::m_SnapTurnAngle},
        {"TurnSpeed", "prvr_turn_speed", "Smooth turning: degrees per second with the stick fully over", &VR::m_TurnSpeed, 1000},
        {"LeftHanded", "prvr_left_handed", "1: the portal gun and the buttons change hands", &VR::m_LeftHanded},
        {"WalkWithRightStick", "prvr_walk_with_right_stick", "1: the right stick walks, the left one turns", &VR::m_WalkWithRightStick},
        {"6DOF", "prvr_6dof", "0: only the head's rotation counts, not its position", &VR::m_6DOF},
        {"SeatedMode", "prvr_seated_mode", "0: your real eye height. 1: the eye height of the game's player", &VR::m_SeatedMode},
        {"SeatedHeightOffset", "prvr_seated_height_offset", "Seated mode only: moves the eyes up (+) or down (-), source units", &VR::m_SeatedHeightOffset},
        {"Roomscale", "prvr_roomscale", "1: the player follows your head when you walk in your room", &VR::m_Roomscale},
        {"AimMode", "prvr_aim_mode", "0: none, 1: aim dot, 2: laser", &VR::m_AimMode},
        {"ApplyPitchAndRollPortalRotationOffset", "prvr_portal_pitch_roll", "1: the camera pitch and roll follow the exit portal", &VR::m_ApplyPitchAndRollPortalRotationOffset},
        {"CameraUprightRecoveryTime", "prvr_camera_upright_ms", "Milliseconds until the camera is upright again after a portal, 0: at once", &VR::m_CameraUprightRecoveryTime, 1000},
        {"VignetteWalking", "prvr_vignette_walking", "Darkens the edge of the view when walking with the stick. 0: off, 1: black", &VR::m_VignetteWalking},
        {"VignetteTurning", "prvr_vignette_turning", "The same when turning smoothly. 0: off, 1: black", &VR::m_VignetteTurning},
        {"VignetteAfterPortal", "prvr_vignette_after_portal", "The same while the camera turns upright after a portal. 0: off, 1: black", &VR::m_VignetteAfterPortal},
        {"VignetteClearAngle", "prvr_vignette_clear_angle", "Degrees from the center of the view that stay clear", &VR::m_VignetteClearAngle},
    };

    bool g_Tried = false;
    tInternalSetValue g_InternalSetValue = nullptr;

    double MemberValue(const VR &vr, const Setting &setting)
    {
        return std::visit([&vr](auto member) { return static_cast<double>(vr.*member); }, setting.member);
    }

    void SetMember(VR &vr, const Setting &setting, double value)
    {
        std::visit([&vr, value](auto member)
            {
                using T = std::remove_reference_t<decltype(vr.*member)>;
                if constexpr (std::is_same_v<T, bool>)
                    vr.*member = value != 0;
                else
                    vr.*member = static_cast<T>(value);
            }, setting.member);
    }

    double ConVarValue(const Setting &setting)
    {
        const uint8_t *parent = *reinterpret_cast<uint8_t *const *>(setting.convar + kConVar_Parent);
        const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(parent));
        if (std::holds_alternative<float VR::*>(setting.member))
        {
            const uint32_t bits = *reinterpret_cast<const uint32_t *>(parent + kConVar_Float) ^ key;
            float value;
            memcpy(&value, &bits, sizeof(value));
            return value;
        }
        return static_cast<int32_t>(*reinterpret_cast<const uint32_t *>(parent + kConVar_Int) ^ key);
    }

    // As the variable takes it, or for config.txt: bools as 1/0 or true/false.
    std::string Text(const Setting &setting, double value, bool forConfig)
    {
        if (std::holds_alternative<bool VR::*>(setting.member))
            return forConfig ? (value != 0 ? "true" : "false") : (value != 0 ? "1" : "0");
        std::ostringstream text;
        if (std::holds_alternative<int VR::*>(setting.member))
            text << static_cast<int>(value);
        else
            text << static_cast<float>(value);
        return text.str();
    }

    // Replaces the value of the key in config.txt and keeps the comment, or adds a line.
    void WriteConfig(const char *key, const std::string &value)
    {
        std::string text;
        {
            std::ifstream file("VR\\config.txt", std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        const std::string prefix = std::string(key) + "=";
        size_t line = 0;
        while (line < text.size() && text.compare(line, prefix.size(), prefix) != 0)
        {
            line = text.find('\n', line);
            line = line == std::string::npos ? text.size() : line + 1;
        }
        if (line < text.size())
        {
            const size_t begin = line + prefix.size();
            size_t end = text.find_first_of("#\r\n", begin);
            if (end == std::string::npos)
                end = text.size();
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
                --end;
            text.replace(begin, end - begin, value);
        }
        else
        {
            const char *newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
            if (!text.empty() && text.back() != '\n')
                text += newline;
            text += prefix + value + newline;
        }
        std::ofstream file("VR\\config.txt", std::ios::binary | std::ios::trunc);
        file << text;
        if (!file)
            std::cout << "[ConVars] can't write VR\\config.txt\n";
    }

    void Create(VR &vr)
    {
        void **vtable = StrataHooks::FindVTable("vstdlib.dll", "ConVar");
        void **iconvarVTable = StrataHooks::FindVTable("vstdlib.dll", "ConVar", kConVar_IConVar);
        typedef void *(*tCreateInterface)(const char *name, int *returnCode);
        HMODULE vstdlib = GetModuleHandleA("vstdlib.dll");
        auto createInterface = vstdlib ? (tCreateInterface)GetProcAddress(vstdlib, "CreateInterface") : nullptr;
        void *cvar = createInterface ? createInterface("VEngineCvar007", nullptr) : nullptr;
        if (!vtable || !iconvarVTable || !cvar)
        {
            std::cout << "[ConVars] vstdlib's ConVar or ICvar not found, no options in the menu\n";
            return;
        }
        auto create = reinterpret_cast<tCreate>(vtable[kConVar_Create]);
        auto registerConCommand = reinterpret_cast<tRegisterConCommand>((*static_cast<void ***>(cvar))[kCvar_RegisterConCommand]);
        g_InternalSetValue = reinterpret_cast<tInternalSetValue>(vtable[kConVar_InternalSetValue]);

        static const Callback noCallback;
        int registered = 0;
        for (Setting &setting : g_Settings)
        {
            // Never freed: the console keeps the pointer until the game ends.
            auto convar = static_cast<uint8_t *>(VirtualAlloc(nullptr, kConVar_Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!convar)
                break;
            *reinterpret_cast<void ***>(convar) = vtable;
            *reinterpret_cast<void ***>(convar + kConVar_IConVar) = iconvarVTable;
            setting.defaultValue = Text(setting, MemberValue(vr, setting) * setting.scale, false);
            create(convar, setting.name, setting.defaultValue.c_str(), FCVAR_UNREGISTERED, setting.help, false, 0, false, 0,
                   &noCallback, &noCallback);
            *reinterpret_cast<int *>(convar + kConVar_Flags) &= ~FCVAR_UNREGISTERED;
            if (!convar[kConVar_Registered])
                registerConCommand(cvar, convar, false);
            setting.convar = convar;
            setting.lastConVar = ConVarValue(setting);
            setting.lastMember = MemberValue(vr, setting);
            ++registered;
        }
        std::cout << "[ConVars] " << registered << " of " << std::size(g_Settings) << " registered\n";
    }
}

void ConVars::Sync(VR &vr)
{
    if (!g_Tried)
    {
        g_Tried = true;
        Create(vr);
    }

    const auto now = std::chrono::steady_clock::now();
    for (Setting &setting : g_Settings)
    {
        if (!setting.convar)
            continue;
        const double fromConVar = ConVarValue(setting);
        const double fromMember = MemberValue(vr, setting);
        if (fromConVar != setting.lastConVar)
        {
            // Changed in the menu or the console.
            SetMember(vr, setting, fromConVar / setting.scale);
            setting.lastConVar = fromConVar;
            setting.lastMember = MemberValue(vr, setting);
            setting.unwritten = true;
            setting.changedAt = now;
        }
        else if (fromMember != setting.lastMember)
        {
            // Changed in config.txt.
            g_InternalSetValue(setting.convar, Text(setting, fromMember * setting.scale, false).c_str());
            setting.lastMember = fromMember;
            setting.lastConVar = ConVarValue(setting);
        }

        if (setting.unwritten && now - setting.changedAt >= kWriteDelay)
        {
            setting.unwritten = false;
            const std::string value = Text(setting, setting.lastMember, true);
            WriteConfig(setting.key, value);
            std::cout << "[ConVars] " << setting.name << " -> config.txt " << setting.key << "=" << value << "\n";
        }
    }
}
