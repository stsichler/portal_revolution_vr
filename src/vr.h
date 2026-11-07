#pragma once
#include "openvr.h"
#include "vector.h"
#include <chrono>

#define MAX_STR_LEN 256

class Game;
struct ID3D11Texture2D;


struct TrackedDevicePoseData 
{
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

// A D3D11 texture handed to OpenVR (vr::TextureType_DirectX).
struct SharedTextureHolder 
{
	ID3D11Texture2D *m_Texture = nullptr;
	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	vr::Texture_t m_VRTexture = { nullptr, vr::TextureType_DirectX, vr::ColorSpace_Auto };
};

class VR
{
public:
	Game *m_Game = nullptr;

	vr::IVRSystem *m_System = nullptr;
	vr::IVRInput *m_Input = nullptr;
	vr::IVROverlay *m_Overlay = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle;
	vr::VROverlayHandle_t m_AimDotHandle = vr::k_ulOverlayHandleInvalid;
	vr::VROverlayHandle_t m_VignetteHandle = vr::k_ulOverlayHandleInvalid;
	//vr::VROverlayHandle_t m_HUDHandle;

	float m_HorizontalOffsetLeft;
	float m_VerticalOffsetLeft;
	float m_HorizontalOffsetRight;
	float m_VerticalOffsetRight;

	uint32_t m_RenderWidth;
	uint32_t m_RenderHeight;
	uint32_t MenuHeight() const; // of the part of the back buffer Panorama lays the menu out for
	float m_Aspect;
	float m_Fov;

	vr::VRTextureBounds_t m_TextureBounds[2];
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount];

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward;
	Vector m_HmdRight;
	Vector m_HmdUp;

	Vector m_LeftControllerForward;
	Vector m_LeftControllerRight;
	Vector m_LeftControllerUp;

	Vector m_RightControllerForward;
	Vector m_RightControllerRight;
	Vector m_RightControllerUp;

	Vector m_ViewmodelForward;
	Vector m_ViewmodelRight;
	Vector m_ViewmodelUp;

	QAngle m_HmdAngAbs;

	Vector m_HmdPosRelativeRaw = { 0,0,0 };
	Vector m_HmdPosRelativeRawPrev = { 0,0,0 };

	Vector m_HmdPosRelative = { 0,0,0 };
	Vector m_HmdPosRelativePrev = { 0,0,0 };

	bool m_Traced = false;

	Vector m_Center = { 0,0,0 };
	Vector m_SetupOrigin = { 0,0,0 };

	float m_HeightOffset = 0.0;

	Vector m_LeftControllerPosAbs;											
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosRel;											
	QAngle m_RightControllerAngAbs;

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;

	Vector m_ViewmodelPosCustomOffset; // Custom (from config) viewmodel position offset applied on top of hardcoded ones
    QAngle m_ViewmodelAngCustomOffset; // Custom (from config) viewmodel angle offset applied on top of hardcoded ones
    bool m_ViewmodelCarrying = false; // The portal gun holds an object, set by the Strata hooks
    float m_ViewmodelCarryWeight = 0.0f; // 0 = pose of the idle animation, 1 = of the carrying one
    std::chrono::steady_clock::time_point m_ViewmodelCarryTime;

	float m_Ipd;																	
	float m_EyeZ;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	SharedTextureHolder m_VKLeftEye;
	SharedTextureHolder m_VKRightEye;
	SharedTextureHolder m_VKBackBuffer; // copy of the game's back buffer (menu overlay)
	SharedTextureHolder m_VKBlankTexture;

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	bool m_RenderedNewFrame = false;
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;

	bool m_PressedTurn = false;
	bool m_PointerCreated = false;

	// action set
	vr::VRActionSetHandle_t m_ActionSet;
	vr::VRActiveActionSet_t m_ActiveActionSet;

	// actions
	vr::VRActionHandle_t m_ActionJump;
	vr::VRActionHandle_t m_ActionPrimaryAttack;
	vr::VRActionHandle_t m_ActionSecondaryAttack;
	vr::VRActionHandle_t m_ActionReload;
	vr::VRActionHandle_t m_ActionWalk;
	vr::VRActionHandle_t m_ActionTurn;
	vr::VRActionHandle_t m_ActionUse;
	vr::VRActionHandle_t m_ActionNextItem;
	vr::VRActionHandle_t m_ActionPrevItem;
	vr::VRActionHandle_t m_ActionResetPosition;
	vr::VRActionHandle_t m_ActionCrouch;
	vr::VRActionHandle_t m_ActionFlashlight;
	vr::VRActionHandle_t m_ActionActivateVR;
	vr::VRActionHandle_t m_ActionQuickPing;
	vr::VRActionHandle_t m_ActionRemoteView;
	vr::VRActionHandle_t m_MenuSelect;
	vr::VRActionHandle_t m_MenuBack;
	vr::VRActionHandle_t m_MenuUp;
	vr::VRActionHandle_t m_MenuDown;
	vr::VRActionHandle_t m_MenuLeft;
	vr::VRActionHandle_t m_MenuRight;
	vr::VRActionHandle_t m_Spray; 
	vr::VRActionHandle_t m_Scoreboard;
	vr::VRActionHandle_t m_ShowHUD;
	vr::VRActionHandle_t m_Pause;
	vr::VRActionHandle_t m_ThirdAttack;
	vr::VRActionHandle_t m_ActionPointerLeft;  // pose at /user/hand/left/pose/tip (menu laser)
	vr::VRActionHandle_t m_ActionPointerRight;
	// Tip pose relative to the controller pose, per hand (0 = right, 1 = left); learned while the
	// pointer action is active, because the action goes inactive once the overlay takes input.
	vr::HmdMatrix34_t m_PointerOffset[2] = {};
	bool m_HavePointerOffset[2] = { false, false };
	bool m_AimFromTip[2] = { false, false }; // hand pose is the tip pose (0 = right, 1 = left)

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	QAngle m_RotationOffset = { 0, 0, 0 };
	bool m_OverrideEyeAngles = false;
	std::chrono::steady_clock::time_point m_PrevFrameTime;
	bool m_InitialPosReset = false;

	float m_TurnSpeed = 0.15;
	bool m_SnapTurning = true;
	float m_SnapTurnAngle = 30.0;
	bool m_LeftHanded = false;
	float m_VRScale = 43.2;
	float m_IpdScale = 1.0;
	bool m_6DOF = true;
	bool m_SeatedMode = false;
	float m_SeatedHeightOffset = 4.0;
	bool m_Roomscale = true;
	float m_HudDistance = 1.3;
	float m_HudSize = 4.0;
	bool m_HudAlwaysVisible = false;
	int m_AimMode = 1;
	bool m_ApplyPitchAndRollPortalRotationOffset = true; // If `true`, the camera pitch/roll follows the exit portal's orientation when portalling
	float m_CameraUprightRecoverySpeed = 0.2f; // If the above is `true`, this controls how quickly the camera turns back upright after portalling
	float m_VignetteWalking = 0.0f; // 0 = off, 1 = black
	float m_VignetteTurning = 0.8f; // 0 = off, 1 = black
	float m_VignetteAfterPortal = 1.0f; // 0 = off, 1 = black
	float m_VignetteClearAngle = 20.0f; // degrees from the center of the view that stay clear

	float m_SmoothTurnRate = 0; // degrees per second the stick turns right now, 0 with snap turning
	float m_VignetteMoving = 0; // current alpha from walking and turning, fades in and out
	float m_VignetteAlpha = 0; // shown right now, 0 = hidden
	float m_VignettePeak = 0; // highest alpha since it was shown
	float m_VignetteTanHalfWidth = 1; // of the overlay, seen from the head
	float m_VignetteDrawnAngle = -1; // the clear angle of its texture
	std::chrono::steady_clock::time_point m_VignetteTime;
	std::chrono::steady_clock::time_point m_VignetteShownAt;

	VR(Game *game);
	void initD3D11Callbacks();
	bool InitOpenVR(); // VR_Init, done before the game creates its swap chain
	void CopyBackBuffer(struct IDXGISwapChain *swapChain);
	void PrepareWindowImage(struct IDXGISwapChain *swapChain);
	void CopyBackBufferToEye(SharedTextureHolder &eye);
	void GetWindowSize(int &width, int &height);
	void CreateBlankTexture();
	bool IsMenuVisible();
	void init();
	int SetActionManifest(const char *fileName);
	void InstallApplicationManifest(const char *fileName);
	void Update();
	void SetScreenSizeOverride(bool bState);
	void CreateVRTextures();
	void SubmitVRTextures();
	void RepositionOverlays();
	void CreateAimDot();
	void ShowAimDot(const Vector &aim, const Vector &viewOrigin);
	void HideAimDot();
	void CreateVignette();
	void DrawVignette(float clearAngle);
	void UpdateVignette(float walkedDistance);
	void HideVignette();
	void GetPoses();
	void UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void ProcessInput();
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	int GetPointerPose(vr::ETrackedControllerRole controllerRole, vr::HmdMatrix34_t &pose);
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetRightControllerAbsAngle();
	QAngle& GetRightControllerAbsAngleConst();
	Vector GetRightControllerAbsPos(Vector eyePosition = {0, 0, 0});
	Vector GetRecommendedViewmodelAbsPos(Vector eyePosition);
	QAngle GetRecommendedViewmodelAbsAngle();
	void UpdateHMDAngles();
	void UpdateHeadTracking();
	void UpdateHeadPose();
	void UpdateHandTracking();
	Vector GetViewAngle();
	Vector GetViewOrigin(Vector setupOrigin);
	Vector GetViewOriginLeft(Vector setupOrigin);
	Vector GetViewOriginRight(Vector setupOrigin);
	bool CheckDigitalActionChanged(vr::VRActionHandle_t& actionHandle, bool& state);
	bool GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut);
	void ResetPosition();
	void MoveRoom(const Vector &step);
	void GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut);
	void ParseConfigFile();
	void WaitForConfigUpdate();
};