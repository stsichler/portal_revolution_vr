//========= Copyright Valve Corporation, All rights reserved. ============//

#pragma once

#include <math.h>
#include "usercmd.h"
#include "worldsize.h"

#define IN_ATTACK		(1 << 0)
#define IN_JUMP			(1 << 1)
#define IN_DUCK			(1 << 2)
#define IN_FORWARD		(1 << 3)
#define IN_BACK			(1 << 4)
#define IN_USE			(1 << 5)
#define IN_CANCEL		(1 << 6)
#define IN_LEFT			(1 << 7)
#define IN_RIGHT		(1 << 8)
#define IN_MOVELEFT		(1 << 9)
#define IN_MOVERIGHT	(1 << 10)
#define IN_ATTACK2		(1 << 11)
#define IN_RUN			(1 << 12)
#define IN_RELOAD		(1 << 13)
#define IN_ALT1			(1 << 14)
#define IN_ALT2			(1 << 15)
#define IN_SCORE		(1 << 16)   // Used by client.dll for when scoreboard is held down
#define IN_SPEED		(1 << 17)	// Player is holding the speed key
#define IN_WALK			(1 << 18)	// Player holding walk key
#define IN_ZOOM			(1 << 19)	// Zoom key for HUD zoom
#define IN_WEAPON1		(1 << 20)	// weapon defines these bits
#define IN_WEAPON2		(1 << 21)	// weapon defines these bits
#define IN_BULLRUSH		(1 << 22)
#define IN_GRENADE1		(1 << 23)	// grenade 1
#define IN_GRENADE2		(1 << 24)	// grenade 2
#define	IN_LOOKSPIN		(1 << 25)

#define MAX_LINEAR_SPEED 175

enum ClearFlags_t
{
	VIEW_CLEAR_COLOR = 0x1,
	VIEW_CLEAR_DEPTH = 0x2,
	VIEW_CLEAR_FULL_TARGET = 0x4,
	VIEW_NO_DRAW = 0x8,
	VIEW_CLEAR_OBEY_STENCIL = 0x10, // Draws a quad allowing stencil test to clear through portals
	VIEW_CLEAR_STENCIL = 0x20,
};

enum RenderViewInfo_t
{
	RENDERVIEW_UNSPECIFIED = 0,
	RENDERVIEW_DRAWVIEWMODEL = (1 << 0),
	RENDERVIEW_DRAWHUD = (1 << 1),
	RENDERVIEW_SUPPRESSMONITORRENDERING = (1 << 2),
};

struct vrect_t
{
	int				x, y, width, height;
	vrect_t* pnext;
};

struct PositionAngle
{
	Vector position;
	QAngle angle;
};

// Calls the virtual function with the given vtable index. Used for the interfaces whose layout
// differs in Strata Source (Portal: Revolution, x64) from the Portal 2 SDK: only methods whose
// index was checked in the game's binaries are declared.
template <typename R, typename... Args>
inline R CallVirtual(const void *object, int index, Args... args)
{
	using Fn = R(*)(const void *, Args...); // x64: one calling convention, 'this' first
	return (*reinterpret_cast<Fn const *const *>(object))[index](object, args...);
}

// VClientEntityList003 of Strata Source (client.dll, the IClientEntityList base of
// CClientEntityList at +0x80028). Checked there: [3] GetClientEntity, [5] NumberOfEntities,
// [6] GetHighestEntityIndex, [8] GetMaxEntities. The entries before [3] only hold its place.
class IClientEntityList
{
public:
	virtual	~IClientEntityList() { }
	virtual void* GetClientNetworkable(int entnum) = 0;
	virtual void* GetClientNetworkableArray(void) = 0;
	virtual void* GetClientEntity(int entnum) = 0;

	int GetHighestEntityIndex() { return CallVirtual<int>(this, 6); }
};

// VEngineVGui001 of Strata Source (engine.dll, vtable of CEngineVGui): [2] is GetPanel(type), [3]
// returns staticGameUIPanel && staticGameUIPanel->IsVisible().
class IEngineVGui
{
public:
	bool IsGameUIVisible() { return CallVirtual<bool>(this, 3); }
};

// VEngineClient015 of Strata Source (engine.dll, vtable of CEngineClient). The vtable starts with
// 37 entries of another base class; the known IVEngineClient methods follow, with changes.
class IEngineClient
{
public:
	void ClientCmd(const char *szCmdString) { CallVirtual<void>(this, 44, szCmdString); }  // Cbuf_AddText(cmd) + "\n"
	int GetLocalPlayer() { return CallVirtual<int>(this, 47); }                            // GetLocalClient().m_nPlayerSlot + 1
	void GetViewAngles(QAngle &angle) { CallVirtual<void>(this, 53, &angle); }
	void SetViewAngles(QAngle &angle) { CallVirtual<void>(this, 54, &angle); }
	bool IsInGame() { return CallVirtual<bool>(this, 61); }                               // signon state == 6
	bool IsConnected() { return CallVirtual<bool>(this, 63); }                            // signon state >= 2
	// Cbuf_AddText(current player, cmd, source) without the ClientCmd restrictions; [128] is the
	// overload with a player slot (MSVC puts overloads in reverse order).
	void ClientCmd_Unrestricted(const char *szCmdString) { CallVirtual<void>(this, 129, szCmdString, false); }
};

enum StereoEye_t
{
	STEREO_EYE_MONO = 0,
	STEREO_EYE_LEFT = 1,
	STEREO_EYE_RIGHT = 2,
	STEREO_EYE_MAX = 3,
};

enum MotionBlurMode_t
{
	MOTION_BLUR_DISABLE = 1,
	MOTION_BLUR_GAME = 2,			// Game uses real-time inter-frame data
	MOTION_BLUR_SFM = 3				// Use SFM data passed in CViewSetup structure
};

// Strata Source (x64) layout, checked in client.dll (CViewRender::Render/RenderView, the inline
// constructor of the HUD view): the fields from fov on are 0x50 further back than in Portal 2,
// the order is the same. Fields after m_flAspectRatio are not used by the mod.
class CViewSetup
{
public:
	int32_t x; //0x0000
	int32_t m_nUnscaledX; //0x0004
	int32_t y; //0x0008
	int32_t m_nUnscaledY; //0x000C
	int32_t width; //0x0010
	int32_t m_nUnscaledWidth; //0x0014
	int32_t height; //0x0018
	int32_t m_nUnscaledHeight; //0x001C
	char pad_0020[152]; //0x0020
	float fov; //0x00B8
	float fovViewmodel; //0x00BC
	Vector origin; //0x00C0
	Vector angles; //0x00CC
	float zNear; //0x00D8
	float zFar; //0x00DC
	float zNearViewmodel; //0x00E0
	float zFarViewmodel; //0x00E4
	float m_flAspectRatio; //0x00E8
	float m_flNearBlurDepth; //0x00EC
	float m_flNearFocusDepth; //0x00F0
	float m_flFarFocusDepth; //0x00F4
	float m_flFarBlurDepth; //0x00F8
	float m_flNearBlurRadius; //0x00FC
	float m_flFarBlurRadius; //0x0100
	int m_nDoFQuality; //0x0104
	MotionBlurMode_t m_nMotionBlurMode; //0x0108
	char pad_010c[68]; //0x010C
	int32_t m_EdgeBlur; //0x0150
	char pad_0154[4]; //0x0154
}; //Size: 0x0158 (stride of CViewRender's per-player views)
static_assert(sizeof(CViewSetup) == 0x158);
static_assert(offsetof(CViewSetup, fov) == 0xB8 && offsetof(CViewSetup, origin) == 0xC0 && offsetof(CViewSetup, m_flAspectRatio) == 0xE8);

// VClient018 of Strata Source (client.dll), only used as a pointer: strata_hooks.cpp calls [8]
// GetAllClasses. The declarations of the Source SDK 2013 had it at [9].
class IBaseClientDLL
{
};

// vgui2 VGUI_InputInternal001 (vtable of CInputWin32 in vgui2.dll), checked in Strata Source.
class IInput
{
public:
	void SetCursorPos(int x, int y) { CallVirtual<void>(this, 7, x, y); }
	void InternalMouseWheeled(int delta) { CallVirtual<void>(this, 73, delta); }  // between InternalCursorMoved [69] and InternalKeyTyped [76]
};

// VGUI_Surface031 (vtable of CMatSystemSurface in vguimatsurface.dll), checked in Strata Source.
class ISurface
{
public:
	bool IsCursorVisible() { return CallVirtual<bool>(this, 59); }  // m_cursorAlwaysVisible || cursor != dc_none
};
