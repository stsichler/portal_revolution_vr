// Class VR: OpenVR, the frames to the headset, head and hand tracking. Its other parts:
// vr_config.cpp, vr_input.cpp, vr_overlays.cpp.
#include "vr.h"
#include <Windows.h>
#include "sdk.h"
#include "game.h"
#include "strata_hooks.h"
#include "convars.h"
#include "log.h"
#include <dxgi1_3.h>
#include <iostream>
#include <string>
#include <thread>
#include <algorithm>
#include <vector>
#include "d3d11_hooks.h"
#include <cmath>
#include <cstring>

VR::VR(Game *game)
{
    m_Game = game;
}

void VR::init()
{
    InitOpenVR();

    if (!vr::VRCompositor())
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    m_Input = vr::VRInput();

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
    m_Fov = static_cast<float>(2.0 * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2));

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    CompleteConfigFile();
    ParseConfigFile();
    std::thread configParser(&VR::WaitForConfigUpdate, this);
    configParser.detach();

    // The menu overlay shows m_BackBufferCopy, a copy of the back buffer made in Present_Pre_Callback.
    m_Overlay = vr::VROverlay();
    m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
    m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
    m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);

    const vr::HmdVector2_t mouseScaleMenu = { (float)m_RenderWidth, (float)MenuHeight() };
    m_Overlay->SetOverlayCurvature(m_MainMenuHandle, 0.15f);
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);
    CreateAimDot();
    CreateVignette();
    CreateHudOverlay();

    UpdateTrackingSpace();

    UpdatePosesAndActions();

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

// Black texture submitted to both eyes while only the overlay is shown.
void VR::CreateBlankTexture()
{
    if (m_BlankTexture.m_Texture || !D3D11Hooks::g_Device)
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

    if (FAILED(D3D11Hooks::g_Device->CreateTexture2D(&desc, &data, &m_BlankTexture.m_Texture)))
    {
        std::cout << "[D3D11] could not create the blank texture\n";
        return;
    }
    m_BlankTexture.m_Width = size;
    m_BlankTexture.m_Height = size;
    m_BlankTexture.m_VRTexture.handle = m_BlankTexture.m_Texture;
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

// The game is quitting, its main loop has ended. Without this SteamVR keeps showing the last
// frame and the overlays until the process is gone, which takes seconds. Main thread.
void VR::Shutdown()
{
    if (m_ShutDown || !m_System)
        return;
    m_ShutDown = true;
    m_IsInitialized = false;
    vr::VR_Shutdown();
    std::cout << "[VR] game quits, VR connection ended\n";
}

void VR::InitOpenVR()
{
    if (m_System)
        return;

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
    char model[vr::k_unMaxPropertyStringSize] = "?";
    system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String, model, sizeof(model));
    std::cout << "[OpenVR] initialized, runtime " << system->GetRuntimeVersion() << ", headset " << model
              << ", render target " << m_RenderWidth << "x" << m_RenderHeight << "\n";
    m_System = system;
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
    if (FAILED(sized) || !m_RenderedNewFrame || !m_RightEye.m_Texture)
        return;

    ID3D11Texture2D *backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
        return;
    const UINT top = (m_RenderHeight - height) / 2;
    const D3D11_BOX middle = { 0, top, 0, m_RenderWidth, top + height, 1 };
    D3D11Hooks::g_Context->CopySubresourceRegion(backBuffer, 0, 0, 0, 0, m_RightEye.m_Texture, 0, &middle);
    backBuffer->Release();
}

// Copies the game's back buffer into m_BackBufferCopy, which the menu overlay shows. A copy,
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

    if (!m_BackBufferCopy.m_Texture || m_BackBufferCopy.m_Width != desc.Width || m_BackBufferCopy.m_Height != desc.Height)
    {
        if (m_BackBufferCopy.m_Texture)
            m_BackBufferCopy.m_Texture->Release();
        m_BackBufferCopy = SharedTextureHolder();

        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.SampleDesc = { 1, 0 };
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        if (FAILED(D3D11Hooks::g_Device->CreateTexture2D(&copyDesc, nullptr, &m_BackBufferCopy.m_Texture)))
        {
            std::cout << "[D3D11] could not create the back buffer copy\n";
            backBuffer->Release();
            return;
        }
        m_BackBufferCopy.m_Width = desc.Width;
        m_BackBufferCopy.m_Height = desc.Height;
        m_BackBufferCopy.m_VRTexture.handle = m_BackBufferCopy.m_Texture;
    }

    const D3D11_BOX part = { 0, 0, 0, desc.Width, desc.Height, 1 };
    D3D11Hooks::g_Context->CopySubresourceRegion(m_BackBufferCopy.m_Texture, 0, 0, 0, 0, backBuffer, 0, &part);
    backBuffer->Release();
}

// The engine plays its startup video (media/strata.webm) before the game's DLLs are loaded, so
// before init(). It draws to the window's part of the back buffer; an overlay of its own shows that
// part where the menu will be, until the menu overlay takes over.
void VR::ShowStartupFrame(IDXGISwapChain *swapChain)
{
    ID3D11Texture2D *backBuffer = nullptr;
    if (!D3D11Hooks::g_Device || !vr::VROverlay() || FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
        return;

    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc(&desc);
    int windowWidth, windowHeight;
    GetWindowSize(windowWidth, windowHeight);
    desc.Width = std::min<UINT>(desc.Width, windowWidth);
    desc.Height = std::min<UINT>(desc.Height, windowHeight);

    if (!m_StartupCopy.m_Texture || m_StartupCopy.m_Width != desc.Width || m_StartupCopy.m_Height != desc.Height)
    {
        if (m_StartupCopy.m_Texture)
            m_StartupCopy.m_Texture->Release();
        m_StartupCopy = SharedTextureHolder();
        D3D11_TEXTURE2D_DESC copyDesc = desc;
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
        copyDesc.SampleDesc = { 1, 0 };
        copyDesc.Usage = D3D11_USAGE_DEFAULT;
        copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copyDesc.CPUAccessFlags = 0;
        copyDesc.MiscFlags = 0;
        if (FAILED(D3D11Hooks::g_Device->CreateTexture2D(&copyDesc, nullptr, &m_StartupCopy.m_Texture)))
        {
            backBuffer->Release();
            return;
        }
        m_StartupCopy.m_Width = desc.Width;
        m_StartupCopy.m_Height = desc.Height;
        m_StartupCopy.m_VRTexture.handle = m_StartupCopy.m_Texture;
    }
    const D3D11_BOX part = { 0, 0, 0, desc.Width, desc.Height, 1 };
    D3D11Hooks::g_Context->CopySubresourceRegion(m_StartupCopy.m_Texture, 0, 0, 0, 0, backBuffer, 0, &part);
    backBuffer->Release();

    if (m_StartupHandle == vr::k_ulOverlayHandleInvalid)
    {
        const vr::EVROverlayError error = vr::VROverlay()->CreateOverlay("StartupOverlayKey", "StartupOverlay", &m_StartupHandle);
        std::cout << "[VR] startup video overlay " << desc.Width << "x" << desc.Height << ": " << vr::VROverlay()->GetOverlayErrorNameFromEnum(error) << "\n";
        if (error != vr::VROverlayError_None)
            return;
        vr::VROverlay()->SetOverlayCurvature(m_StartupHandle, 0.15f);
    }

    // Placed again until the headset's pose is known, at the start it may still be invalid.
    static bool placed = false;
    if (!placed)
    {
        const vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();
        const vr::HmdMatrix34_t transform = MenuTransform(trackingOrigin, &placed);
        vr::VROverlay()->SetOverlayTransformAbsolute(m_StartupHandle, trackingOrigin, &transform);
    }
    vr::VROverlay()->SetOverlayWidthInMeters(m_StartupHandle, kMenuOverlayHeight * desc.Width / desc.Height);
    vr::VROverlay()->SetOverlayTexture(m_StartupHandle, &m_StartupCopy.m_VRTexture);
    vr::VROverlay()->ShowOverlay(m_StartupHandle);
}

// The scene around the startup video: black, as around the menu.
void VR::SubmitStartupFrame()
{
    if (m_StartupHandle == vr::k_ulOverlayHandleInvalid)
        return;
    CreateBlankTexture();
    if (!m_BlankTexture.m_Texture)
        return;
    vr::VRCompositor()->WaitGetPoses(nullptr, 0, nullptr, 0);
    vr::VRCompositor()->Submit(vr::Eye_Left, &m_BlankTexture.m_VRTexture, nullptr, vr::Submit_Default);
    vr::VRCompositor()->Submit(vr::Eye_Right, &m_BlankTexture.m_VRTexture, nullptr, vr::Submit_Default);
}

void VR::EndStartupFrames()
{
    if (m_StartupHandle == vr::k_ulOverlayHandleInvalid)
        return;
    vr::VROverlay()->DestroyOverlay(m_StartupHandle);
    m_StartupHandle = vr::k_ulOverlayHandleInvalid;
    if (m_StartupCopy.m_Texture)
        m_StartupCopy.m_Texture->Release();
    m_StartupCopy = SharedTextureHolder();
    std::cout << "[VR] startup video overlay removed\n";
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
            InitOpenVR();
            width = m_RenderWidth;
            height = m_RenderHeight;
        };

    D3D11Hooks::Present_Pre_Callback = [this](IDXGISwapChain *swapChain)
        {
            if (m_ShutDown)
                return;
            if (m_IsInitialized)
            {
                ConVars::Sync(*this);
                UpdateMenuFormat();
                CopyBackBuffer(swapChain);
                PrepareWindowImage(swapChain);
                EndStartupFrames();
            }
            else
                ShowStartupFrame(swapChain);
        };

    D3D11Hooks::Present_Post_Callback = [this](IDXGISwapChain *)
        {
            if (m_ShutDown)
                return;
            if (m_IsInitialized)
                Update();
            else
                SubmitStartupFrame();
        };
}

void VR::InstallApplicationManifest(const char *fileName)
{
    char currentDir[MAX_STR_LEN];
    GetCurrentDirectory(MAX_STR_LEN, currentDir);
    char path[MAX_STR_LEN];
    sprintf_s(path, MAX_STR_LEN, "%s\\VR\\%s", currentDir, fileName);

    vr::VRApplications()->AddApplicationManifest(path);
}

// SeatedMode can change while the game runs; the position is measured anew then, once the poses
// are from the new space: WaitGetPoses still reports the old one for a while (measured).
void VR::UpdateTrackingSpace()
{
    m_TrackingSpaceSettleFrames = (std::max)(m_TrackingSpaceSettleFrames - 1, 0);

    const vr::ETrackingUniverseOrigin wanted = m_SeatedMode ? vr::TrackingUniverseSeated : vr::TrackingUniverseStanding;
    if (vr::VRCompositor()->GetTrackingSpace() == wanted)
        return;
    vr::VRCompositor()->SetTrackingSpace(wanted);
    m_InitialPosReset = false;
    m_TrackingSpaceSettleFrames = 90;
    RepositionOverlays();
    std::cout << "[VR] tracking space: " << (m_SeatedMode ? "seated" : "standing") << "\n";
}

void VR::Update()
{
    if (!m_IsInitialized || !m_Game->m_Initialized)
        return;

    SubmitVRTextures();
    UpdateTrackingSpace();
    UpdatePosesAndActions();
    UpdateHeadTracking();
    UpdateHandTracking();

    const bool menuVisible = IsMenuVisible();
    static int lastMenuVisible = -1;
    if ((int)menuVisible != lastMenuVisible)
    {
        std::cout << "[VR] menu input: menu visible=" << menuVisible << "\n";
        lastMenuVisible = menuVisible;
    }
    const bool videoVisible = StrataHooks::VideoVisible();
    static bool lastVideoVisible = false;
    if (videoVisible != lastVideoVisible)
    {
        std::cout << "[VR] video visible=" << videoVisible << "\n";
        lastVideoVisible = videoVisible;
    }

    // Not in the menu: the headset may still lie somewhere when the game starts.
    if (!m_InitialPosReset && !menuVisible && !m_TrackingSpaceSettleFrames && m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
    {
        ResetPosition();
        m_InitialPosReset = true;
    }
    if (m_InitialPosReset)
        LogHintButtons(menuVisible);

    UpdateNotice(menuVisible);

    s_MenuVisible = menuVisible;
    if (menuVisible) {
        m_SmoothTurnRate = 0;
        ProcessMenuInput();
    } else {
        // ProcessMenuInput may have left the overlay interactive (laser); not outside the menus.
        vr::VROverlay()->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);
        ProcessInput();
    }
    UpdateRumble(menuVisible);
}

// What the compositor gets: CopyBackBufferToEye fills them, the game doesn't know them.
void VR::CreateVRTextures()
{
    m_CreatedVRTextures = true; // once, also if it fails

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_RenderWidth;
    desc.Height = m_RenderHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS; // copies from the back buffer need the same family
    desc.SampleDesc = { 1, 0 };
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (SharedTextureHolder *eye : { &m_LeftEye, &m_RightEye })
    {
        ID3D11Texture2D *texture = nullptr;
        if (FAILED(D3D11Hooks::g_Device->CreateTexture2D(&desc, nullptr, &texture)))
            continue;
        eye->m_Texture = texture;
        eye->m_Width = desc.Width;
        eye->m_Height = desc.Height;
        eye->m_VRTexture.handle = texture;
    }

    std::cout << "[VR] eye textures " << m_RenderWidth << "x" << m_RenderHeight << ": " << m_LeftEye.m_Texture << "/" << m_RightEye.m_Texture << "\n";
}

void VR::SubmitVRTextures()
{
    if (!m_RenderedNewFrame)
    {
        CreateBlankTexture();
        if (!m_BackBufferCopy.m_Texture || !m_BlankTexture.m_Texture)
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
            const float uMax = (float)windowWidth / m_BackBufferCopy.m_Width;
            const float vMax = (float)windowHeight / m_BackBufferCopy.m_Height;
            bounds.uMax = uMax < 1.0f ? uMax : 1.0f;
            bounds.vMax = vMax < 1.0f ? vMax : 1.0f;
        }

        vr::VROverlay()->SetOverlayTextureBounds(m_MainMenuHandle, &bounds);
        vr::EVROverlayError overlayError = vr::VROverlay()->SetOverlayTexture(m_MainMenuHandle, &m_BackBufferCopy.m_VRTexture);
        vr::VROverlay()->ShowOverlay(m_MainMenuHandle);
        HideAimDot();
        HideVignette();
        HideHud();

        vr::EVRCompositorError submitError = vr::VRCompositor()->Submit(vr::Eye_Left, &m_BlankTexture.m_VRTexture, NULL, vr::Submit_Default);
        vr::VRCompositor()->Submit(vr::Eye_Right, &m_BlankTexture.m_VRTexture, NULL, vr::Submit_Default);

        // Diagnostics: the first overlay frame and every change of the result codes.
        static int lastOverlayError = -1, lastSubmitError = -1, lastInGame = -1, lastMenu = -1;
        const int inGame = m_Game->m_EngineClient->IsInGame() ? 1 : 0;
        if (overlayError != lastOverlayError || submitError != lastSubmitError || inGame != lastInGame || (int)menuVisible != lastMenu)
        {
            lastMenu = menuVisible;
            std::cout << "[VR] overlay frame: inGame=" << inGame << " menu=" << menuVisible << " backbuffer " << m_BackBufferCopy.m_Width << "x"
                      << m_BackBufferCopy.m_Height << " bounds u<" << bounds.uMax << " v<" << bounds.vMax
                      << " SetOverlayTexture=" << overlayError << " Submit=" << submitError << "\n";
            lastOverlayError = overlayError;
            lastSubmitError = submitError;
            lastInGame = inGame;
        }

        return;
    }
    vr::VROverlay()->HideOverlay(m_MainMenuHandle);

    vr::VRCompositor()->Submit(vr::Eye_Left, &m_LeftEye.m_VRTexture, &(m_TextureBounds)[0], vr::Submit_Default);
    vr::VRCompositor()->Submit(vr::Eye_Right, &m_RightEye.m_VRTexture, &(m_TextureBounds)[1], vr::Submit_Default);
    ShowHud();

    m_RenderedNewFrame = false;
}

void VR::GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut)
{
    if (poseRaw.bPoseIsValid)
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        QAngle ang;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(mat.m[1][2]) * (180.0f / 3.141592654f);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0f / 3.141592654f);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0f / 3.141592654f);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceAng = ang;
    }
}

Vector VR::GetRightControllerAbsPos(Vector eyePosition)
{
    Vector offset = eyePosition;

    if (offset.x == 0 && offset.y == 0 && offset.z == 0)
        offset = m_SetupOrigin;

    Vector position = offset + m_RightControllerPosRel;

    if (m_6DOF)
        position += m_HmdPosRelative;

    return position;
}

Vector VR::GetLeftControllerAbsPos(Vector eyePosition)
{
    Vector position = eyePosition + m_LeftControllerPosRel;
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
    // The offsets fit the right hand. The left controller is its mirror image, also where the tip
    // sits (Touch: 1.7 cm inward of the controller's origin), the unmirrored gun not: without this
    // it sat 3.3 cm too far right in the left hand.
    if (m_LeftHanded)
        viewmodelPos += m_RightControllerRight * (2.0f * m_TipSideOffset[1] * m_VRScale);

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

    hmdAngLocal.x += m_RotationOffset.x;
    hmdAngLocal.y += m_RotationOffset.y;
    hmdAngLocal.z += m_RotationOffset.z;

    QAngle::AngleVectors(hmdAngLocal, &m_HmdForward, &m_HmdRight, &m_HmdUp);

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

    Vector hmdPosCorrected = hmdPosCentered;
    VectorPivotXY(hmdPosCorrected, { 0, 0, 0 }, m_RotationOffset.y);

    UpdateHMDAngles();

    m_HmdPosRelative = hmdPosCorrected * m_VRScale;
    // 64 is the game's eye height above the player's feet. Standing, the HMD's height above the
    // floor counts; seated, its height above the reset position plus the eyes of the body height.
    if (!m_SeatedMode)
        m_HmdPosRelative.z -= 64;
    else
        m_HmdPosRelative.z += (m_BodyHeight - kEyeBelowCrown) / 100 * m_VRScale - 64;
    m_HmdPosRelative.z += m_EyeLift;

    GetViewParameters();
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;
}

// Controller poses -> aim and viewmodel directions, positions relative to the HMD. Needs nothing
// from the game.
void VR::UpdateHandTracking()
{
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;

    QAngle leftControllerAngLocal = m_LeftControllerPose.TrackedDeviceAng;

    Vector rightControllerPosLocal = m_RightControllerPose.TrackedDevicePos;
    QAngle rightControllerAngLocal = m_RightControllerPose.TrackedDeviceAng;

    Vector hmdToController = rightControllerPosLocal - hmdPosLocal;

    // When using stick turning, pivot the controllers around the HMD
    VectorPivotXY(hmdToController, { 0, 0, 0 }, m_RotationOffset.y);

    m_RightControllerPosRel = hmdToController * m_VRScale;

    Vector hmdToLeftController = m_LeftControllerPose.TrackedDevicePos - hmdPosLocal;
    VectorPivotXY(hmdToLeftController, { 0, 0, 0 }, m_RotationOffset.y);
    m_LeftControllerPosRel = hmdToLeftController * m_VRScale;

    rightControllerAngLocal.x += m_RotationOffset.x;
    rightControllerAngLocal.y += m_RotationOffset.y;
    rightControllerAngLocal.z += m_RotationOffset.z;

    // Same for the left hand (it sets the walking direction).
    leftControllerAngLocal.x += m_RotationOffset.x;
    leftControllerAngLocal.y += m_RotationOffset.y;
    leftControllerAngLocal.z += m_RotationOffset.z;

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
    m_ViewmodelPosOffset = viewmodelIdle.position + (viewmodelCarrying.position - viewmodelIdle.position) * w + m_GunPosCustomOffset;
    m_ViewmodelAngOffset.x = viewmodelIdle.angle.x + (viewmodelCarrying.angle.x - viewmodelIdle.angle.x) * w + m_GunAngCustomOffset.x;
    m_ViewmodelAngOffset.y = viewmodelIdle.angle.y + (viewmodelCarrying.angle.y - viewmodelIdle.angle.y) * w + m_GunAngCustomOffset.y;
    m_ViewmodelAngOffset.z = viewmodelIdle.angle.z + (viewmodelCarrying.angle.z - viewmodelIdle.angle.z) * w + m_GunAngCustomOffset.z;

    m_ViewmodelForward = m_RightControllerForward;
    m_ViewmodelUp = m_RightControllerUp;
    m_ViewmodelRight = m_RightControllerRight;

    TurnByOffset(m_ViewmodelAngOffset, m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelUp);

    // The hand's gun: idle, with the default offsets.
    m_HandGunForward = m_RightControllerForward;
    m_HandGunUp = m_RightControllerUp;
    m_HandGunRight = m_RightControllerRight;
    TurnByOffset(viewmodelIdle.angle, m_HandGunForward, m_HandGunRight, m_HandGunUp);
    m_HandGunPosOffset = viewmodelIdle.position + m_HandPosCustomOffset;
}

// Yaw, pitch, then roll.
void VR::TurnByOffset(const QAngle &offset, Vector &forward, Vector &right, Vector &up)
{
    forward = VectorRotate(forward, up, offset.y);
    right = VectorRotate(right, up, offset.y);
    forward = VectorRotate(forward, right, offset.x);
    up = VectorRotate(up, right, offset.x);
    right = VectorRotate(right, forward, offset.z);
    up = VectorRotate(up, forward, offset.z);
}

Vector VR::GetHandGunAbsPos(Vector eyePosition)
{
    Vector position = GetRightControllerAbsPos(eyePosition);
    position -= m_HandGunForward * m_HandGunPosOffset.x;
    position -= m_HandGunRight * m_HandGunPosOffset.y;
    position -= m_HandGunUp * m_HandGunPosOffset.z;
    if (m_LeftHanded) // as in GetRecommendedViewmodelAbsPos
        position += m_RightControllerRight * (2.0f * m_TipSideOffset[1] * m_VRScale);
    return position;
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
