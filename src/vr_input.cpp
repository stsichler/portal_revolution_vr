// The controllers: poses, actions and what they do in the game and the menus, their vibration
// and the buttons named in the game's hints.
#include "vr.h"
#include <Windows.h>
#include <windowsx.h>
#include "game.h"
#include "strata_hooks.h"
#include "log.h"
#include "d3d11_hooks.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>

// The window shows the menu's part of the back buffer scaled to its size, but Panorama takes mouse
// coordinates as pixels of that part. So mouse messages of the window get scaled in the menu.
// The laser's messages already are in those pixels and carry a mark instead. This goes in front of
// the window procedure of inputsystem.dll, which passes the mouse on to Panorama.
namespace
{
    constexpr WPARAM kMouseFromLaser = 0x80000000; // not used by the MK_ flags
    WNDPROC g_GameWindowProc = nullptr;

    LRESULT CALLBACK MenuMouseWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        // The wheel messages have screen coordinates.
        if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL)
        {
            RECT client;
            if (wParam & kMouseFromLaser)
                wParam &= ~kMouseFromLaser;
            else if (VR::s_MenuVisible && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
                lParam = MAKELPARAM(GET_X_LPARAM(lParam) * VR::s_MenuPartWidth / client.right, GET_Y_LPARAM(lParam) * VR::s_MenuPartHeight / client.bottom);
        }
        return CallWindowProcW(g_GameWindowProc, window, message, wParam, lParam);
    }
}

namespace
{
    // The action sets of action_manifest.json without base, by LeftHanded + 2 * WalkWithRightStick.
    const char *const kActionSets[VR::kSetCount] = { "gunright_walkleft", "gunleft_walkleft", "gunright_walkright", "gunleft_walkright" };
    // The actions in each of them.
    const std::pair<VR::Action VR::*, const char *> kActions[] = {
        {&VR::m_ActionPrimaryAttack, "PrimaryAttack"}, {&VR::m_ActionSecondaryAttack, "SecondaryAttack"},
        {&VR::m_ActionJump, "Jump"}, {&VR::m_ActionCrouch, "Crouch"}, {&VR::m_ActionUse, "Use"}, {&VR::m_Pause, "Pause"},
        {&VR::m_ActionWalk, "Walk"}, {&VR::m_ActionTurn, "Turn"}, {&VR::m_ActionResetPosition, "ResetPosition"},
        {&VR::m_MenuSelect, "MenuSelect"}, {&VR::m_MenuBack, "MenuBack"},
        {&VR::m_MenuUp, "MenuUp"}, {&VR::m_MenuDown, "MenuDown"}, {&VR::m_MenuLeft, "MenuLeft"}, {&VR::m_MenuRight, "MenuRight"},
    };
    // Removed in d2ce664, without effect in Portal: Revolution; for a port to another game. What
    // ProcessInput sent, and the old default on Touch with the gun in the right hand:
    //   ThirdAttack  "ent_fire @att3 trigger" on press (third portal, Portal Reloaded)  left trigger
    //   Reload       +reload / -reload                                                left trigger
    //   QuickPing    +quick_ping / -quick_ping (Portal 2 co-op)                       X
    //   RemoteView   +remote_view / -remote_view (Portal 2 co-op)                     right stick click
    //   PrevItem     invprev on press                                                 right stick up
    //   NextItem     invnext on press                                                 right stick down
    //   Flashlight   impulse 100 on press                                             not bound
    //   Spray        impulse 201 on press                                             not bound
    // Never read: Scoreboard (X), ShowHUD (not bound), ActivateVR (A). The hints (kHintCommands)
    // had quick_ping, remote_view, reload, invprev, invnext, impulse 100 and impulse 201.
    // Old manifest and bindings: git show d2ce664^:package-files/SteamVRActionManifest/<file>
}

int VR::SetActionManifest(const char *fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\SteamVRActionManifest\\%s", currentDir, fileName);

    if (m_Input->SetActionManifestPath(path) != vr::VRInputError_None)
    {
        Game::errorMsg("SetActionManifestPath failed");
    }

    for (int set = 0; set < kSetCount; ++set)
    {
        const std::string path = std::string("/actions/") + kActionSets[set];
        m_Input->GetActionSetHandle(path.c_str(), &m_ActionSets[set]);
        for (const auto &[member, name] : kActions)
            m_Input->GetActionHandle((path + "/in/" + name).c_str(), &(this->*member).handle[set]);
    }
    m_Input->GetActionHandle("/actions/base/in/PointerLeft", &m_ActionPointerLeft);
    m_Input->GetActionHandle("/actions/base/in/PointerRight", &m_ActionPointerRight);
    m_Input->GetActionHandle("/actions/base/out/vibration_left", &m_ActionVibrationLeft);
    m_Input->GetActionHandle("/actions/base/out/vibration_right", &m_ActionVibrationRight);
    m_Input->GetInputSourceHandle("/user/hand/left", &m_InputHandLeft);
    m_Input->GetInputSourceHandle("/user/hand/right", &m_InputHandRight);

    // Haptic outputs (rumble) only work while their action set is active.
    m_ActiveActionSet[1] = {};
    m_Input->GetActionSetHandle("/actions/base", &m_ActiveActionSet[1].ulActionSet);
    UpdateActiveActionSet();

    return 0;
}

// The game's gamepad rumble (StrataHooks::Rumble): portal gun effects in the gun hand, scaled by
// ForceFeedbackPortalGun, the others (damage) in both hands as they are. Our own: a light hum in
// the gun hand while it holds an object. Pulses overlap: one per frame, a bit longer than a frame.
void VR::UpdateRumble(bool menuVisible)
{
    constexpr float kPulseSeconds = 0.05f, kFrequency = 160.0f;
    constexpr float kCarryAmplitude = 0.1f; // at ForceFeedbackPortalGun 1; about the motors' minimum at 50 %
    float game;
    bool gunOnly;
    if (!m_ForceFeedback || !StrataHooks::Rumble(game, gunOnly))
        return;
    game = (std::min)(game, 1.0f);
    const float gunScale = std::clamp(m_ForceFeedbackPortalGun, 0.0f, 1.0f);
    float gunHand = gunOnly ? game * gunScale : game;
    const float otherHand = gunOnly ? 0.0f : game;
    if (m_ViewmodelCarrying && !menuVisible)
        gunHand = (std::max)(gunHand, kCarryAmplitude * gunScale);

    // The haptic actions are bound to the physical hands, not swapped for LeftHanded.
    const float left = m_LeftHanded ? gunHand : otherHand;
    const float right = m_LeftHanded ? otherHand : gunHand;
    for (const auto &[action, amplitude] : { std::pair{m_ActionVibrationLeft, left}, std::pair{m_ActionVibrationRight, right} })
    {
        if (amplitude <= 0)
            continue;
        const vr::EVRInputError error = m_Input->TriggerHapticVibrationAction(action, 0, kPulseSeconds, kFrequency, amplitude, vr::k_ulInvalidInputValueHandle);
        static vr::EVRInputError lastError = vr::VRInputError_None;
        if (error != lastError)
        {
            lastError = error;
            std::cout << "[Rumble] TriggerHapticVibrationAction: error " << error << "\n";
        }
    }
}

void VR::PulseButtonPress(bool rightHand)
{
    constexpr float kPulseSeconds = 0.04f, kFrequency = 160.0f, kAmplitude = 0.4f; // 0.06 and 0.6 felt too strong
    if (!m_ForceFeedback || !m_Input)
        return;
    const vr::EVRInputError error = m_Input->TriggerHapticVibrationAction(rightHand ? m_ActionVibrationRight : m_ActionVibrationLeft, 0,
                                                                         kPulseSeconds, kFrequency, kAmplitude, vr::k_ulInvalidInputValueHandle);
    static vr::EVRInputError lastError = vr::VRInputError_None;
    if (error != lastError)
    {
        lastError = error;
        std::cout << "[Rumble] button press pulse: error " << error << "\n";
    }
}

namespace
{
    // The commands ProcessInput sends (and walking/turning, which move without one).
    const std::pair<const char *, VR::Action VR::*> kHintCommands[] = {
        {"attack", &VR::m_ActionPrimaryAttack}, {"attack2", &VR::m_ActionSecondaryAttack},
        {"jump", &VR::m_ActionJump}, {"duck", &VR::m_ActionCrouch}, {"toggle_duck", &VR::m_ActionCrouch},
        {"use", &VR::m_ActionUse}, {"gameui_activate", &VR::m_Pause},
        {"forward", &VR::m_ActionWalk}, {"back", &VR::m_ActionWalk}, {"moveleft", &VR::m_ActionWalk},
        {"moveright", &VR::m_ActionWalk}, {"left", &VR::m_ActionTurn}, {"right", &VR::m_ActionTurn},
    };
}

const char *VR::HintButtonName(const char *binding)
{
    constexpr auto kRefresh = std::chrono::seconds(1); // the bindings or the gun hand may change

    std::string command = binding[0] == '+' || binding[0] == '-' ? binding + 1 : binding;
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    struct Entry
    {
        const char *name = nullptr;
        std::chrono::steady_clock::time_point at;
    };
    static std::unordered_map<std::string, Entry> cache;
    const auto now = std::chrono::steady_clock::now();
    const auto cached = cache.find(command);
    if (cached != cache.end() && now - cached->second.at < kRefresh)
        return cached->second.name;

    const auto known = std::find_if(std::begin(kHintCommands), std::end(kHintCommands), [&](const auto &c) { return command == c.first; });
    const char *name = nullptr;
    if (known != std::end(kHintCommands))
    {
        name = ActionButtonName(this->*known->second);
        // No origins while SteamVR's overlay or the menu has the input: keep the last name.
        if (!name && cached != cache.end())
            name = cached->second.name;
    }

    const bool changed = cached == cache.end() || cached->second.name != name;
    cache[command] = { name, now };
    if (changed)
        std::cout << "[Hints] " << binding << ": " << (name ? name : known != std::end(kHintCommands) ? "no VR button bound" : "no VR action, keyboard key") << "\n";
    return name;
}

const char *VR::ActionButtonName(const Action &action)
{
    static std::unordered_set<std::string> names; // the game keeps the pointers: never freed

    // Short names for the game's key icons (2 characters fit a square one), with R/L in front.
    // Not for letters on controllers whose four buttons have different ones (B is right).
    const auto describe = [&](vr::VRInputValueHandle_t origin, std::string &out)
    {
        vr::InputOriginInfo_t info = {};
        if (m_Input->GetOriginTrackedDeviceInfo(origin, &info, sizeof(info)) != vr::VRInputError_None)
            return false;
        const std::string hand = info.devicePath == m_InputHandLeft ? "L " : "R ";
        std::string part = info.rchRenderModelComponentName;
        std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        // The component of the controller's model, e.g. "trigger", "thumbstick".
        static const std::pair<const char *, const char *> kParts[] = {
            {"trigger", "Trigger"}, {"grip", "Grip"}, {"stick", "Stick"}, {"pad", "Pad"}, {"menu", "Menu"}, {"system", "Menu"},
        };
        for (const auto &[contains, label] : kParts)
        {
            if (part.find(contains) != std::string::npos)
            {
                out = hand + label;
                return true;
            }
        }
        // A button with a letter: SteamVR's name has it as a word of its own ("B-Taste", "B Button").
        // Otherwise SteamVR's name (SteamVR gives no model component for the Touch controllers).
        char text[64] = {};
        if (m_Input->GetOriginLocalizedName(origin, text, sizeof(text), vr::VRInputString_InputSource) != vr::VRInputError_None || !text[0])
            return false;
        const size_t length = strlen(text);
        for (size_t i = 0; i < length; ++i)
        {
            const bool startsWord = i == 0 || !isalnum((unsigned char)text[i - 1]);
            const bool endsWord = i + 1 == length || !isalnum((unsigned char)text[i + 1]);
            if (startsWord && endsWord && strchr("ABXY", text[i]))
            {
                char type[64] = {};
                m_System->GetStringTrackedDeviceProperty(info.trackedDeviceIndex, vr::Prop_ControllerType_String, type, sizeof(type));
                const bool ownLetters = !strcmp(type, "oculus_touch") || !strcmp(type, "vive_cosmos_controller");
                out = (ownLetters ? "" : hand) + text[i];
                return true;
            }
        }
        out = hand + text; // can be long
        return true;
    };

    // The first origin of the action that has a name.
    vr::VRInputValueHandle_t origins[16] = {};
    if (m_Input->GetActionOrigins(m_ActionSets[(std::max)(m_ActiveSet, 0)], ActiveHandle(action), origins, (uint32_t)std::size(origins)) != vr::VRInputError_None)
        return nullptr;
    std::string label;
    for (const vr::VRInputValueHandle_t origin : origins)
    {
        if (origin == vr::k_ulInvalidInputValueHandle)
            break;
        if (describe(origin, label))
            return names.insert(label).first->c_str();
    }
    return nullptr;
}

// One line in the log with the button of every command of the active set, for reports (most hints
// are rare). Once SteamVR names a button, at the latest after kWait: it names none for a while
// after the start or a set change. Not in the menu (no origins while it has the input).
void VR::LogHintButtons(bool menuVisible)
{
    constexpr auto kWait = std::chrono::seconds(10); // then also without names (nothing bound?)
    if (menuVisible)
        m_HintButtonsSet = -1; // wait anew afterwards
    if (m_HintButtonsLogged == m_ActiveSet || m_ActiveSet < 0 || menuVisible)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (m_HintButtonsSet != m_ActiveSet)
    {
        m_HintButtonsSet = m_ActiveSet;
        m_HintButtonsSince = now;
    }
    bool any = false;
    std::string line = "[Hints] VR buttons:";
    for (const auto &[command, action] : kHintCommands)
    {
        const char *name = ActionButtonName(this->*action);
        any = any || name;
        line += std::string(" ") + command + "=" + (name ? name : "-") + ",";
    }
    if (!any && now - m_HintButtonsSince < kWait)
        return;
    line.back() = '\n';
    std::cout << line;
    m_HintButtonsLogged = m_ActiveSet;
}

void VR::GetPoses()
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];

    vr::TrackedDeviceIndex_t leftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t rightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);

    if (m_LeftHanded)
        std::swap(leftControllerIndex, rightControllerIndex);

    // A hand without an assigned controller (not switched on yet, lost) has the index
    // k_unTrackedDeviceIndexInvalid; its pose stays invalid and GetPoseData keeps the last one.
    auto poseOf = [this](vr::TrackedDeviceIndex_t index)
    {
        vr::TrackedDevicePose_t pose = {};
        if (index < vr::k_unMaxTrackedDeviceCount)
            pose = m_Poses[index];
        return pose;
    };
    vr::TrackedDevicePose_t leftControllerPose = poseOf(leftControllerIndex);
    vr::TrackedDevicePose_t rightControllerPose = poseOf(rightControllerIndex);

    // The hands aim along SteamVR's laser (tip pose) when it is known; otherwise the grip pose,
    // corrected in UpdateHandTracking.
    auto useTip = [this](vr::TrackedDevicePose_t &pose, vr::ETrackedControllerRole role)
    {
        vr::HmdMatrix34_t tip;
        if (!pose.bPoseIsValid || GetPointerPose(role, tip) < 0)
            return false;
        pose.mDeviceToAbsoluteTracking = tip;
        return true;
    };
    vr::ETrackedControllerRole leftRole = vr::TrackedControllerRole_LeftHand, rightRole = vr::TrackedControllerRole_RightHand;
    if (m_LeftHanded)
        std::swap(leftRole, rightRole);
    m_AimFromTip[1] = useTip(leftControllerPose, leftRole);
    m_AimFromTip[0] = useTip(rightControllerPose, rightRole);
    static int loggedTip[2] = { -1, -1 };
    for (int hand = 0; hand < 2; ++hand)
    {
        if (loggedTip[hand] != (int)m_AimFromTip[hand])
        {
            std::cout << "[VR] " << (hand ? "left" : "right") << " hand aims along the " << (m_AimFromTip[hand] ? "tip pose" : "grip pose - 30 deg") << "\n";
            loggedTip[hand] = m_AimFromTip[hand];
        }
    }

    GetPoseData(hmdPose, m_HmdPose);
    GetPoseData(leftControllerPose, m_LeftControllerPose);
    GetPoseData(rightControllerPose, m_RightControllerPose);
}

void VR::UpdatePosesAndActions()
{
    vr::VRCompositor()->WaitGetPoses(m_Poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    UpdateActiveActionSet();
    m_Input->UpdateActionState(m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), (uint32_t)std::size(m_ActiveActionSet));
}

// LeftHanded and WalkWithRightStick can change while the game runs (menu, config.txt).
void VR::UpdateActiveActionSet()
{
    const int active = m_LeftHanded + 2 * m_WalkWithRightStick;
    if (active == m_ActiveSet)
        return;
    m_ActiveActionSet[0] = {};
    m_ActiveActionSet[0].ulActionSet = m_ActionSets[active];
    // A button held in the old set never reports its release.
    if (m_ActiveSet >= 0)
    {
        for (const char *command : { "-attack", "-attack2", "-jump", "-duck", "-use" })
            m_Game->ClientCmd_Unrestricted(command);
    }
    m_ActiveSet = active;
    std::cout << "[VR] action set: " << kActionSets[active] << "\n";
}

void VR::GetViewParameters()
{
    vr::HmdMatrix34_t eyeToHeadLeft = m_System->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t eyeToHeadRight = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    m_EyeToHeadTransformPosLeft.x = eyeToHeadLeft.m[0][3];
    m_EyeToHeadTransformPosLeft.y = eyeToHeadLeft.m[1][3];
    m_EyeToHeadTransformPosLeft.z = eyeToHeadLeft.m[2][3];

    m_EyeToHeadTransformPosRight.x = eyeToHeadRight.m[0][3];
    m_EyeToHeadTransformPosRight.y = eyeToHeadRight.m[1][3];
    m_EyeToHeadTransformPosRight.z = eyeToHeadRight.m[2][3];
}

vr::VRActionHandle_t VR::ActiveHandle(const Action &action) const
{
    return m_ActiveSet >= 0 ? action.handle[m_ActiveSet] : vr::k_ulInvalidActionHandle;
}

// Where the walk stick's "forward" points: the head or the walk stick's hand. GetPoses has
// swapped the hands if LeftHanded: "right" is the hand with the gun.
float VR::WalkYaw() const
{
    if (m_HeadOrientedWalking)
        return m_HmdAngAbs.y;
    return m_WalkWithRightStick != m_LeftHanded ? m_RightControllerAngAbs.y : m_LeftControllerAngAbs.y;
}

bool VR::CheckDigitalActionChanged(const Action &action, bool &state)
{
    vr::InputDigitalActionData_t digitalActionData;
    vr::EVRInputError result = m_Input->GetDigitalActionData(ActiveHandle(action), &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
    {
        state = digitalActionData.bState;
        return digitalActionData.bChanged;
    }

    state = false;
    return false;
}

bool VR::GetAnalogActionData(const Action &action, vr::InputAnalogActionData_t &analogDataOut)
{
    vr::EVRInputError result = m_Input->GetAnalogActionData(ActiveHandle(action), &analogDataOut, sizeof(analogDataOut), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
        return true;

    return false;
}

void VR::ProcessMenuInput()
{
    // Here, because inputsystem.dll has its window procedure in place by the time there is a menu.
    DXGI_SWAP_CHAIN_DESC swapDesc;
    const HWND window = D3D11Hooks::g_SwapChain && SUCCEEDED(D3D11Hooks::g_SwapChain->GetDesc(&swapDesc))
        ? swapDesc.OutputWindow : nullptr;
    if (window && !g_GameWindowProc)
    {
        s_MenuPartWidth = m_RenderWidth;
        s_MenuPartHeight = MenuHeight();
        g_GameWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuMouseWindowProc)));
        std::cout << "[VR] window mouse scaled to the menu: " << (g_GameWindowProc ? "yes" : "no") << "\n";
    }
    const WPARAM laserMark = g_GameWindowProc ? kMouseFromLaser : 0;

    // Check if left or right hand controller is pointing at the overlay
    const bool isHoveringOverlay = CheckOverlayIntersectionForController(m_MainMenuHandle, vr::TrackedControllerRole_LeftHand) ||
                                   CheckOverlayIntersectionForController(m_MainMenuHandle, vr::TrackedControllerRole_RightHand);

    static int lastHovering = -1;
    if ((int)isHoveringOverlay != lastHovering)
    {
        if (VerboseLog())
            std::cout << "[VR] controller pointing at the menu overlay: " << isHoveringOverlay << "\n";
        lastHovering = isHoveringOverlay;
    }

    // Overlays can't process action inputs if the laser is active, so
    // only activate laser if a controller is pointing at the overlay
    if (isHoveringOverlay)
    {
        vr::VROverlay()->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

        // Laser position on the overlay (back buffer pixels, origin bottom left) -> mouse message
        // coordinates. Panorama lays the menu out over a part of the back buffer, not the window
        // size the engine uses, and takes the message coordinates as back buffer pixels:
        // window coordinates landed in the top left corner of the menu (also with a real mouse).
        auto toWindow = [&](const vr::VREvent_Mouse_t &mouse, int &x, int &y)
        {
            x = (int)mouse.x;
            y = (int)(MenuHeight() - mouse.y);
        };

        // The menu is Panorama; it gets the mouse from inputsystem.dll, which subclasses the game
        // window. So mouse messages go straight to that window (SendInput hit whatever was under
        // the real cursor, and only if the game had the focus).
        // Moves with the button held carry MK_LBUTTON: without it, Panorama doesn't drag sliders.
        static WPARAM buttons = 0;
        vr::VREvent_t vrEvent;
        while (vr::VROverlay()->PollNextOverlayEvent(m_MainMenuHandle, &vrEvent, sizeof(vrEvent)))
        {
            int x, y;
            switch (vrEvent.eventType)
            {
            case vr::VREvent_MouseMove:
                toWindow(vrEvent.data.mouse, x, y);
                if (window)
                    PostMessage(window, WM_MOUSEMOVE, buttons | laserMark, MAKELPARAM(x, y));
                break;

            case vr::VREvent_MouseButtonDown:
            {
                toWindow(vrEvent.data.mouse, x, y);
                if (VerboseLog())
                {
                    POINT cursor = {};
                    GetCursorPos(&cursor);
                    if (window)
                        ScreenToClient(window, &cursor);
                    std::cout << "[VR] menu click at overlay " << vrEvent.data.mouse.x << "," << vrEvent.data.mouse.y
                              << " -> message " << x << "," << y << " (cursor " << cursor.x << "," << cursor.y
                              << ", game window " << (window ? "found" : "NOT found")
                              << (window && GetForegroundWindow() == window ? ", foreground" : ", NOT foreground") << ")\n";
                }
                if (window)
                {
                    PostMessage(window, WM_MOUSEMOVE, laserMark, MAKELPARAM(x, y));
                    PostMessage(window, WM_LBUTTONDOWN, MK_LBUTTON | laserMark, MAKELPARAM(x, y));
                }
                buttons = MK_LBUTTON;
                break;
            }

            case vr::VREvent_MouseButtonUp:
                toWindow(vrEvent.data.mouse, x, y);
                if (window)
                    PostMessage(window, WM_LBUTTONUP, laserMark, MAKELPARAM(x, y));
                buttons = 0;
                break;

            case vr::VREvent_ScrollDiscrete:
            {
                // Panorama, like the mouse above: a wheel message (120 per notch) to the window.
                const int delta = (int)(vrEvent.data.scroll.ydelta * WHEEL_DELTA);
                static int logged = 0;
                if (logged < 10)
                {
                    std::cout << "[VR] menu scroll " << vrEvent.data.scroll.ydelta << " -> wheel " << delta << "\n";
                    ++logged;
                }
                if (window && delta)
                    PostMessage(window, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)delta), 0);
                break;
            }
            }
        }
    }
    else
    {
        vr::VROverlay()->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

        bool state;
        if (CheckDigitalActionChanged(m_MenuSelect, state) && state)
        {
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_RETURN;
            SendInput(1, &input, sizeof(INPUT));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));
        }
        if ((CheckDigitalActionChanged(m_MenuBack, state) && state) || (CheckDigitalActionChanged(m_Pause, state) && state))
        {
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_ESCAPE;
            SendInput(1, &input, sizeof(INPUT));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));
        }
        if (CheckDigitalActionChanged(m_MenuUp, state) && state)
        {
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_UP;
            SendInput(1, &input, sizeof(INPUT));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));
        }
        if (CheckDigitalActionChanged(m_MenuDown, state) && state)
        {
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_DOWN;
            SendInput(1, &input, sizeof(INPUT));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));
        }
        if (CheckDigitalActionChanged(m_MenuLeft, state) && state)
        {
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_LEFT;
            SendInput(1, &input, sizeof(INPUT));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));
        }
        if (CheckDigitalActionChanged(m_MenuRight, state) && state)
        {
            INPUT input {};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = VK_RIGHT;
            SendInput(1, &input, sizeof(INPUT));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(INPUT));
        }
    }
}

void VR::ProcessInput()
{
    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    duration elapsed = currentTime - m_PrevFrameTime;
    float deltaTime = elapsed.count();
    m_PrevFrameTime = currentTime;

    vr::InputAnalogActionData_t analogActionData;

    m_SmoothTurnRate = 0;
    if (GetAnalogActionData(m_ActionTurn, analogActionData))
    {
        if (m_SnapTurning)
        {
            if (!m_PressedTurn && analogActionData.x > 0.5)
            {
                m_RotationOffset.y -= m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (!m_PressedTurn && analogActionData.x < -0.5)
            {
                m_RotationOffset.y += m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (analogActionData.x < 0.3 && analogActionData.x > -0.3)
                m_PressedTurn = false;
        }
        // Smooth turning
        else
        {
            float deadzone = 0.2f;
            // smoother turning
            float xNormalized = (abs(analogActionData.x) - deadzone) / (1 - deadzone);
            if (abs(analogActionData.x) > deadzone)
                m_SmoothTurnRate = m_TurnSpeed * 1000 * xNormalized;
            if (analogActionData.x > deadzone)
            {
                m_RotationOffset.y -= m_TurnSpeed * deltaTime * xNormalized;
            }
            if (analogActionData.x < -deadzone)
            {
                m_RotationOffset.y += m_TurnSpeed * deltaTime * xNormalized;
            }
        }

        // Wrap from 0 to 360
        m_RotationOffset.y -= 360 * std::floor(m_RotationOffset.y / 360);
    }

    // Re-align camera upright after portalling
    if (m_RotationOffset.x != 0.f || m_RotationOffset.z != 0.f)
    {
        // Last valid yaw angle, in case we need to revert to it.
        const float lastYaw = m_RotationOffset.y;

        const QAngle targetRotation(0, lastYaw, 0);

        const auto lerp = [](float a, float b, float f) -> float {
            return a * (1.0f - f) + (b * f);
        };

        // After the recovery time 1% of the tilt is left, whatever the frame rate.
        const float share = m_CameraUprightRecoveryTime > 0 ? 1.0f - std::pow(0.01f, deltaTime / 1000 / m_CameraUprightRecoveryTime) : 1.0f;

        float lerpedPitch = lerp(m_RotationOffset.x, targetRotation.x, share);
        float lerpedYaw = lerp(m_RotationOffset.y, targetRotation.y, share);
        float lerpedRoll = lerp(m_RotationOffset.z, targetRotation.z, share);

        m_RotationOffset = QAngle(lerpedPitch, lerpedYaw, lerpedRoll);

        if (abs(m_RotationOffset.x) < 0.0001)
            m_RotationOffset.x = 0;
        if (abs(m_RotationOffset.z) < 0.0001)
            m_RotationOffset.z = 0;

        // Just in case any calculation went awry, revert to an upright vector:
        if (std::isnan(m_RotationOffset.x) || std::isnan(m_RotationOffset.y) || std::isnan(m_RotationOffset.z))
        {
            m_RotationOffset = QAngle(0.f, lastYaw, 0.f);
        }
    }

    bool state;
    if (CheckDigitalActionChanged(m_ActionPrimaryAttack, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+attack" : "-attack");
    }

    if (CheckDigitalActionChanged(m_ActionSecondaryAttack, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+attack2" : "-attack2");
    }

    if (CheckDigitalActionChanged(m_ActionJump, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+jump" : "-jump");
    }

    if (CheckDigitalActionChanged(m_ActionCrouch, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+duck" : "-duck");
    }

    if (CheckDigitalActionChanged(m_ActionUse, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+use" : "-use");
    }

    if (CheckDigitalActionChanged(m_ActionResetPosition, state) && state)
    {
        ResetPosition();
    }

    if (CheckDigitalActionChanged(m_Pause, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays();
    }
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String,
                                                 buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = {0};
        vr::RenderModel_ComponentState_t componentState = {0};

        const bool found = vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState);
        static bool logged[2] = { false, false };
        const int hand = controllerRole == vr::TrackedControllerRole_LeftHand ? 0 : 1;
        if (!logged[hand])
        {
            std::cout << "[VR] render model '" << buffer << "': tip component " << (found ? "found" : "NOT found") << "\n";
            logged[hand] = true;
        }
        if (found)
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

// Rigid poses: 'to' relative to 'from' (inverse(from) * to), and a * b.
static vr::HmdMatrix34_t RelativePose(const vr::HmdMatrix34_t &from, const vr::HmdMatrix34_t &to)
{
    vr::HmdMatrix34_t r = {};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r.m[i][j] += from.m[k][i] * to.m[k][j];
        for (int k = 0; k < 3; ++k)
            r.m[i][3] += from.m[k][i] * (to.m[k][3] - from.m[k][3]);
    }
    return r;
}

static vr::HmdMatrix34_t ComposePose(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b)
{
    vr::HmdMatrix34_t r = {};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 3; ++k)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
        r.m[i][3] += a.m[i][3];
    }
    return r;
}

// SteamVR's laser ray is the tip pose (/user/hand/*/pose/tip, pointer actions). Once the menu
// overlay is interactive, SteamVR suspends the app's input and the action goes inactive, so the
// tip is kept as an offset to the controller pose (which keeps updating) and the ray comes from
// that. Without it the menu hit test flipped every frame (flickering laser, no clicks).
// Returns 0 (controller pose x tip offset), 1 (tip pose action) or -1 (no pose).
int VR::GetPointerPose(vr::ETrackedControllerRole controllerRole, vr::HmdMatrix34_t &pose)
{
    const bool left = controllerRole == vr::TrackedControllerRole_LeftHand;
    vr::InputPoseActionData_t pointer = {};
    const bool havePointer =
        m_Input->GetPoseActionDataForNextFrame(left ? m_ActionPointerLeft : m_ActionPointerRight, vr::VRCompositor()->GetTrackingSpace(),
            &pointer, sizeof(pointer), vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
        pointer.bActive && pointer.pose.bPoseIsValid;

    const vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);
    const vr::TrackedDevicePose_t *controllerPose =
        deviceIndex < vr::k_unMaxTrackedDeviceCount && m_Poses[deviceIndex].bPoseIsValid ? &m_Poses[deviceIndex] : nullptr;

    if (havePointer && controllerPose)
    {
        m_PointerOffset[left] = RelativePose(controllerPose->mDeviceToAbsoluteTracking, pointer.pose.mDeviceToAbsoluteTracking);
        if (!m_HavePointerOffset[left])
        {
            // For the gun, placed at the tip: the controller's origin seen from the tip (x right,
            // y up, -z along the laser, cm) and the laser in the controller's axes. Mirror images
            // on the two hands of a symmetric controller pair.
            const vr::HmdMatrix34_t &o = m_PointerOffset[left];
            float origin[3], laser[3];
            for (int i = 0; i < 3; ++i)
            {
                origin[i] = -100.0f * (o.m[0][i] * o.m[0][3] + o.m[1][i] * o.m[1][3] + o.m[2][i] * o.m[2][3]);
                laser[i] = -o.m[i][2];
            }
            m_TipSideOffset[left] = origin[0] / 100.0f;
            std::cout << "[VR] laser " << (left ? "left" : "right") << ": tip offset learned from the pointer action, controller origin at "
                      << origin[0] << "," << origin[1] << "," << origin[2] << " cm from the tip, laser " << laser[0] << "," << laser[1] << "," << laser[2] << "\n";
        }
        m_HavePointerOffset[left] = true;
    }

    if (controllerPose && m_HavePointerOffset[left])
    {
        pose = ComposePose(controllerPose->mDeviceToAbsoluteTracking, m_PointerOffset[left]);
        return 0;
    }
    if (havePointer)
    {
        pose = pointer.pose.mDeviceToAbsoluteTracking;
        return 1;
    }
    return -1;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    vr::VROverlayIntersectionParams_t  params  = {0};
    vr::VROverlayIntersectionResults_t results = {0};
    params.eOrigin = vr::VRCompositor()->GetTrackingSpace();

    // Fallback before the first pointer pose: the render model's "tip" component (missing e.g. on
    // Quest via Virtual Desktop, then it's the grip pose, which points well below the laser).
    const bool left = controllerRole == vr::TrackedControllerRole_LeftHand;
    vr::HmdMatrix34_t ray;
    int source = GetPointerPose(controllerRole, ray);
    if (source < 0)
    {
        const vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);
        if (deviceIndex >= vr::k_unMaxTrackedDeviceCount || !m_Poses[deviceIndex].bPoseIsValid)
            return false;
        ray = ComposePose(m_Poses[deviceIndex].mDeviceToAbsoluteTracking, GetControllerTipMatrix(controllerRole));
        source = 2;
    }

    static int lastSource[2] = { -1, -1 };
    if (lastSource[left] != source)
    {
        static const char *const names[] = { "controller pose x tip offset", "tip pose action", "fallback (render model tip)" };
        std::cout << "[VR] menu laser " << (left ? "left" : "right") << ": " << names[source] << "\n";
        lastSource[left] = source;
    }

    params.vSource    = { ray.m[0][3], ray.m[1][3], ray.m[2][3] };
    params.vDirection = { -ray.m[0][2], -ray.m[1][2], -ray.m[2][2] };
    return m_Overlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}
