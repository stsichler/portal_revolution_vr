#include "vr.h"
#include <Windows.h>
#include <windowsx.h>
#include "sdk.h"
#include "game.h"
#include "trace.h"
#include "strata_hooks.h"
#include <dxgi1_3.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <thread>
#include <type_traits>
#include <algorithm>
#include <atomic>
#include <vector>
#include "d3d11_hooks.h"

// The window shows the menu's part of the back buffer scaled to its size, but Panorama takes mouse
// coordinates as pixels of that part. So mouse messages of the window get scaled in the menu.
// The laser's messages already are in those pixels and carry a mark instead. This goes in front of
// the window procedure of inputsystem.dll, which passes the mouse on to Panorama.
namespace
{
    constexpr WPARAM kMouseFromLaser = 0x80000000; // not used by the MK_ flags
    WNDPROC g_GameWindowProc = nullptr;
    int g_MenuPartWidth = 0;
    int g_MenuPartHeight = 0;
    std::atomic<bool> g_MenuVisible{false};

    LRESULT CALLBACK MenuMouseWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        // The wheel messages have screen coordinates.
        if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && message != WM_MOUSEWHEEL && message != WM_MOUSEHWHEEL)
        {
            RECT client;
            if (wParam & kMouseFromLaser)
                wParam &= ~kMouseFromLaser;
            else if (g_MenuVisible && GetClientRect(window, &client) && client.right > 0 && client.bottom > 0)
                lParam = MAKELPARAM(GET_X_LPARAM(lParam) * g_MenuPartWidth / client.right, GET_Y_LPARAM(lParam) * g_MenuPartHeight / client.bottom);
        }
        return CallWindowProcW(g_GameWindowProc, window, message, wParam, lParam);
    }
}

VR::VR(Game *game) 
{
    m_Game = game;
}

void VR::init()
{
    InitOpenVR(); // loads the mod's openvr_api.dll

    char errorString[MAX_STR_LEN];

    vr::HmdError error = vr::VRInitError_None;
    m_System = vr::VR_Init(&error, vr::VRApplication_Scene);

    if (error != vr::VRInitError_None) 
    {
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        return;
    }

    vr::EVRInitError peError = vr::VRInitError_None;

    if (!vr::VRCompositor())
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    m_Input = vr::VRInput();
    m_System = vr::OpenVRInternal_ModuleContext().VRSystem();

    m_System->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    float tanHalfFov[2];

    tanHalfFov[0] = std::max({ -l_left, l_right, -r_left, r_right });
    tanHalfFov[1] = std::max({ -l_top, l_bottom, -r_top, r_bottom });

    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    // The menu overlay shows m_VKBackBuffer, a copy of the back buffer made in Present_Pre_Callback.
    m_Overlay = vr::VROverlay();
    m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
    //m_Overlay->CreateOverlay("HUDOverlayKey", "HUDOverlay", &m_HUDHandle);
    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    //m_Overlay->SetOverlayInputMethod(m_HUDHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    //m_Overlay->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

    const vr::HmdVector2_t mouseScaleMenu = { (float)m_RenderWidth, (float)MenuHeight() };
    m_Overlay->SetOverlayCurvature(m_MainMenuHandle, 0.15f);
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);
    CreateAimDot();
    CreateVignette();

    vr::VRCompositor()->SetTrackingSpace(m_SeatedMode
        ? vr::TrackingUniverseSeated
        : vr::TrackingUniverseStanding);

    UpdatePosesAndActions();

    // In-game VR: stereo rendering (StrataHooks), head tracking, VR input. Controllers, movement
    // and the gameplay hooks are still missing.
    m_IsVREnabled = true;

    m_IsInitialized = true;
    std::cout << "[VR] initialized, menu overlay created\n";
}

// Main menu or pause menu shown. Strata Source runs a background map behind the main menu, so
// IsInGame() is true there too, and the vgui surface reports no cursor; the engine's GameUI panel
// is what tells the menu apart.
bool VR::IsMenuVisible()
{
    if (m_Game->m_EngineVGui)
        return m_Game->m_EngineVGui->IsGameUIVisible();
    return !m_Game->m_EngineClient->IsInGame();
}

// Size of the game window's client area (what the engine renders the menu for).
void VR::GetWindowSize(int &width, int &height)
{
    width = 1280;
    height = 720;
    DXGI_SWAP_CHAIN_DESC desc;
    RECT rect;
    if (D3D11Hooks::g_SwapChain && SUCCEEDED(D3D11Hooks::g_SwapChain->GetDesc(&desc)) &&
        GetClientRect(desc.OutputWindow, &rect) && rect.right > 0 && rect.bottom > 0)
    {
        width = rect.right;
        height = rect.bottom;
    }
}

// Black texture submitted to both eyes while only the overlay is shown. Created directly with
// D3D11 (the material system of Strata Source isn't hooked yet).
void VR::CreateBlankTexture()
{
    if (m_VKBlankTexture.m_Texture || !D3D11Hooks::g_Device)
        return;

    const UINT size = 64;
    std::vector<uint32_t> pixels(size * size, 0xff000000); // opaque black
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = size;
    desc.Height = size;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = { pixels.data(), size * sizeof(uint32_t), 0 };

    if (FAILED(D3D11Hooks::g_Device->CreateTexture2D(&desc, &data, &m_VKBlankTexture.m_Texture)))
    {
        std::cout << "[D3D11] could not create the blank texture\n";
        return;
    }
    m_VKBlankTexture.m_Width = size;
    m_VKBlankTexture.m_Height = size;
    m_VKBlankTexture.m_VRTexture.handle = m_VKBlankTexture.m_Texture;
}

// The game ships its own, older openvr_api.dll in bin\win64, next to this mod. openvr_api.dll is
// delay-loaded, so the mod's copy from <game>\VR\ is loaded before the first OpenVR call.
static void LoadModOpenVR()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    if (GetModuleHandleA("openvr_api.dll"))
    {
        std::cout << "[OpenVR] openvr_api.dll was already loaded by someone else\n";
        return;
    }
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\openvr_api.dll", currentDir);
    HMODULE module = LoadLibraryA(path);
    std::cout << "[OpenVR] " << path << (module ? " loaded" : " NOT found, using the game's openvr_api.dll") << "\n";
}

bool VR::InitOpenVR()
{
    static bool initialized = false;
    if (initialized)
        return true;

    LoadModOpenVR();

    vr::HmdError error = vr::VRInitError_None;
    vr::IVRSystem *system = vr::VR_Init(&error, vr::VRApplication_Scene);
    if (error != vr::VRInitError_None)
    {
        char errorString[MAX_STR_LEN];
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        ExitProcess(0);
    }

    system->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);
    std::cout << "[OpenVR] initialized, render target " << m_RenderWidth << "x" << m_RenderHeight << "\n";
    initialized = true;
    return true;
}

uint32_t VR::MenuHeight() const
{
    return StrataHooks::PanoramaHeight(m_RenderWidth, m_RenderHeight);
}

// The window shows the top part of the back buffer in the menu's format, scaled to its size: the
// menu is there, and in the game the middle of the right eye gets copied there. The back buffer
// isn't used anymore after Present. Without that part the window shows all of it, as the game does.
void VR::PrepareWindowImage(IDXGISwapChain *swapChain)
{
    const UINT height = MenuHeight();
    IDXGISwapChain2 *swapChain2 = nullptr;
    if (height == m_RenderHeight || FAILED(swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void **)&swapChain2)))
        return;

    const HRESULT sized = swapChain2->SetSourceSize(m_RenderWidth, height);
    swapChain2->Release();
    static HRESULT lastSized = S_OK;
    if (sized != lastSized)
    {
        std::cout << "[D3D11] SetSourceSize " << m_RenderWidth << "x" << height << ": 0x" << std::hex << sized << std::dec << "\n";
        lastSized = sized;
    }
    if (FAILED(sized) || !m_RenderedNewFrame || !m_VKRightEye.m_Texture)
        return;

    ID3D11Texture2D *backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
        return;
    const UINT top = (m_RenderHeight - height) / 2;
    const D3D11_BOX middle = { 0, top, 0, m_RenderWidth, top + height, 1 };
    D3D11Hooks::g_Context->CopySubresourceRegion(backBuffer, 0, 0, 0, 0, m_VKRightEye.m_Texture, 0, &middle);
    backBuffer->Release();
}

// Copies the game's back buffer into m_VKBackBuffer, which the menu overlay shows. A copy,
// because back buffers can't be handed to OpenVR (and with the flip model change every frame).
void VR::CopyBackBuffer(IDXGISwapChain *swapChain)
{
    ID3D11Texture2D *backBuffer = nullptr;
    if (!D3D11Hooks::g_Device || FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
        return;

    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc(&desc);

    // Only the part Panorama draws to: OpenVR takes the area the laser can hit from the texture's
    // proportions and ignores the texture bounds. The back buffer is never multisampled (flip model).
    desc.Height = desc.Height < MenuHeight() ? desc.Height : MenuHeight();

    if (!m_VKBackBuffer.m_Texture || m_VKBackBuffer.m_Width != desc.Width || m_VKBackBuffer.m_Height != desc.Height)
    {
        if (m_VKBackBuffer.m_Texture)
            m_VKBackBuffer.m_Texture->Release();
        m_VKBackBuffer = SharedTextureHolder();

        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.SampleDesc = { 1, 0 };
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        if (FAILED(D3D11Hooks::g_Device->CreateTexture2D(&copyDesc, nullptr, &m_VKBackBuffer.m_Texture)))
        {
            std::cout << "[D3D11] could not create the back buffer copy\n";
            backBuffer->Release();
            return;
        }
        m_VKBackBuffer.m_Width = desc.Width;
        m_VKBackBuffer.m_Height = desc.Height;
        m_VKBackBuffer.m_VRTexture.handle = m_VKBackBuffer.m_Texture;
    }

    const D3D11_BOX part = { 0, 0, 0, desc.Width, desc.Height, 1 };
    D3D11Hooks::g_Context->CopySubresourceRegion(m_VKBackBuffer.m_Texture, 0, 0, 0, 0, backBuffer, 0, &part);
    backBuffer->Release();
}

// The multisampled target the game is drawing to, nullptr if it isn't one. Needs Release.
static ID3D11Texture2D *BoundMultisampledTarget()
{
    ID3D11RenderTargetView *view = nullptr;
    D3D11Hooks::g_Context->OMGetRenderTargets(1, &view, nullptr);
    if (!view)
        return nullptr;

    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    view->Release();
    ID3D11Texture2D *texture = nullptr;
    const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture);
    resource->Release();
    if (FAILED(hr))
        return nullptr;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1)
        return texture;
    texture->Release();
    return nullptr;
}

// Copies the finished image of an eye into its D3D11 texture. It's in the back buffer. With the
// game's antialiasing it's in a multisampled target, which the game resolves to the back buffer
// only before Present: the back buffer would still hold the last frame then.
void VR::CopyBackBufferToEye(SharedTextureHolder &eye)
{
    ID3D11Texture2D *backBuffer = nullptr;
    if (!D3D11Hooks::g_SwapChain || FAILED(D3D11Hooks::g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
        return;

    D3D11_TEXTURE2D_DESC source, target;
    backBuffer->GetDesc(&source);
    eye.m_Texture->GetDesc(&target);

    // CopyResource silently does nothing if the sizes differ.
    const bool copyable = source.Width == target.Width && source.Height == target.Height && source.SampleDesc.Count == target.SampleDesc.Count;

    ID3D11Texture2D *multisampled = BoundMultisampledTarget();
    if (multisampled)
    {
        D3D11_TEXTURE2D_DESC desc;
        multisampled->GetDesc(&desc);
        if (desc.Width != target.Width || desc.Height != target.Height || desc.Format != target.Format)
        {
            multisampled->Release();
            multisampled = nullptr;
        }
    }

    static bool logged = false;
    if (!logged)
    {
        std::cout << "[VR] stereo: back buffer " << source.Width << "x" << source.Height << " format " << source.Format << " samples "
                  << source.SampleDesc.Count << " -> eye " << target.Width << "x" << target.Height << " format " << target.Format
                  << " samples " << target.SampleDesc.Count << (multisampled ? ", resolved from the multisampled target" : copyable ? "" : ": can't copy") << "\n";
        logged = true;
    }
    if (multisampled)
    {
        D3D11Hooks::g_Context->ResolveSubresource(eye.m_Texture, 0, multisampled, 0, source.Format);
        multisampled->Release();
    }
    else if (copyable)
        D3D11Hooks::g_Context->CopyResource(eye.m_Texture, backBuffer);
    backBuffer->Release();
}

void VR::initD3D11Callbacks()
{
    // The game renders at the HMD resolution: the back buffer is shown on the menu overlay, the
    // eye textures are created with the same size.
    D3D11Hooks::SwapChainSize_Callback = [this](UINT &width, UINT &height)
        {
            if (!InitOpenVR())
                return;
            width = m_RenderWidth;
            height = m_RenderHeight;
        };

    D3D11Hooks::CreateTexture2D_Post_Callback = [this](ID3D11Texture2D *texture, const D3D11_TEXTURE2D_DESC &desc)
        {
            // CreateNamedRenderTargetTextureEx may also create a depth texture; only the color
            // render target is submitted.
            if (m_CreatingTextureID == VR::Texture_None || !(desc.BindFlags & D3D11_BIND_RENDER_TARGET))
                return;

            SharedTextureHolder *target = nullptr;
            switch (m_CreatingTextureID)
            {
            case VR::Texture_LeftEye: target = &m_VKLeftEye; break;
            case VR::Texture_RightEye: target = &m_VKRightEye; break;
            case VR::Texture_HUD: target = &m_VKHUD; break;
            case VR::Texture_Blank: target = &m_VKBlankTexture; break;
            default: return;
            }

            target->m_Texture = texture;
            target->m_Width = desc.Width;
            target->m_Height = desc.Height;
            target->m_VRTexture.handle = texture;
            std::cout << "[D3D11] VR texture " << m_CreatingTextureID << ": " << desc.Width << "x" << desc.Height
                      << " format=" << desc.Format << "\n";
        };

    D3D11Hooks::Present_Pre_Callback = [this](IDXGISwapChain *swapChain)
        {
            if (m_IsInitialized)
            {
                CopyBackBuffer(swapChain);
                PrepareWindowImage(swapChain);
            }
        };

    D3D11Hooks::Present_Post_Callback = [this](IDXGISwapChain *)
        {
            Update();
        };
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

    m_Input->GetActionHandle("/actions/main/in/ActivateVR", &m_ActionActivateVR);
    m_Input->GetActionHandle("/actions/main/in/Jump", &m_ActionJump);
    m_Input->GetActionHandle("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    m_Input->GetActionHandle("/actions/main/in/Reload", &m_ActionReload);
    m_Input->GetActionHandle("/actions/main/in/Use", &m_ActionUse);
    m_Input->GetActionHandle("/actions/main/in/Walk", &m_ActionWalk);
    m_Input->GetActionHandle("/actions/main/in/Turn", &m_ActionTurn);
    m_Input->GetActionHandle("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    m_Input->GetActionHandle("/actions/main/in/NextItem", &m_ActionNextItem);
    m_Input->GetActionHandle("/actions/main/in/PrevItem", &m_ActionPrevItem);
    m_Input->GetActionHandle("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    m_Input->GetActionHandle("/actions/main/in/Crouch", &m_ActionCrouch);
    m_Input->GetActionHandle("/actions/main/in/Flashlight", &m_ActionFlashlight);
    m_Input->GetActionHandle("/actions/main/in/QuickPing", &m_ActionQuickPing);
    m_Input->GetActionHandle("/actions/main/in/RemoteView", &m_ActionRemoteView);
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ShowHUD", &m_ShowHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);
    m_Input->GetActionHandle("/actions/main/in/ThirdAttack", &m_ThirdAttack);
    m_Input->GetActionHandle("/actions/main/in/PointerLeft", &m_ActionPointerLeft);
    m_Input->GetActionHandle("/actions/main/in/PointerRight", &m_ActionPointerRight);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_ActiveActionSet = {};
    m_ActiveActionSet.ulActionSet = m_ActionSet;

    return 0;
}

void VR::InstallApplicationManifest(const char *fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\%s", currentDir, fileName);

    vr::VRApplications()->AddApplicationManifest(path);
}

void VR::SetScreenSizeOverride(bool bState) {
    bool isOverriding = m_Game->m_VguiSurface->IsScreenSizeOverrideActive();

    if (bState && !isOverriding || !bState && isOverriding) {
        int iOldWidth, iOldHeight;
        m_Game->m_VguiSurface->GetScreenSize(iOldWidth, iOldHeight);
        m_Game->m_VguiSurface->ForceScreenSizeOverride(bState, m_RenderWidth, m_RenderHeight);
       /*int x = 0, y = 0, w = m_RenderWidth, h = m_RenderHeight;

        if (m_Game->m_ClientMode->GetViewport())
            m_Game->m_ClientMode->AdjustEngineViewport(x, y, w, h);*/

        if (bState) {
            /*IMatRenderContext* renderContext = m_Game->m_MaterialSystem->GetRenderContext();
            renderContext->Viewport(0, 0, m_RenderWidth, m_RenderHeight);
            renderContext->Release();*/
        }

        m_Game->m_VguiSurface->OnScreenSizeChanged(iOldWidth, iOldHeight);
    }
}

void VR::Update()
{
    if (!m_IsInitialized || !m_Game->m_Initialized)
        return;

    // The Portal 2 version reset the render target here and recreated the eye textures after
    // each map change (workshop maps). Not here: this runs on the render thread with queued
    // rendering, and the RenderView hook resets the render target itself.

    SubmitVRTextures();
    UpdatePosesAndActions();
    UpdateHeadTracking();
    UpdateHandTracking();

    if (!m_InitialPosReset)
    {
        if (m_SeatedMode)
            ResetPosition();
        m_InitialPosReset = true;
    }

    // Portal 2/Reloaded: IsCursorVisible(); in Strata Source the vgui surface reports no cursor in
    // the menus (logged for comparison).
    const bool cursorVisible = m_Game->m_VguiSurface->IsCursorVisible();
    const bool menuVisible = IsMenuVisible();
    static int lastCursorVisible = -1, lastMenuVisible = -1;
    if ((int)cursorVisible != lastCursorVisible || (int)menuVisible != lastMenuVisible)
    {
        std::cout << "[VR] menu input: menu visible=" << menuVisible << " (cursor visible=" << cursorVisible << ")\n";
        lastCursorVisible = cursorVisible;
        lastMenuVisible = menuVisible;
    }

    g_MenuVisible = menuVisible;
    if (menuVisible) {
        m_SmoothTurnRate = 0;
        ProcessMenuInput();
    } else {
        // ProcessMenuInput may have left the overlay interactive (laser); not outside the menus.
        vr::VROverlay()->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        ProcessInput();
    }
}

// Eye render targets, created through the material system (the engine renders into ITextures);
// CreateTexture2D_Post_Callback picks up their D3D11 textures for the submit. Called from the
// RenderView hook (main thread). The blank texture is created directly with D3D11
// (CreateBlankTexture), the HUD texture is not needed yet.
void VR::CreateVRTextures()
{
    m_CreatedVRTextures = true; // once, also if it fails

    IMaterialSystem *materialSystem = m_Game->m_MaterialSystem;
    // Same format as the back buffer (B8G8R8A8_UNORM).
    const ImageFormat format = IMAGE_FORMAT_BGRA8888;

    // Render targets may only be allocated before the game has started; the flag is set again
    // before the textures are created, as in the Portal 2 version.
    bool &gameStarted = StrataMaterialSystem::GameStarted(materialSystem);
    const bool wasStarted = gameStarted;
    gameStarted = false;
    StrataMaterialSystem::BeginRenderTargetAllocation(materialSystem);
    gameStarted = wasStarted;

    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = StrataMaterialSystem::CreateNamedRenderTargetTextureEx(materialSystem, "vrLeftEye", m_RenderWidth, m_RenderHeight,
        RT_SIZE_NO_CHANGE, format, MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP, 0);

    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = StrataMaterialSystem::CreateNamedRenderTargetTextureEx(materialSystem, "vrRightEye", m_RenderWidth, m_RenderHeight,
        RT_SIZE_NO_CHANGE, format, MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP, 0);

    m_CreatingTextureID = Texture_None;

    StrataMaterialSystem::EndRenderTargetAllocation(materialSystem);

    std::cout << "[VR] eye render targets " << m_RenderWidth << "x" << m_RenderHeight << " (game started " << wasStarted << "): "
              << m_LeftEyeTexture << "/" << m_RightEyeTexture << ", D3D11 " << m_VKLeftEye.m_Texture << "/" << m_VKRightEye.m_Texture << "\n";
}

void VR::SubmitVRTextures()
{
    if (!m_RenderedNewFrame)
    {
        CreateBlankTexture();
        if (!m_VKBackBuffer.m_Texture || !m_VKBlankTexture.m_Texture)
            return;

        if (!vr::VROverlay()->IsOverlayVisible(m_MainMenuHandle))
            RepositionOverlays();

        vr::VRTextureBounds_t bounds{ 0, 0, 1, 1 };
        const bool menuVisible = IsMenuVisible();
        if (!menuVisible)
        {
            // The game view only renders to the window portion of the back buffer (Panorama
            // draws its menus over a larger part). Until we figure out a proper fix,
            // as a workaround only show that portion of the texture.
            int windowWidth, windowHeight;
            GetWindowSize(windowWidth, windowHeight);
            const float uMax = (float)windowWidth / m_VKBackBuffer.m_Width;
            const float vMax = (float)windowHeight / m_VKBackBuffer.m_Height;
            bounds.uMax = uMax < 1.0f ? uMax : 1.0f;
            bounds.vMax = vMax < 1.0f ? vMax : 1.0f;
        }

        vr::VROverlay()->SetOverlayTextureBounds(m_MainMenuHandle, &bounds);
        vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(m_MainMenuHandle, &m_VKBackBuffer.m_VRTexture);
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        //vr::VROverlay()->HideOverlay(m_HUDHandle);
        HideAimDot();
        HideVignette();

        vr::EVRCompositorError submitError = vr::VRCompositor()->Submit(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, NULL, vr::Submit_Default);
        vr::VRCompositor()->Submit(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, NULL, vr::Submit_Default);

        // Diagnostics: the first overlay frame and every change of the result codes.
        static int lastOverlayError = -1, lastSubmitError = -1, lastInGame = -1, lastMenu = -1;
        const int inGame = m_Game->m_EngineClient->IsInGame() ? 1 : 0;
        if (overlayError != lastOverlayError || submitError != lastSubmitError || inGame != lastInGame || menuVisible != lastMenu)
        {
            lastMenu = menuVisible;
            std::cout << "[VR] overlay frame: inGame=" << inGame << " menu=" << menuVisible << " backbuffer " << m_VKBackBuffer.m_Width << "x"
                      << m_VKBackBuffer.m_Height << " bounds u<" << bounds.uMax << " v<" << bounds.vMax
                      << " SetOverlayTexture=" << overlayError << " Submit=" << submitError << "\n";
            lastOverlayError = overlayError;
            lastSubmitError = submitError;
            lastInGame = inGame;
        }

        return;
    }
    vr::VROverlay()->HideOverlay(m_MainMenuHandle);

    //vr::VROverlay()->SetOverlayTexture(m_HUDHandle, &m_VKHUD.m_VRTexture);

    if (m_Game->m_VguiSurface->IsCursorVisible())
    {
        // We're in the pause menu
        //vr::VROverlay()->ShowOverlay(m_HUDHandle);
    }

    vr::VRCompositor()->Submit(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &(m_TextureBounds)[0], vr::Submit_Default);
    vr::VRCompositor()->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &(m_TextureBounds)[1], vr::Submit_Default);

    m_RenderedNewFrame = false;
}

void VR::GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut)
{
    if (poseRaw.bPoseIsValid) 
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        Vector vel;
        QAngle ang;
        QAngle angvel;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(mat.m[1][2]) * (180.0 / 3.141592654);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
        vel.x = -poseRaw.vVelocity.v[2];
        vel.y = -poseRaw.vVelocity.v[0];
        vel.z = poseRaw.vVelocity.v[1];
        angvel.x = -poseRaw.vAngularVelocity.v[2] * (180.0 / 3.141592654);
        angvel.y = -poseRaw.vAngularVelocity.v[0] * (180.0 / 3.141592654);
        angvel.z = poseRaw.vAngularVelocity.v[1] * (180.0 / 3.141592654);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceVel = vel;
        poseOut.TrackedDeviceAng = ang;
        poseOut.TrackedDeviceAngVel = angvel;
    }
}

void VR::RepositionOverlays()
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
    Vector hmdForward = { -hmdMat.m[0][2], 0, -hmdMat.m[2][2] };

    vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();

    hmdForward[1] = 0;
    VectorNormalize(hmdForward);

    // Main menu overlay: 3 m in front of the HMD, turned towards it (yaw only). A rigid
    // transform: the overlay's size comes from SetOverlayWidthInMeters, its aspect ratio from
    // the texture. (The D3D9 version scaled this matrix instead, so the visible area and
    // ComputeOverlayIntersection didn't match: no laser in the menu.)
    Vector menuNewPos = hmdForward * 3 + hmdPosition;
    float hmdYaw = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);

    vr::HmdMatrix34_t menuTransform =
    {
        cosf(hmdYaw),  0.0f, sinf(hmdYaw), menuNewPos.x,
        0.0f,          1.0f, 0.0f,         menuNewPos.y - 0.25f,
        -sinf(hmdYaw), 0.0f, cosf(hmdYaw), menuNewPos.z
    };

    const float menuHeight = 1.8f; // meters
    vr::VROverlay()->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_MainMenuHandle, menuHeight * m_RenderWidth / MenuHeight());

    // Reposition HUD overlay
    /*vr::HmdMatrix34_t hudTransform =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    Vector hudDistance = hmdForward * m_HudDistance;
    Vector hudNewPos = hudDistance + hmdPosition;

    hudTransform.m[0][3] = hudNewPos.x;
    hudTransform.m[1][3] = hudNewPos.y - 0.25;
    hudTransform.m[2][3] = hudNewPos.z;

    hudTransform.m[0][0] *= cos(hmdRotationDegrees);
    hudTransform.m[0][2] = sin(hmdRotationDegrees);
    hudTransform.m[2][0] = -sin(hmdRotationDegrees);
    hudTransform.m[2][2] *= cos(hmdRotationDegrees);

    vr::VROverlay()->SetOverlayTransformAbsolute(m_HUDHandle, trackingOrigin, &hudTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_HUDHandle, m_HudSize);*/
}

// A white dot with a dark edge, visible on any background.
void VR::CreateAimDot()
{
    constexpr int size = 32;
    constexpr float pixel = 2.0f / size;
    std::vector<uint8_t> pixels(size * size * 4);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float dx = (x + 0.5f) * pixel - 1, dy = (y + 0.5f) * pixel - 1;
            const float radius = sqrtf(dx * dx + dy * dy);
            uint8_t *rgba = &pixels[(y * size + x) * 4];
            rgba[0] = rgba[1] = rgba[2] = static_cast<uint8_t>(255 * std::clamp((0.7f - radius) / pixel + 0.5f, 0.0f, 1.0f));
            rgba[3] = static_cast<uint8_t>(255 * std::clamp((0.95f - radius) / pixel + 0.5f, 0.0f, 1.0f));
        }
    }

    m_Overlay->CreateOverlay("AimDotOverlayKey", "AimDotOverlay", &m_AimDotHandle);
    m_Overlay->SetOverlayAlpha(m_AimDotHandle, 0.4f);
    const vr::EVROverlayError error = m_Overlay->SetOverlayRaw(m_AimDotHandle, pixels.data(), size, size, 4);
    std::cout << "[VR] aim dot overlay: " << m_Overlay->GetOverlayErrorNameFromEnum(error) << "\n";
}

// Shows the dot at a point in the world, seen from the view of this frame: always the same size to
// the eye, turned towards it. As an overlay it's in front of everything.
void VR::ShowAimDot(const Vector &aim, const Vector &viewOrigin)
{
    constexpr float widthPerMeter = 0.006f; // about 0.35 degrees
    constexpr float maxDistance = 100.0f; // meters; no difference for the eyes beyond that

    const vr::TrackedDevicePose_t &hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    const auto dot = [](const Vector &a, const Vector &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const Vector delta = aim - viewOrigin;

    // From the eyes in meters, head coordinates: x right, y up, z backward.
    float offset[3] = { dot(delta, m_HmdRight) / m_VRScale, dot(delta, m_HmdUp) / m_VRScale, -dot(delta, m_HmdForward) / m_VRScale };
    const float length = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
    if (!hmdPose.bPoseIsValid || length < 0.001f)
        return HideAimDot();

    const float distance = (std::min)(length, maxDistance);
    const float z[3] = { -offset[0] / length, -offset[1] / length, -offset[2] / length };
    const float flat = sqrtf(z[0] * z[0] + z[2] * z[2]);
    const float x[3] = { flat > 0.001f ? z[2] / flat : 1.0f, 0.0f, flat > 0.001f ? -z[0] / flat : 0.0f };
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    const float position[3] = { -z[0] * distance, -z[1] * distance, m_EyeZ - z[2] * distance };

    // Head pose * dot in the head
    const vr::HmdMatrix34_t &head = hmdPose.mDeviceToAbsoluteTracking;
    vr::HmdMatrix34_t transform;
    const float *columns[4] = { x, y, z, position };
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            const float *c = columns[column];
            transform.m[row][column] = head.m[row][0] * c[0] + head.m[row][1] * c[1] + head.m[row][2] * c[2];
        }
        transform.m[row][3] += head.m[row][3];
    }

    m_Overlay->SetOverlayTransformAbsolute(m_AimDotHandle, vr::VRCompositor()->GetTrackingSpace(), &transform);
    m_Overlay->SetOverlayWidthInMeters(m_AimDotHandle, distance * widthPerMeter);
    m_Overlay->ShowOverlay(m_AimDotHandle);
}

void VR::HideAimDot()
{
    m_Overlay->HideOverlay(m_AimDotHandle);
}

// Fixed to the head, its edge outside the view. Fades only with its alpha, so no shaders.
void VR::CreateVignette()
{
    constexpr float distance = 2.0f; // meters; further away the eyes disagree less about its center

    m_Overlay->CreateOverlay("VignetteOverlayKey", "VignetteOverlay", &m_VignetteHandle);

    const vr::HmdMatrix34_t inFront =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, -distance
    };
    std::cout << "[VR] vignette transform: " << m_Overlay->GetOverlayErrorNameFromEnum(
        m_Overlay->SetOverlayTransformTrackedDeviceRelative(m_VignetteHandle, vr::k_unTrackedDeviceIndex_Hmd, &inFront)) << "\n";

    const float tanHalfFov = tanf(DEG2RAD(m_Fov) / 2);
    m_VignetteTanHalfWidth = 1.2f * (std::max)(tanHalfFov, tanHalfFov / m_Aspect);
    m_Overlay->SetOverlayWidthInMeters(m_VignetteHandle, 2 * distance * m_VignetteTanHalfWidth);

    // Same order and distance would sort by distance only; the aim dot has to stay visible.
    m_Overlay->SetOverlaySortOrder(m_AimDotHandle, 1);
}

// Black with a soft round opening: clear up to the angle from the center of the view, opaque from
// softAngle further out.
void VR::DrawVignette(float clearAngle)
{
    constexpr int size = 256; // OpenVR limits the data of SetOverlayRaw
    constexpr float softAngle = 16;

    const float clearRadius = tanf(DEG2RAD(clearAngle)) / m_VignetteTanHalfWidth;
    const float opaqueRadius = tanf(DEG2RAD(clearAngle + softAngle)) / m_VignetteTanHalfWidth;
    std::vector<uint8_t> pixels(size * size * 4);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            const float dx = (x + 0.5f) * 2 / size - 1, dy = (y + 0.5f) * 2 / size - 1;
            const float t = std::clamp((sqrtf(dx * dx + dy * dy) - clearRadius) / (opaqueRadius - clearRadius), 0.0f, 1.0f);
            pixels[(y * size + x) * 4 + 3] = static_cast<uint8_t>(255 * t * t * (3 - 2 * t));
        }
    }

    const vr::EVROverlayError error = m_Overlay->SetOverlayRaw(m_VignetteHandle, pixels.data(), size, size, 4);
    std::cout << "[VR] vignette clear up to " << clearAngle << " degrees: " << m_Overlay->GetOverlayErrorNameFromEnum(error) << "\n";
    m_VignetteDrawnAngle = clearAngle;
}

// Once per frame in the game. walkedDistance: how far the view moved with the stick or keyboard.
void VR::UpdateVignette(float walkedDistance)
{
    constexpr float fullSpeed = 120; // units per second, estimated; walking is up to 175
    constexpr float fullTurnRate = 60; // degrees per second, estimated; the stick turns up to 150
    constexpr float fullTilt = 20; // degrees, estimated
    constexpr float fadeInSeconds = 0.15f, fadeOutSeconds = 0.4f;

    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::chrono::duration<float>(now - m_VignetteTime).count();
    m_VignetteTime = now;

    const float walking = std::clamp(walkedDistance / (std::max)(seconds, 0.001f) / fullSpeed, 0.0f, 1.0f);
    const float turning = (std::min)(1.0f, m_SmoothTurnRate / fullTurnRate);
    const float target = (std::max)(walking * m_VignetteWalking, turning * m_VignetteTurning);
    m_VignetteMoving = target > m_VignetteMoving
        ? (std::min)(target, m_VignetteMoving + seconds / fadeInSeconds)
        : (std::max)(target, m_VignetteMoving - seconds / fadeOutSeconds);

    // Straight from the tilt left, without a fade: it's gone again within a fraction of a second.
    const float tilt = (std::min)(1.0f, (std::max)(std::abs(m_RotationOffset.x), std::abs(m_RotationOffset.z)) / fullTilt);

    const float alpha = (std::max)(m_VignetteMoving, tilt * m_VignetteAfterPortal);
    if (alpha <= 0.001f)
        return HideVignette();
    if (alpha == m_VignetteAlpha)
        return;

    m_Overlay->SetOverlayAlpha(m_VignetteHandle, alpha);
    if (m_VignetteAlpha == 0)
    {
        // The config can change while the game runs.
        const float clearAngle = std::clamp(m_VignetteClearAngle, 0.0f, 70.0f);
        if (clearAngle != m_VignetteDrawnAngle)
            DrawVignette(clearAngle);
        m_Overlay->ShowOverlay(m_VignetteHandle);
        m_VignetteShownAt = now;
        m_VignettePeak = 0;
    }
    m_VignetteAlpha = alpha;
    m_VignettePeak = (std::max)(m_VignettePeak, alpha);
}

void VR::HideVignette()
{
    m_VignetteMoving = 0;
    if (m_VignetteAlpha == 0)
        return;

    m_Overlay->HideOverlay(m_VignetteHandle);
    m_VignetteAlpha = 0;
    std::cout << "[VR] vignette for " << std::chrono::duration<float>(std::chrono::steady_clock::now() - m_VignetteShownAt).count()
              << " s, up to " << m_VignettePeak << "\n";
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
    m_Input->UpdateActionState(&m_ActiveActionSet, sizeof(vr::VRActiveActionSet_t), 1);
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

bool VR::CheckDigitalActionChanged(vr::VRActionHandle_t &actionHandle, bool &state)
{
    vr::InputDigitalActionData_t digitalActionData;
    vr::EVRInputError result = m_Input->GetDigitalActionData(actionHandle, &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle);
    
    if (result == vr::VRInputError_None)
    {
        state = digitalActionData.bState;
        return digitalActionData.bChanged;
    }

    state = false;
    return false;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut)
{
    vr::EVRInputError result = m_Input->GetAnalogActionData(actionHandle, &analogDataOut, sizeof(analogDataOut), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
        return true;

    return false;
}

void VR::ProcessMenuInput()
{
    //vr::VROverlayHandle_t currentOverlay = m_Game->m_EngineClient->IsInGame() ? m_HUDHandle : m_MainMenuHandle;
    vr::VROverlayHandle_t currentOverlay = m_MainMenuHandle;

    // Here, because inputsystem.dll has its window procedure in place by the time there is a menu.
    DXGI_SWAP_CHAIN_DESC swapDesc;
    const HWND window = D3D11Hooks::g_SwapChain && SUCCEEDED(D3D11Hooks::g_SwapChain->GetDesc(&swapDesc))
        ? swapDesc.OutputWindow : nullptr;
    if (window && !g_GameWindowProc)
    {
        g_MenuPartWidth = m_RenderWidth;
        g_MenuPartHeight = MenuHeight();
        g_GameWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuMouseWindowProc)));
        std::cout << "[VR] window mouse scaled to the menu: " << (g_GameWindowProc ? "yes" : "no") << "\n";
    }
    const WPARAM laserMark = g_GameWindowProc ? kMouseFromLaser : 0;

    // Check if left or right hand controller is pointing at the overlay
    const bool isHoveringOverlay = CheckOverlayIntersectionForController(currentOverlay, vr::TrackedControllerRole_LeftHand) ||
                                   CheckOverlayIntersectionForController(currentOverlay, vr::TrackedControllerRole_RightHand);

    static int lastHovering = -1;
    if ((int)isHoveringOverlay != lastHovering)
    {
        std::cout << "[VR] controller pointing at the menu overlay: " << isHoveringOverlay << "\n";
        lastHovering = isHoveringOverlay;
    }

    // Overlays can't process action inputs if the laser is active, so
    // only activate laser if a controller is pointing at the overlay
    if (isHoveringOverlay)
    {
        vr::VROverlay()->SetOverlayFlag(currentOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

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
        vr::VREvent_t vrEvent;
        while (vr::VROverlay()->PollNextOverlayEvent(currentOverlay, &vrEvent, sizeof(vrEvent)))
        {
            int x, y;
            switch (vrEvent.eventType)
            {
            case vr::VREvent_MouseMove:
                toWindow(vrEvent.data.mouse, x, y);
                if (window)
                    PostMessage(window, WM_MOUSEMOVE, laserMark, MAKELPARAM(x, y));
                break;

            case vr::VREvent_MouseButtonDown:
            {
                toWindow(vrEvent.data.mouse, x, y);
                POINT cursor = {};
                GetCursorPos(&cursor);
                if (window)
                    ScreenToClient(window, &cursor);
                std::cout << "[VR] menu click at overlay " << vrEvent.data.mouse.x << "," << vrEvent.data.mouse.y
                          << " -> message " << x << "," << y << " (cursor " << cursor.x << "," << cursor.y
                          << ", game window " << (window ? "found" : "NOT found")
                          << (window && GetForegroundWindow() == window ? ", foreground" : ", NOT foreground") << ")\n";
                if (window)
                {
                    PostMessage(window, WM_MOUSEMOVE, laserMark, MAKELPARAM(x, y));
                    PostMessage(window, WM_LBUTTONDOWN, MK_LBUTTON | laserMark, MAKELPARAM(x, y));
                }
                break;
            }

            case vr::VREvent_MouseButtonUp:
                toWindow(vrEvent.data.mouse, x, y);
                if (window)
                    PostMessage(window, WM_LBUTTONUP, laserMark, MAKELPARAM(x, y));
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
        vr::VROverlay()->SetOverlayFlag(currentOverlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        
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
    if (!m_IsVREnabled)
        return;

    //vr::VROverlay()->SetOverlayFlag(m_HUDHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

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
            float deadzone = 0.2;
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
            return a * (1.0 - f) + (b * f);
        };

        float lerpedPitch = lerp(m_RotationOffset.x, targetRotation.x, m_CameraUprightRecoverySpeed);
        float lerpedYaw = lerp(m_RotationOffset.y, targetRotation.y, m_CameraUprightRecoverySpeed);
        float lerpedRoll = lerp(m_RotationOffset.z, targetRotation.z, m_CameraUprightRecoverySpeed);

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

    if (CheckDigitalActionChanged(m_ThirdAttack, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("ent_fire @att3 trigger");
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

    if (CheckDigitalActionChanged(m_ActionQuickPing, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+quick_ping" : "-quick_ping");
    }

    if (CheckDigitalActionChanged(m_ActionRemoteView, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+remote_view" : "-remote_view");
    }

    if (CheckDigitalActionChanged(m_ActionReload, state))
    {
        m_Game->ClientCmd_Unrestricted(state ? "+reload" : "-reload");
    }

    if (CheckDigitalActionChanged(m_ActionPrevItem, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("invprev");
    }
    else if (CheckDigitalActionChanged(m_ActionNextItem, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("invnext");
    }

    if (CheckDigitalActionChanged(m_ActionResetPosition, state) && state)
    {
        ResetPosition();
    }

    if (CheckDigitalActionChanged(m_ActionFlashlight, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("impulse 100");
    }

    if (CheckDigitalActionChanged(m_Spray, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("impulse 201");
    }
    
    /*bool isControllerVertical = m_RightControllerAngAbs.x > 60 || m_RightControllerAngAbs.x < -45;
    if ((PressedDigitalAction(m_ShowHUD) || PressedDigitalAction(m_Scoreboard) || isControllerVertical || m_HudAlwaysVisible)
        && m_RenderedHud)
    {
        if (!vr::VROverlay()->IsOverlayVisible(m_HUDHandle) || m_HudAlwaysVisible)
            RepositionOverlays();

        if (PressedDigitalAction(m_Scoreboard))
            m_Game->ClientCmd_Unrestricted("+showscores");
        else
            m_Game->ClientCmd_Unrestricted("-showscores");

        vr::VROverlay()->ShowOverlay(m_HUDHandle);
    }
    else
    {
        vr::VROverlay()->HideOverlay(m_HUDHandle);
    }*/

    m_RenderedHud = false;

    if (CheckDigitalActionChanged(m_Pause, state) && state)
    {
        m_Game->ClientCmd_Unrestricted("gameui_activate");
        RepositionOverlays();
    }
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
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
        if (!m_HavePointerOffset[left])
            std::cout << "[VR] laser " << (left ? "left" : "right") << ": tip offset learned from the pointer action\n";
        m_PointerOffset[left] = RelativePose(controllerPose->mDeviceToAbsoluteTracking, pointer.pose.mDeviceToAbsoluteTracking);
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

QAngle VR::GetRightControllerAbsAngle()
{
    return m_RightControllerAngAbs;
}

QAngle& VR::GetRightControllerAbsAngleConst()
{
    return m_RightControllerAngAbs;
}

Vector VR::GetRightControllerAbsPos(Vector eyePosition)
{
    Vector offset = eyePosition;

    if (offset.x == 0 && offset.y == 0 && offset.z == 0) {
        /*int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
        if (!localPlayer)
            return {0, 0, 0};

        offset = localPlayer->EyePosition();*/

        offset = m_SetupOrigin;
    }

    Vector position = offset + m_RightControllerPosRel;

    if (m_6DOF)
        position += m_HmdPosRelative;

    return position;
}

Vector VR::GetRecommendedViewmodelAbsPos(Vector eyePosition)
{
    Vector viewmodelPos = GetRightControllerAbsPos(eyePosition);
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    return result;
}

void VR::UpdateHMDAngles() {
    QAngle hmdAngLocal = m_HmdPose.TrackedDeviceAng;

    //hmdAngLocal += m_RotationOffset;
    hmdAngLocal.x += m_RotationOffset.x;
    hmdAngLocal.y += m_RotationOffset.y;
    hmdAngLocal.z += m_RotationOffset.z;

    //hmdAngLocal.Normalize();

    QAngle::AngleVectors(hmdAngLocal, &m_HmdForward, &m_HmdRight, &m_HmdUp);

    //hmdAngLocal.x = (hmdAngLocal.x > 180 ? 180)
    hmdAngLocal.Normalize();

    m_HmdAngAbs = hmdAngLocal;
}

void VR::ResetPosition()
{
    m_Center = m_HmdPose.TrackedDevicePos;
    if (!m_SeatedMode)
        m_Center.z = 0;
}

// The head stays where it is in the world although the player's eyes move by step (world units,
// horizontal): the room moves along.
void VR::MoveRoom(const Vector &step)
{
    Vector stepInRoom = step;
    VectorPivotXY(stepInRoom, { 0, 0, 0 }, -m_RotationOffset.y);
    m_Center.x += stepInRoom.x / m_VRScale;
    m_Center.y += stepInRoom.y / m_VRScale;
    m_HmdPosRelative.x -= step.x;
    m_HmdPosRelative.y -= step.y;
}

// HMD pose -> view angles, position relative to the player's eyes, eye distance. Needs nothing
// from the game.
void VR::UpdateHeadTracking()
{
    GetPoses();
    UpdateHeadPose();
}

// The pose read last, with the current rotation offset.
void VR::UpdateHeadPose()
{
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;
    Vector hmdPosCentered = hmdPosLocal - m_Center;

    m_HmdPosRelativeRaw = hmdPosCentered;

    //std::cout << "HMD - X: " << hmdWorldPos.x << ", Y: " << hmdWorldPos.y << ", Z: " << hmdWorldPos.z << "\n";

    Vector hmdPosCorrected = hmdPosCentered;
    VectorPivotXY(hmdPosCorrected, { 0, 0, 0 }, m_RotationOffset.y);
    
    UpdateHMDAngles();

    m_HmdPosRelative = hmdPosCorrected * m_VRScale;
    if (!m_SeatedMode)
        // 64 is the eye view height from the player's base position
        // we subtract this here so that we place the HMD height at the actual player height if 6DOF is enabled
        m_HmdPosRelative.z -= 64;
    else
        m_HmdPosRelative.z += m_SeatedHeightOffset;

    GetViewParameters();
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;
}

// Controller poses -> aim and viewmodel directions, positions relative to the HMD. Needs nothing
// from the game.
void VR::UpdateHandTracking()
{
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;

    // Hand tracking
    Vector leftControllerPosLocal = m_LeftControllerPose.TrackedDevicePos;
    QAngle leftControllerAngLocal = m_LeftControllerPose.TrackedDeviceAng;

    Vector rightControllerPosLocal = m_RightControllerPose.TrackedDevicePos;
    QAngle rightControllerAngLocal = m_RightControllerPose.TrackedDeviceAng;

    //std::cout << "Right Controller - X: " << rightControllerPosLocal.x << "Y: " << rightControllerPosLocal.y << "Z: " << rightControllerPosLocal.z << "\n";

    Vector hmdToController = rightControllerPosLocal - hmdPosLocal;
    //Vector rightControllerPosCorrected = hmdPosCorrected + hmdToController;

    // When using stick turning, pivot the controllers around the HMD
    VectorPivotXY(hmdToController, { 0, 0, 0 }, m_RotationOffset.y);

    m_RightControllerPosRel = hmdToController * m_VRScale;

    //rightControllerAngLocal += m_RotationOffset;
    rightControllerAngLocal.x += m_RotationOffset.x;
    rightControllerAngLocal.y += m_RotationOffset.y;
    rightControllerAngLocal.z += m_RotationOffset.z;

    // Same for the left hand (it sets the walking direction).
    leftControllerAngLocal.x += m_RotationOffset.x;
    leftControllerAngLocal.y += m_RotationOffset.y;
    leftControllerAngLocal.z += m_RotationOffset.z;

    // Wrap angle from -180 to 180
    //rightControllerAngLocal.Normalize();

    QAngle::AngleVectors(leftControllerAngLocal, &m_LeftControllerForward, &m_LeftControllerRight, &m_LeftControllerUp);
    QAngle::AngleVectors(rightControllerAngLocal, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);

    // The grip pose points above where the hand aims: turn it downward. Not needed with the tip
    // pose (SteamVR's laser direction), see GetPoses.
    const float leftOffset = m_AimFromTip[1] ? 0.0f : -30.0f;
    const float rightOffset = m_AimFromTip[0] ? 0.0f : -30.0f;

    m_LeftControllerForward = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, leftOffset);
    m_LeftControllerUp = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, leftOffset);

    m_RightControllerForward = VectorRotate(m_RightControllerForward, m_RightControllerRight, rightOffset);
    m_RightControllerUp = VectorRotate(m_RightControllerUp, m_RightControllerRight, rightOffset);

    // controller angles
    QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, m_LeftControllerAngAbs);
    QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, m_RightControllerAngAbs);
    m_RightControllerAngAbs.Normalize();

    // The gun's visible axis in v_portalgun.mdl points 4.55 deg left and 5.01 deg up of the model's x
    // axis in the idle animation (bone Base, its z axis plus 0.82 deg to the right for the mesh). The
    // carrying animation turns and moves the gun further. So the gun is turned to bring that axis in
    // line with the controller, around the hand: with the positions, for the config's position
    // offset 8, 7.5, -12, the bone Base stays where the unturned {4.5, -1, 1.5} had it.
    const PositionAngle viewmodelIdle{ {4.24f, -1.43f, 1.96f}, {-5.03f, -4.53f, 0} };
    const PositionAngle viewmodelCarrying{ {3.34f, -4.14f, -2.0f}, {-9.52f, -7.64f, -24.61f} };

    // About as long as the animations take to get there.
    constexpr float carryBlendSeconds = 0.5f;
    const auto now = std::chrono::steady_clock::now();
    const float carryStep = std::chrono::duration<float>(now - m_ViewmodelCarryTime).count() / carryBlendSeconds;
    m_ViewmodelCarryTime = now;
    m_ViewmodelCarryWeight = std::clamp(m_ViewmodelCarryWeight + (m_ViewmodelCarrying ? carryStep : -carryStep), 0.0f, 1.0f);
    const float w = m_ViewmodelCarryWeight;

    // Apply both hardcoded and custom (from config) viewmodel offsets here:
    m_ViewmodelPosOffset = viewmodelIdle.position + (viewmodelCarrying.position - viewmodelIdle.position) * w + m_ViewmodelPosCustomOffset;
    m_ViewmodelAngOffset.x = viewmodelIdle.angle.x + (viewmodelCarrying.angle.x - viewmodelIdle.angle.x) * w + m_ViewmodelAngCustomOffset.x;
    m_ViewmodelAngOffset.y = viewmodelIdle.angle.y + (viewmodelCarrying.angle.y - viewmodelIdle.angle.y) * w + m_ViewmodelAngCustomOffset.y;
    m_ViewmodelAngOffset.z = viewmodelIdle.angle.z + (viewmodelCarrying.angle.z - viewmodelIdle.angle.z) * w + m_ViewmodelAngCustomOffset.z;

    m_ViewmodelForward = m_RightControllerForward;
    m_ViewmodelUp = m_RightControllerUp;
    m_ViewmodelRight = m_RightControllerRight;

    // Viewmodel yaw offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, m_ViewmodelAngOffset.y);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, m_ViewmodelAngOffset.y);

    // Viewmodel pitch offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelAngOffset.x);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, m_ViewmodelAngOffset.x);

    // Viewmodel roll offset
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, m_ViewmodelAngOffset.z);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, m_ViewmodelAngOffset.z);
}

Vector VR::GetViewAngle()
{
    return Vector( m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z );
}

Vector VR::GetViewOrigin(Vector setupOrigin)
{
    Vector center = setupOrigin;

    if (m_6DOF)
        center += m_HmdPosRelative;

    return center + (m_HmdForward * -(m_EyeZ * m_VRScale));
}

Vector VR::GetViewOriginLeft(Vector setupOrigin)
{
    Vector viewOriginLeft = GetViewOrigin(setupOrigin);
    viewOriginLeft -= m_HmdRight * ((m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginLeft;
}

Vector VR::GetViewOriginRight(Vector setupOrigin)
{
    Vector viewOriginRight = GetViewOrigin(setupOrigin);
    viewOriginRight += m_HmdRight * ((m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}

// [CONFIG PARSING UTILITY FUNCTION]
// Generates an error message by stringifying and concatenating 'args...'.
template <typename... Ts>
static void concatErrorMsg(Game& game, const Ts&... args)
{
    std::ostringstream oss;
    (oss << ... << args);
    game.errorMsg(oss.str().c_str());
}

// [CONFIG PARSING UTILITY FUNCTION]
// Attempts to parse an entry with key 'key' from the provided 'userConfig'. If the key is
// missing or if the parsing fails, 'defaultValue' is returned and an error message is
// generated.
template <typename T>
static T parseConfigEntry(
    const std::unordered_map<std::string, std::string>& userConfig, Game& game,
    const char* key, const T& defaultValue)
try
{
    const auto itr = userConfig.find(key);

    if (itr == userConfig.end())
    {
        concatErrorMsg(game, "Config entry with key '", key,
            "' missing -- reverting to default value of '", defaultValue, "'");

        return defaultValue;
    }

    const std::string& configValue = itr->second;

    if constexpr (std::is_same_v<T, bool>)
    {
        return configValue == "true";
    }
    else if constexpr(std::is_floating_point_v<T>)
    {
        return std::stof(configValue);
    }
    else if constexpr(std::is_integral_v<T>)
    {
        return std::stol(configValue);
    }
    else
    {
        // Just a way of generating a compilation failure in case this branch is taken.
        struct invalid_type;
        return invalid_type{};
    }
}
catch (const std::logic_error& e)
{
    concatErrorMsg(game, "Error parsing config entry with key '", key,
        "' -- reverting to default value of '", defaultValue, "' -- error: (", e.what(), ")");

    throw;
}

void VR::ParseConfigFile()
{
    std::ifstream configStream("VR\\config.txt");
    std::unordered_map<std::string, std::string> userConfig;

    std::string line;
    while (std::getline(configStream, line))
    {
        std::istringstream sLine(line);
        std::string key;
        if (std::getline(sLine, key, '='))
        {
            std::string value;
            if (std::getline(sLine, value, '#'))
            {
                // Without this, "true # comment" would not count as true.
                value.erase(value.find_last_not_of(" \t\r") + 1);
                userConfig[key] = value;
            }
        }
    }

    if (userConfig.empty())
        return;

    // Parse a single entry with key 'key' from the config into 'target'.
    // If the entry does not exist, or if the parsing fails, sets 'target' to
    // 'defaultValue'.
    const auto parseOrDefault = [&](const char* key, auto& target,
                                    const auto& defaultValue) 
    { 
        target = parseConfigEntry(userConfig, *m_Game, key, defaultValue);
        std::cout << "Setting '" << key << "' to '" << target << "'\n";
    };

    // Parses a vector or angle from the config into 'target'. The XYZ coordinates
    // are read from three separate config entries with key 'keyPrefix' + 'X'/'Y'/'Z'.
    // If any entry does not exist, or if the parsing fails, sets the corresponding
    // coordinate in 'target' to zero.
    const auto parseXYZOrDefaultZero = [&](std::string keyPrefix, auto& target)
    {
        parseOrDefault((keyPrefix + "X").c_str(), target.x, 0.f);
        parseOrDefault((keyPrefix + "Y").c_str(), target.y, 0.f);
        parseOrDefault((keyPrefix + "Z").c_str(), target.z, 0.f);
    };

    parseOrDefault("SnapTurning", m_SnapTurning, true);
    parseOrDefault("SnapTurnAngle", m_SnapTurnAngle, 30.0f);
    parseOrDefault("TurnSpeed", m_TurnSpeed, 0.15f);
    parseOrDefault("LeftHanded", m_LeftHanded, false);
    parseOrDefault("VRScale", m_VRScale, 43.2f);
    parseOrDefault("IPDScale", m_IpdScale, 1.0f);
    parseOrDefault("6DOF", m_6DOF, true);
    parseOrDefault("SeatedMode", m_SeatedMode, false);
    parseOrDefault("SeatedHeightOffset", m_SeatedHeightOffset, 4.0f);
    parseOrDefault("Roomscale", m_Roomscale, true);
    /*parseOrDefault("HudDistance", m_HudDistance, 1.3f);
    parseOrDefault("HudSize", m_HudSize, 4.0f);
    parseOrDefault("HudAlwaysVisible", m_HudAlwaysVisible, false);*/
    parseOrDefault("AimMode", m_AimMode, 1);
    parseXYZOrDefaultZero("ViewmodelPosCustomOffset", m_ViewmodelPosCustomOffset);
    parseXYZOrDefaultZero("ViewmodelAngCustomOffset", m_ViewmodelAngCustomOffset);
    parseOrDefault("ApplyPitchAndRollPortalRotationOffset", m_ApplyPitchAndRollPortalRotationOffset, true);
    parseOrDefault("CameraUprightRecoverySpeed", m_CameraUprightRecoverySpeed, 0.2f);
    parseOrDefault("VignetteWalking", m_VignetteWalking, 0.0f);
    parseOrDefault("VignetteTurning", m_VignetteTurning, 0.8f);
    parseOrDefault("VignetteAfterPortal", m_VignetteAfterPortal, 1.0f);
    parseOrDefault("VignetteClearAngle", m_VignetteClearAngle, 20.0f);
}

void VR::WaitForConfigUpdate()
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char configDir[MAX_STR_LEN];
    sprintf_s(configDir, MAX_STR_LEN, "%s\\VR\\", currentDir);
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir, false, FILE_NOTIFY_CHANGE_LAST_WRITE);

    std::filesystem::file_time_type configLastModified;
    while (1)
    {
        try 
        {
            // Windows only notifies of change within a directory, so extra check here for just config.txt
            auto configModifiedTime = std::filesystem::last_write_time("VR\\config.txt");
            if (configModifiedTime != configLastModified)
            {
                configLastModified = configModifiedTime;
                ParseConfigFile();
                
                std::cout << "Successfully reloaded 'config.txt'\n";
            }
        }
        catch (const std::invalid_argument &e)
        {
            concatErrorMsg(
                *m_Game, "Failed to parse 'config.txt' (", e.what(), ")");
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            concatErrorMsg(
                *m_Game, "'config.txt' not found. (", e.what(), ")");
            
            return;
        }
        
        FindNextChangeNotification(fileChangeHandle);
        WaitForSingleObject(fileChangeHandle, INFINITE);
        Sleep(100); // Sometimes the thread tries to read config.txt before it's finished writing
    }
}
