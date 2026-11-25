// The overlays: the menu, the aim dot, the vignette, the HUD and the notice after a game update.
#include "vr.h"
#include <Windows.h>
#include "game.h"
#include "strata_hooks.h"
#include "log.h"
#include "d3d11_hooks.h"
#include "settings_menu.h"
#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstring>

void VR::RepositionOverlays()
{
    const vr::ETrackingUniverseOrigin trackingOrigin = vr::VRCompositor()->GetTrackingSpace();
    const vr::HmdMatrix34_t menuTransform = MenuTransform(trackingOrigin);
    vr::VROverlay()->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
    vr::VROverlay()->SetOverlayWidthInMeters(m_MainMenuHandle, kMenuOverlayHeight * m_RenderWidth / MenuHeight());
}

// Where the menu overlay goes, for the head's current position.
vr::HmdMatrix34_t VR::MenuTransform(vr::ETrackingUniverseOrigin trackingOrigin, bool *poseValid)
{
    // Asked for directly: right after a change of the tracking space, m_Poses are from the old one.
    vr::TrackedDevicePose_t hmdPose = {};
    m_System->GetDeviceToAbsoluteTrackingPose(trackingOrigin, 0, &hmdPose, 1);
    if (poseValid)
        *poseValid = hmdPose.bPoseIsValid;
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = { hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] };
    Vector hmdForward = { -hmdMat.m[0][2], 0, -hmdMat.m[2][2] };

    static float loggedHeight = 1000;
    if (std::abs(hmdPosition.y - loggedHeight) > 0.05f)
    {
        std::cout << "[VR] menu placed for a head at height " << hmdPosition.y << " m, tracking space " << trackingOrigin << ", pose valid " << hmdPose.bPoseIsValid << "\n";
        loggedHeight = hmdPosition.y;
    }

    hmdForward[1] = 0;
    VectorNormalize(hmdForward);

    // Main menu overlay: 3 m in front of the HMD, turned towards it (yaw only). A rigid
    // transform: the overlay's size comes from SetOverlayWidthInMeters, its aspect ratio from
    // the texture. With a scaled matrix, the visible area and ComputeOverlayIntersection don't
    // match: no laser in the menu.
    Vector menuNewPos = hmdForward * 3 + hmdPosition;
    float hmdYaw = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);

    return
    {
        cosf(hmdYaw),  0.0f, sinf(hmdYaw), menuNewPos.x,
        0.0f,          1.0f, 0.0f,         menuNewPos.y - 0.25f,
        -sinf(hmdYaw), 0.0f, cosf(hmdYaw), menuNewPos.z
    };
}

// The menu takes the aspect ratio of the video mode, which the video settings can change (see
// StrataHooks::PanoramaHeight). The overlay keeps its height and gets wider or narrower; the laser
// and the window's mouse follow the new part of the back buffer.
void VR::UpdateMenuFormat()
{
    const uint32_t height = MenuHeight();
    if (height == m_MenuFormatHeight || height == 0)
        return;
    m_MenuFormatHeight = height;

    const vr::HmdVector2_t mouseScale = { (float)m_RenderWidth, (float)height };
    m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScale);
    m_Overlay->SetOverlayWidthInMeters(m_MainMenuHandle, kMenuOverlayHeight * m_RenderWidth / height);
    s_MenuPartWidth = m_RenderWidth;
    s_MenuPartHeight = height;
    std::cout << "[VR] menu format " << m_RenderWidth << "x" << height << "\n";
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
    if (VerboseLog())
        std::cout << "[VR] vignette for " << std::chrono::duration<float>(std::chrono::steady_clock::now() - m_VignetteShownAt).count()
                  << " s, up to " << m_VignettePeak << "\n";
}

// Fixed to the head, in front of it (PlaceHudOverlay).
void VR::CreateHudOverlay()
{
    const vr::EVROverlayError error = m_Overlay->CreateOverlay("HudOverlayKey", "HudOverlay", &m_HudHandle);
    std::cout << "[VR] HUD overlay: " << m_Overlay->GetOverlayErrorNameFromEnum(error) << "\n";
    if (error != vr::VROverlayError_None)
    {
        m_HudHandle = vr::k_ulOverlayHandleInvalid;
        return;
    }
    PlaceHudOverlay();
    // Drawn over transparent black, the colors are multiplied by the alpha VGUI writes. If it
    // writes none, the alpha stays 0 and the HUD adds its light to the scene.
    m_Overlay->SetOverlayFlag(m_HudHandle, vr::VROverlayFlags_IsPremultiplied, true);
    m_Overlay->SetOverlaySortOrder(m_HudHandle, 2); // over the vignette and the aim dot
}

// At HudDistance, as wide as a monitor seen from the usual distance at any distance.
void VR::PlaceHudOverlay()
{
    constexpr float angle = 60.0f; // horizontal, degrees
    const float distance = std::clamp(m_HudDistance, 0.3f, 5.0f);
    const vr::HmdMatrix34_t inFront =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, -distance
    };
    m_Overlay->SetOverlayTransformTrackedDeviceRelative(m_HudHandle, vr::k_unTrackedDeviceIndex_Hmd, &inFront);
    m_Overlay->SetOverlayWidthInMeters(m_HudHandle, 2 * distance * tanf(DEG2RAD(angle / 2)));
    m_HudPlacedDistance = m_HudDistance;
}

void VR::SetHudRect(int x, int y, int width, int height)
{
    m_HudRect[0] = x;
    m_HudRect[1] = y;
    m_HudRect[2] = width;
    m_HudRect[3] = height;
}

static DXGI_FORMAT TypelessOf(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    default:
        return format;
    }
}

// Called by the VGui_Paint hook before the HUD is drawn: binds m_HudTarget instead of the game's
// target, cleared to transparent black. False if the game's target can't be replaced.
// overHud: for the full screen panels (captions) after the HUD of the frame, which stays.
bool VR::BeginHudPaint(bool overHud)
{
    if (overHud && (!m_RenderedHud || !m_HudTargetView))
        return false;
    ID3D11DeviceContext *context = D3D11Hooks::g_Context;
    if (!context || m_HudHandle == vr::k_ulOverlayHandleInvalid)
        return false;
    ID3D11RenderTargetView *view = nullptr;
    ID3D11DepthStencilView *depth = nullptr;
    context->OMGetRenderTargets(1, &view, &depth);
    auto release = [&]
        {
            if (view)
                view->Release();
            if (depth)
                depth->Release();
        };
    if (!view)
    {
        release();
        return false;
    }

    ID3D11Resource *resource = nullptr;
    view->GetResource(&resource);
    ID3D11Texture2D *texture = nullptr;
    const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&texture);
    resource->Release();
    if (FAILED(hr))
    {
        release();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    texture->Release(); // the view keeps it
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc;
    view->GetDesc(&viewDesc);
    // The HUD is laid out for the window, which can be wider than the eyes.
    const UINT width = (std::max)(desc.Width, (UINT)(std::max)(0, m_HudRect[0] + m_HudRect[2]));
    const UINT height = (std::max)(desc.Height, (UINT)(std::max)(0, m_HudRect[1] + m_HudRect[3]));

    static DXGI_FORMAT targetViewFormat = DXGI_FORMAT_UNKNOWN;
    if (overHud)
    {
        static bool loggedOver = false;
        if (!loggedOver)
        {
            std::cout << "[VR] full screen panels over the HUD: game's target " << desc.Width << "x" << desc.Height << " format " << desc.Format
                      << " view " << viewDesc.Format << " samples " << desc.SampleDesc.Count << "\n";
            loggedOver = true;
        }
    }
    else if (!m_HudTarget.m_Texture || m_HudTarget.m_Width != width || m_HudTarget.m_Height != height || targetViewFormat != viewDesc.Format)
    {
        if (m_HudTargetView)
            m_HudTargetView->Release();
        m_HudTargetView = nullptr;
        if (m_HudTarget.m_Texture)
            m_HudTarget.m_Texture->Release();
        m_HudTarget = SharedTextureHolder();

        D3D11_TEXTURE2D_DESC hudDesc = {};
        hudDesc.Width = width;
        hudDesc.Height = height;
        hudDesc.MipLevels = 1;
        hudDesc.ArraySize = 1;
        hudDesc.Format = TypelessOf(viewDesc.Format);
        hudDesc.SampleDesc = { 1, 0 };
        hudDesc.Usage = D3D11_USAGE_DEFAULT;
        hudDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_RENDER_TARGET_VIEW_DESC hudViewDesc = {};
        hudViewDesc.Format = viewDesc.Format;
        hudViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        const bool created = SUCCEEDED(D3D11Hooks::g_Device->CreateTexture2D(&hudDesc, nullptr, &m_HudTarget.m_Texture)) &&
            SUCCEEDED(D3D11Hooks::g_Device->CreateRenderTargetView(m_HudTarget.m_Texture, &hudViewDesc, &m_HudTargetView));
        std::cout << "[VR] HUD target " << width << "x" << height << " format " << hudDesc.Format << ", game's target " << desc.Width << "x"
                  << desc.Height << " format " << desc.Format << " view " << viewDesc.Format << " samples " << desc.SampleDesc.Count
                  << (created ? "" : ": can't create") << "\n";
        targetViewFormat = viewDesc.Format;
        if (!created)
        {
            if (m_HudTarget.m_Texture)
                m_HudTarget.m_Texture->Release();
            m_HudTarget = SharedTextureHolder();
            release();
            return false;
        }
        m_HudTarget.m_Width = width;
        m_HudTarget.m_Height = height;
    }

    static bool logged = false;
    if (!logged)
    {
        D3D11_VIEWPORT viewport = {};
        UINT viewports = 1;
        context->RSGetViewports(&viewports, &viewport);
        std::cout << "[VR] HUD: rect " << m_HudRect[0] << "," << m_HudRect[1] << " " << m_HudRect[2] << "x" << m_HudRect[3] << ", viewport "
                  << viewport.TopLeftX << "," << viewport.TopLeftY << " " << viewport.Width << "x" << viewport.Height << "\n";
        logged = true;
    }

    const float transparent[4] = { 0, 0, 0, 0 };
    context->OMSetRenderTargets(1, &m_HudTargetView, nullptr);
    if (!overHud)
        context->ClearRenderTargetView(m_HudTargetView, transparent);
    // The engine binds its target again while it draws the HUD.
    if (!D3D11Hooks::RedirectRenderTarget(texture, m_HudTargetView))
    {
        context->OMSetRenderTargets(1, &view, depth);
        release();
        return false;
    }
    m_HudSavedView = view;
    m_HudSavedDepth = depth;
    return true;
}

// After the HUD is drawn: the game's target again.
void VR::EndHudPaint(bool overHud)
{
    const D3D11Hooks::RedirectCounts counts = D3D11Hooks::EndRedirect();
    static bool logged[2] = {};
    if (!logged[overHud] && counts.binds)
    {
        std::cout << "[VR] " << (overHud ? "full screen panels" : "HUD") << " drawn: " << counts.binds << " binds of the game's target redirected, "
                  << counts.otherBinds << " of other targets, " << counts.blends << " blend states\n";
        logged[overHud] = true;
    }
    D3D11Hooks::g_Context->OMSetRenderTargets(1, &m_HudSavedView, m_HudSavedDepth);
    m_HudSavedView->Release();
    if (m_HudSavedDepth)
        m_HudSavedDepth->Release();
    m_HudSavedView = nullptr;
    m_HudSavedDepth = nullptr;
    m_RenderedHud = true;
}

// With the eyes of the frame: the HUD's part of m_HudTarget, if it was drawn.
void VR::ShowHud()
{
    if (!m_RenderedHud || !m_HudTarget.m_Texture)
        return HideHud();
    m_RenderedHud = false;

    const int x = std::clamp(m_HudRect[0], 0, (int)m_HudTarget.m_Width);
    const int y = std::clamp(m_HudRect[1], 0, (int)m_HudTarget.m_Height);
    const int width = std::clamp(m_HudRect[2], 0, (int)m_HudTarget.m_Width - x);
    const int height = std::clamp(m_HudRect[3], 0, (int)m_HudTarget.m_Height - y);
    if (!width || !height)
        return HideHud();

    if (!m_HudCopy.m_Texture || m_HudCopy.m_Width != (uint32_t)width || m_HudCopy.m_Height != (uint32_t)height)
    {
        if (m_HudCopy.m_Texture)
            m_HudCopy.m_Texture->Release();
        m_HudCopy = SharedTextureHolder();
        D3D11_TEXTURE2D_DESC desc;
        m_HudTarget.m_Texture->GetDesc(&desc);
        desc.Width = width;
        desc.Height = height;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const bool created = SUCCEEDED(D3D11Hooks::g_Device->CreateTexture2D(&desc, nullptr, &m_HudCopy.m_Texture));
        std::cout << "[VR] HUD overlay " << width << "x" << height << " from " << x << "," << y << (created ? "" : ": can't create") << "\n";
        if (!created)
        {
            m_HudCopy = SharedTextureHolder();
            return HideHud();
        }
        m_HudCopy.m_Width = width;
        m_HudCopy.m_Height = height;
        m_HudCopy.m_VRTexture.handle = m_HudCopy.m_Texture;
    }
    const D3D11_BOX part = { (UINT)x, (UINT)y, 0, (UINT)(x + width), (UINT)(y + height), 1 };
    D3D11Hooks::g_Context->CopySubresourceRegion(m_HudCopy.m_Texture, 0, 0, 0, 0, m_HudTarget.m_Texture, 0, &part);
    if (m_HudDistance != m_HudPlacedDistance)
        PlaceHudOverlay();
    m_Overlay->SetOverlayTexture(m_HudHandle, &m_HudCopy.m_VRTexture);
    m_Overlay->ShowOverlay(m_HudHandle);
}

void VR::HideHud()
{
    m_RenderedHud = false;
    if (m_HudHandle != vr::k_ulOverlayHandleInvalid)
        m_Overlay->HideOverlay(m_HudHandle);
}

// The text (UTF-8) centered in white on dark red, as RGBA for SetOverlayRaw. Empty if GDI fails.
static std::vector<uint8_t> RenderNoticeText(const std::string &utf8, int width, int height)
{
    std::vector<uint8_t> rgba;
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (length <= 1)
        return rgba;
    std::wstring text(length - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, text.data(), length);

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    HFONT font = CreateFontW(-height / 7, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (bitmap && font)
    {
        memset(bits, 0, (size_t)width * height * 4);
        const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        const HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rect = { width / 20, height / 10, width - width / 20, height - height / 10 };
        RECT measured = rect;
        DrawTextW(dc, text.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
        rect.top += (std::max)(0L, ((rect.bottom - rect.top) - (measured.bottom - measured.top)) / 2);
        DrawTextW(dc, text.c_str(), -1, &rect, DT_WORDBREAK | DT_CENTER);
        GdiFlush();
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);

        // Grey antialiasing: any channel is the text's coverage.
        const uint8_t background[3] = { 70, 16, 16 };
        const uint8_t *bgra = static_cast<const uint8_t *>(bits);
        rgba.resize((size_t)width * height * 4);
        for (size_t i = 0; i < (size_t)width * height; ++i)
        {
            const int coverage = (std::max)({ bgra[i * 4], bgra[i * 4 + 1], bgra[i * 4 + 2] });
            for (int c = 0; c < 3; ++c)
                rgba[i * 4 + c] = (uint8_t)(background[c] + (255 - background[c]) * coverage / 255);
            rgba[i * 4 + 3] = 235;
        }
    }
    if (font)
        DeleteObject(font);
    if (bitmap)
        DeleteObject(bitmap);
    if (dc)
        DeleteDC(dc);
    return rgba;
}

// Just above the menu overlay, in the game's language.
bool VR::CreateNotice()
{
    constexpr int width = 1024, height = 256;
    constexpr float widthInMeters = 2.0f;
    const std::vector<uint8_t> pixels = RenderNoticeText(SettingsMenu::Text("GameUpdated"), width, height);
    const vr::EVROverlayError error =
        pixels.empty() ? vr::VROverlayError_InvalidParameter : m_Overlay->CreateOverlay("NoticeOverlayKey", "NoticeOverlay", &m_NoticeHandle);
    std::cout << "[VR] game update notice: " << m_Overlay->GetOverlayErrorNameFromEnum(error) << "\n";
    if (error != vr::VROverlayError_None)
    {
        m_NoticeHandle = vr::k_ulOverlayHandleInvalid;
        return false;
    }
    m_NoticeAboveMenu = kMenuOverlayHeight / 2 + widthInMeters * height / width / 2 + 0.05f;
    m_Overlay->SetOverlayWidthInMeters(m_NoticeHandle, widthInMeters);
    m_Overlay->SetOverlayRaw(m_NoticeHandle, (void *)pixels.data(), width, height, 4);
    m_NoticeLast = std::chrono::steady_clock::now();
    return true;
}

// With the menus, until they were open for kNoticeSeconds; once per game start.
void VR::UpdateNotice(bool menuVisible)
{
    constexpr float kNoticeSeconds = 20;
    if (!m_Game->m_UntestedGameVersion || m_NoticeDone)
        return;
    if (m_NoticeHandle == vr::k_ulOverlayHandleInvalid)
    {
        if (!menuVisible)
            return;
        if (!CreateNotice())
        {
            m_NoticeDone = true;
            return;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (menuVisible)
    {
        m_NoticeSeconds += std::chrono::duration<float>(now - m_NoticeLast).count();
        // Where the menu is (RepositionOverlays moves it), m_NoticeAboveMenu up in its plane and a
        // bit in front of it.
        vr::ETrackingUniverseOrigin origin;
        vr::HmdMatrix34_t transform;
        if (m_Overlay->GetOverlayTransformAbsolute(m_MainMenuHandle, &origin, &transform) == vr::VROverlayError_None)
        {
            const float up = m_NoticeAboveMenu, forward = 0.05f;
            for (int row = 0; row < 3; ++row)
                transform.m[row][3] += transform.m[row][1] * up + transform.m[row][2] * forward;
            m_Overlay->SetOverlayTransformAbsolute(m_NoticeHandle, origin, &transform);
        }
        m_Overlay->ShowOverlay(m_NoticeHandle);
    }
    else
        m_Overlay->HideOverlay(m_NoticeHandle);
    m_NoticeLast = now;
    if (m_NoticeSeconds >= kNoticeSeconds)
    {
        m_Overlay->DestroyOverlay(m_NoticeHandle);
        m_NoticeHandle = vr::k_ulOverlayHandleInvalid;
        m_NoticeDone = true;
    }
}
