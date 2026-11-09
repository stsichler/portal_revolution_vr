#pragma once
// The mod is loaded as d3d11.dll from the game's bin\win64 (shaderapidx11.dll imports
// D3D11CreateDevice from it). d3d11_proxy.cpp forwards the exports to the system d3d11.dll;
// this layer hooks the created device, its immediate context, the DXGI factory and the swap
// chain, and reports the events the VR code needs through the callbacks below.
#include <d3d11.h>
#include <dxgi1_2.h>
#include <functional>

namespace D3D11Hooks
{
    // Device and immediate context of the game (set on device creation), swap chain (set on
    // swap chain creation).
    extern ID3D11Device *g_Device;
    extern ID3D11DeviceContext *g_Context;
    extern IDXGISwapChain *g_SwapChain;

    // Before a swap chain is created or its buffers are resized: may change the buffer size.
    extern std::function<void(UINT &width, UINT &height)> SwapChainSize_Callback;
    // Before and after IDXGISwapChain::Present/Present1.
    extern std::function<void(IDXGISwapChain *swapChain)> Present_Pre_Callback;
    extern std::function<void(IDXGISwapChain *swapChain)> Present_Post_Callback;

    // Writes a marker into the frame trace, if one is running (F11, see d3d11_hooks.cpp; only
    // with PRVR_DIAGNOSTICS, i.e. not in Release).
    void TraceNote(const char *note);

    // Until EndRedirect, the game's binds of this texture as render target bind the view instead,
    // without depth, and its alpha blending keeps the alpha of what is below (premultiplied colors
    // over transparent black). False if the hooks for it are missing. EndRedirect tells what
    // happened meanwhile.
    struct RedirectCounts
    {
        unsigned binds = 0; // redirected
        unsigned otherBinds = 0; // of other targets
        unsigned blends = 0; // blend states set
    };
    bool RedirectRenderTarget(ID3D11Resource *from, ID3D11RenderTargetView *to);
    RedirectCounts EndRedirect();

    // Called by the proxy after the system's D3D11CreateDevice succeeded.
    void OnDeviceCreated(ID3D11Device *device, ID3D11DeviceContext *context);
    // Called by the proxy after the system's D3D11CreateDeviceAndSwapChain succeeded.
    void OnSwapChainCreated(IDXGISwapChain *swapChain);
}
