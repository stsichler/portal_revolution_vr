#include "game.h"
#include <Windows.h>
#include <initializer_list>
#include <iostream>
#include <string>
#include "sdk.h"
#include "vr.h"
#include "strata_hooks.h"

// Runs in DllMain: the D3D11 callbacks must be in place before the game creates its device.
Game::Game()
{
    m_VR = new VR(this);
    m_VR->initD3D11Callbacks();
}

// Runs in a thread of its own, waits for the game's DLLs.
void Game::init()
{
    for (const char *dll : { "client.dll", "engine.dll", "materialsystem.dll", "server.dll", "vgui2.dll", "vguimatsurface.dll" })
    {
        while (!GetModuleHandle(dll))
            Sleep(50);
    }

    CheckGameVersion();

    m_ClientEntityList = (IClientEntityList *)GetInterface("client.dll", "VClientEntityList003");
    m_BaseClientDll = (IBaseClientDLL *)GetInterface("client.dll", "VClient018");
    m_EngineTrace = (IEngineTrace *)GetInterface("engine.dll", "EngineTraceClient004");
    m_EngineClient = (IEngineClient *)GetInterface("engine.dll", "VEngineClient015");
    m_EngineVGui = (IEngineVGui *)GetInterface("engine.dll", "VEngineVGui001");
    m_VguiInput = (IInput *)GetInterface("vgui2.dll", "VGUI_InputInternal001");
    m_VguiSurface = (ISurface *)GetInterface("vguimatsurface.dll", "VGUI_Surface031");

    std::cout << "[Game] interfaces: EngineClient=" << m_EngineClient << " EngineVGui=" << m_EngineVGui << " ClientEntityList=" << m_ClientEntityList
              << " VguiInput=" << m_VguiInput << " VguiSurface=" << m_VguiSurface << "\n";

    m_VR->init();
    StrataHooks::Init(this);

    m_Initialized = true;
}

// The mod depends on fixed positions in the game's DLLs (vtable indices, offsets), checked with
// Steam build 16912226 of the game. After an update they may have moved: the build times of the
// DLLs tell.
void Game::CheckGameVersion()
{
    static const struct
    {
        const char *dll;
        DWORD built;
    } kTested[] = { // PE TimeDateStamp, Unix time: all 2025-01-05 00:54-00:57 UTC
        {"client.dll", 1736038611},   {"engine.dll", 1736038585},         {"server.dll", 1736038626},
        {"vstdlib.dll", 1736038502},  {"tier0.dll", 1736038483},          {"materialsystem.dll", 1736038553},
        {"vgui2.dll", 1736038536},    {"vguimatsurface.dll", 1736038537}, {"panorama.dll", 1736038584},
    };
    std::string differing;
    for (const auto &tested : kTested)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandle(tested.dll));
        if (!base)
        {
            differing += std::string(" ") + tested.dll + " not loaded;";
            continue;
        }
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        const DWORD built = nt->FileHeader.TimeDateStamp;
        if (built != tested.built)
            differing += std::string(" ") + tested.dll + " built at " + std::to_string(built) + ", tested " + std::to_string(tested.built) + ";";
    }
    m_UntestedGameVersion = !differing.empty();
    if (m_UntestedGameVersion)
        std::cout << "[Game] WARNING: not the game version the mod was tested with (Steam build 16912226):" << differing << "\n";
    else
        std::cout << "[Game] the game version the mod was tested with (Steam build 16912226)\n";
}

void *Game::GetInterface(const char *dllname, const char *interfacename)
{
    HMODULE module = GetModuleHandle(dllname);
    tCreateInterface CreateInterface = module ? (tCreateInterface)GetProcAddress(module, "CreateInterface") : nullptr;
    if (!CreateInterface)
    {
        std::cout << "[Game] " << dllname << ": no CreateInterface\n";
        return nullptr;
    }

    int returnCode = 0;
    void *createdInterface = CreateInterface(interfacename, &returnCode);
    if (!createdInterface)
        std::cout << "[Game] " << dllname << ": " << interfacename << " not found\n";

    return createdInterface;
}

void Game::errorMsg(const char *msg)
{
    MessageBox(0, msg, "Portal Revolution VR", MB_ICONERROR | MB_OK);
}

CBaseEntity *Game::GetClientEntity(int entityIndex)
{
    return (CBaseEntity *)(m_ClientEntityList->GetClientEntity(entityIndex));
}

void Game::ClientCmd_Unrestricted(const char *szCmdString)
{
    m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}
