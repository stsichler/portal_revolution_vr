#pragma once
#include <cstdint>
#include "vector.h"

class IClientEntityList;
class IEngineTrace;
class IEngineClient;
class IEngineVGui;
class IBaseClientDLL;
class CBaseEntity;
class IInput;
class ISurface;

class Game;
class VR;

inline Game *g_Game;

class Game
{
public:
    IClientEntityList* m_ClientEntityList = nullptr;
    IEngineTrace* m_EngineTrace = nullptr;
    IEngineClient* m_EngineClient = nullptr;
    IEngineVGui* m_EngineVGui = nullptr;
    IBaseClientDLL* m_BaseClientDll = nullptr;
    IInput* m_VguiInput = nullptr;
    ISurface* m_VguiSurface = nullptr;

    Vector m_singlePlayerPortalColors[3] = { Vector(255.0f, 255.0f, 255.0f), Vector(64.0f, 160.0f, 255.0f), Vector(255.0f, 160.0f, 32.0f) };

    VR *m_VR = nullptr;

    bool m_Initialized = false;

    Game();
    void init();

    void *GetInterface(const char *dllname, const char *interfacename);

    static void errorMsg(const char *msg);

    CBaseEntity *GetClientEntity(int entityIndex);
    void ClientCmd_Unrestricted(const char *szCmdString);

    typedef void *(__cdecl *tCreateInterface)(const char *name, int *returnCode);
};
