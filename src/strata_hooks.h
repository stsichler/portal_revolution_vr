#pragma once
#include <chrono>
#include <cstdint>
#include <vector>
#include "arms.h"
#include "vector.h"

class Game;

// Engine hooks for Strata Source (Portal: Revolution, x64). Functions are found through RTTI
// (class name -> vtable) and vtable indices checked in the game's DLLs instead of byte signatures.
namespace StrataHooks
{
    // Returns the vtable of the class with this name in the module, or nullptr. With multiple
    // inheritance, offset selects the one of the base class at that offset in the object.
    void **FindVTable(const char *module, const char *className, uint32_t offset = 0);

    // The number of entries: pointers into the module's code, up to the first that isn't one.
    int VTableLength(void *const *vtable, const char *module);

    // FindVTable, but nullptr (and a line in the log) if the vtable doesn't have the length it had
    // in the game version the indices were checked with: after a game update with added or
    // removed entries the indices would point to other functions.
    void **FindCheckedVTable(const char *module, const char *className, int length, uint32_t offset = 0);

    bool Init(Game *game);

    // Thread function, to start before the game loads panorama.dll: the hooks must be in place
    // when Panorama's windows are created.
    unsigned long __stdcall HookPanorama(void *);

    // A full screen video (VGUI VideoPanel) is on the screen.
    bool VideoVisible();

    // The height of the part of the back buffer Panorama lays out for: in the aspect ratio of the
    // video mode, or all of it if the hooks aren't in place. Changes with the video settings.
    int PanoramaHeight(int width, int height);

    // The game's gamepad rumble of the last frame: the stronger motor (0..1, 0 if none), and
    // whether it comes only from the portal gun. False while there is no player (no rumble then).
    bool Rumble(float &amplitude, bool &gunOnly);
}

// Shared by the parts of the hooks (strata_*.cpp), not for other files. Init calls each part's
// Install function.
namespace StrataHooks
{
    // strata_hooks.cpp: the views, the HUD, the network fields, Init.
    extern Game *g_StrataGame;
    extern void *g_ViewModelOwner; // the player, as last seen by hkCalcViewModelView
    // The eyes are rendered and the viewmodel is in the hand.
    bool VRViewActive();
    // MinHook: creates and enables the hook, with a line in the log.
    bool InstallHookAt(const char *name, void *target, void *detour, void **original, const char *module);
    template <typename F>
    bool InstallHook(const char *name, void *target, F detour, F &original, const char *module = "client.dll")
    {
        return InstallHookAt(name, target, reinterpret_cast<void *>(detour), reinterpret_cast<void **>(&original), module);
    }

    // C_Prop_Portal: the matrix to the linked portal; network fields (FindNetworkFields).
    constexpr int kPortal_MatrixToLinked = 0x3188;
    extern int g_Portal_Origin;
    extern int g_Portal_Angles;
    extern int g_Portal_LinkedPortal;
    extern int g_Portal_Activated;
    extern int g_Portal_HalfWidth;
    extern int g_Portal_HalfHeight;
    extern void **g_PropPortalVTable;
    // C_Portal_Player: the view is on the other side of a portal than the player, through the portal
    // named here. Set by [387] for its own eye; the player and its copies in portals are drawn by it.
    constexpr int kPlayer_ViewThroughPortal = 0x3aaa;
    constexpr int kPlayer_ViewExitPortal = 0x3ab0;
    extern void **g_PortalPlayerVTable;
    extern bool g_GameEyeThrough;
    extern const uint8_t *g_GameEyeExit;

    // strata_vtables.cpp: finding code in the game's DLLs.
    // The instruction `lea <register>, [rip + text]`; modrm selects the register (0x0D rcx, 0x15 rdx).
    uint8_t *FindStringLea(uint8_t *base, const char *text, uint8_t modrm);
    // The target of the `call rel32` at this address, nullptr if there is none.
    uint8_t *CallTarget(uint8_t *call);

    // strata_movement.cpp: walking, roomscale, turning with portals.
    struct PortalPassage
    {
        bool waiting = false;
        int frames = 0;
        float matrix[3][4] = {};
    };
    extern PortalPassage g_PortalPassage;
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
    };
    extern RoomscaleState g_Roomscale;
    extern int g_Player_Flags;
    Vector RotatedByPortal(const float (*m)[4], const Vector &v);
    Vector MovedByPortal(const float (*m)[4], const Vector &v);
    QAngle TurnedByPortal(const float (*m)[4], const QAngle &angles);
    float Heading(const Vector &forward, const Vector &up);
    void FollowPortalPassage(const Vector &position, const Vector &lastPosition);
    const uint8_t *FindPortalBetween(const Vector &from, const Vector &to);
    void UpdateRoomscale(const Vector &eyes);
    float WalkedDistance(const Vector &position, const Vector &last);
    bool InstallMovementHooks(bool &ok);
    void InstallPortalTeleportationHook(void **player, bool &ok);

    // strata_gun.cpp: the gun in the hand, its muzzle, the shot, the aim and the laser.
    using tCalcViewModelView = void (*)(void *thisptr, void *owner, const Vector &eyePosition, const QAngle &eyeAngles);
    extern tCalcViewModelView oCalcViewModelView;
    extern Vector g_EyePosition; // of the player in the game, as last seen by hkCalcViewModelView
    extern Vector g_Muzzle;
    extern bool g_MuzzleValid;
    extern bool g_ShotFromEyes;
    extern Vector g_Aim;
    extern bool g_AimValid;
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
    };
    extern GunPlacement g_Gun;
    constexpr int kAimMode_None = 0;
    constexpr int kAimMode_Dot = 1;
    constexpr int kAimMode_Laser = 2;
    extern int g_Player_ActiveWeaponHandle;
    extern int g_Player_HoldingSomething;
    extern int g_Portalgun_CanFirePortal1;
    extern int g_Portalgun_CanFirePortal2;
    extern int g_Portalgun_LastFiredPortal;
    extern int g_Portalgun_OpenProngs;
    using tTraceFilterConstructor = void (*)(void *filter, void *passEntity, int collisionGroup);
    using tTraceFilterList = void (*)(void *filter);
    extern tTraceFilterConstructor TraceFilterConstructor;
    extern tTraceFilterList TraceFilterList;
    struct ShotTrace
    {
        Vector end;
        float fraction;
    };
    ShotTrace TraceShot(void *gun, const Vector &start, const Vector &delta);
    void RemoveLaser();
    bool GunCovers(const Vector &eye, const Vector &point);
    bool InstallViewModelHooks(bool &ok);
    bool InstallShotHooks(Game *game, bool &ok);

    // strata_buttons.cpp: pressing buttons with the hands and the gun.
    struct ButtonPress
    {
        std::vector<int> touching; // the button entities a hand or the gun is in
        bool pending = false; // for the next command
        Vector target; // the button's center
    };
    extern ButtonPress g_ButtonPress;
    extern int g_Entity_ModelIndex;
    extern int g_Entity_Mins;
    extern int g_Entity_Maxs;
    void UpdateButtonPress();
    void FindButtons(Game *game);

    // strata_arms.cpp: Chell's arms and her scale.
    extern std::vector<matrix3x4_t> g_EyeBones;
    extern std::chrono::steady_clock::time_point g_EyeBonesTime;
    void HandFrames(Arms::Frame &gunHand, Arms::Frame &otherHand);
    void UpdateArmsHooks();
    void InstallArmsHooks(void **player);

    // strata_menu.cpp: video panel, Panorama's menu size, the hints' buttons, rumble.
    void InstallVideoPanelHook();
    void InstallRumbleHooks();
    void InstallHintHooks();
}
