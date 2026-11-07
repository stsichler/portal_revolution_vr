#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <string>
#include "game.h"
#include "strata_hooks.h"
#include "module_placement.h"

// Writes everything to the log file (b) and, if there is one, also to a second stream buffer
// (a, the console).
class TeeBuf : public std::streambuf
{
public:
    TeeBuf(std::streambuf *a, std::streambuf *b) : m_A(a), m_B(b) {}

    void SetFirst(std::streambuf *a) { m_A = a; }

protected:
    int overflow(int c) override
    {
        if (c == EOF)
            return !EOF;
        const int r1 = m_A ? m_A->sputc((char)c) : c;
        const int r2 = m_B->sputc((char)c);
        return (r1 == EOF || r2 == EOF) ? EOF : c;
    }

    int sync() override
    {
        return ((!m_A || m_A->pubsync() == 0) && m_B->pubsync() == 0) ? 0 : -1;
    }

private:
    std::streambuf *m_A;
    std::streambuf *m_B;
};

static const char *ModuleOf(const void *address, uintptr_t &offset)
{
    static char name[MAX_PATH];
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)address, &module) ||
        !GetModuleFileNameA(module, name, MAX_PATH))
        return nullptr;
    offset = (uintptr_t)address - (uintptr_t)module;
    const char *slash = strrchr(name, '\\');
    return slash ? slash + 1 : name;
}

// The engine installs its own unhandled exception filter, so use a vectored handler to see the
// crash first. Exceptions that the code handles itself get logged too; the last one is the crash.
// Limited, so that repeated handled ones can't fill the disk of a player.
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS *ep)
{
    static LONG logged = 0;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&logged) > 100)
        return EXCEPTION_CONTINUE_SEARCH;

    const void *address = ep->ExceptionRecord->ExceptionAddress;
    uintptr_t offset = 0;
    const char *module = ModuleOf(address, offset);
    std::cout << std::hex << "[CRASH] code=0x" << code << " at 0x" << (uintptr_t)address << " "
              << (module ? module : "?") << "+0x" << offset;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        std::cout << " (" << (ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write")
                  << " of 0x" << ep->ExceptionRecord->ExceptionInformation[1] << ")";
    const CONTEXT *c = ep->ContextRecord;
    std::cout << "\n[CRASH] rax=" << c->Rax << " rbx=" << c->Rbx << " rcx=" << c->Rcx << " rdx=" << c->Rdx
              << " rsi=" << c->Rsi << " rdi=" << c->Rdi << " rbp=" << c->Rbp << " rsp=" << c->Rsp
              << "\n[CRASH] r8=" << c->R8 << " r9=" << c->R9 << " r10=" << c->R10 << " r11=" << c->R11
              << " r12=" << c->R12 << " r13=" << c->R13 << " r14=" << c->R14 << " r15=" << c->R15 << "\n";

    // Poor man's backtrace: stack words that point into executable module memory.
    const uintptr_t *stack = (const uintptr_t *)c->Rsp;
    int found = 0;
    for (int i = 0; i < 512 && found < 24; ++i)
    {
        uintptr_t word;
        MEMORY_BASIC_INFORMATION mbi;
        if (!ReadProcessMemory(GetCurrentProcess(), stack + i, &word, sizeof(word), nullptr) ||
            !VirtualQuery((LPCVOID)word, &mbi, sizeof(mbi)) ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            continue;
        uintptr_t off = 0;
        if (const char *m = ModuleOf((const void *)word, off))
        {
            std::cout << "[CRASH]   rsp+0x" << i * sizeof(uintptr_t) << ": " << m << "+0x" << off << "\n";
            ++found;
        }
    }
    std::cout << std::dec << std::flush;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The first entries of tier0's ICommandLine. Checked against the vtable of CCommandLine in
// Portal Revolution's tier0.dll (Strata Source): like CS:GO it has HasParm after CheckParm,
// which shifts RemoveParm and AppendParm by one compared to the Source SDK 2013.
class ICommandLine
{
public:
    virtual void CreateCmdLine(const char *commandline) = 0;
    virtual void CreateCmdLineArgv(int argc, char **argv) = 0;
    virtual const char *GetCmdLine() const = 0;
    virtual const char *CheckParm(const char *psz, const char **ppszValue = nullptr) const = 0;
    virtual bool HasParm(const char *psz) const = 0;
    virtual void RemoveParm(const char *parm) = 0;
    virtual void AppendParm(const char *pszParm, const char *pszValues) = 0;
};

// What AddLaunchOptions did, written to the log once the log is open.
static std::string g_LaunchOptionsLog;

// The value for the game's mat_antialias: AntiAliasing from the mod's config. The game needs it at
// its start, before the mod reads the config otherwise.
static const char *ConfiguredAntiAliasing()
{
    static const char kKey[] = "AntiAliasing=";
    std::ifstream config("VR\\config.txt");
    std::string line;
    while (std::getline(config, line))
    {
        if (line.rfind(kKey, 0) != 0)
            continue;
        switch (atoi(line.c_str() + sizeof(kKey) - 1))
        {
        case 0: return "0";
        case 4: return "4";
        case 8: return "8";
        }
        break;
    }
    return "2";
}

// Adds the launch options the mod needs to the engine's command line, so players don't have to
// set them in Steam. Runs in DllMain: d3d11.dll is a static import of shaderapidx11.dll, which the
// launcher loads before the engine reads the video mode options, starts the intro video or runs
// the "+" commands. Options the player set (also contrary ones) are left alone.
// Returns false if tier0's command line isn't available.
static bool AddLaunchOptions()
{
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    typedef ICommandLine *(*tCommandLine)();
    tCommandLine getCommandLine = tier0 ? (tCommandLine)GetProcAddress(tier0, "CommandLine") : nullptr;
    ICommandLine *cmdLine = getCommandLine ? getCommandLine() : nullptr;
    if (!cmdLine)
        return false;

    auto hasAny = [cmdLine](std::initializer_list<const char *> parms)
    {
        for (const char *parm : parms)
            if (cmdLine->CheckParm(parm))
                return true;
        return false;
    };
    auto append = [cmdLine](const char *parm, const char *value)
    {
        cmdLine->AppendParm(parm, value);
        g_LaunchOptionsLog += std::string(" ") + parm + (value ? std::string(" ") + value : "");
    };

    if (!hasAny({"-insecure"}))
        append("-insecure", nullptr);
    // Window mode and size: the game's video settings, they only affect the window on the monitor.
    if (!hasAny({"-novid"}))
        append("-novid", nullptr);
#ifdef PRVR_DIAGNOSTICS
    // The game's console, in <game>\revolution\console.log.
    if (!hasAny({"-condebug"}))
        append("-condebug", nullptr);
#endif

    static const char *const convars[][2] = {
        {"+mat_motion_blur_percent_of_screen_max", "0"},
        {"+mat_queue_mode", "0"},
        {"+mat_vsync", "0"},
        {"+mat_grain_scale_override", "0"},
    };
    for (const auto &convar : convars)
        if (!hasAny({convar[0]}))
            append(convar[0], convar[1]);
    if (!hasAny({"+mat_antialias"}))
        append("+mat_antialias", ConfiguredAntiAliasing());

    g_LaunchOptionsLog = "[LaunchOptions] added:" + (g_LaunchOptionsLog.empty() ? " (none)" : g_LaunchOptionsLog) +
                         "\n[LaunchOptions] command line: " + cmdLine->GetCmdLine() + "\n";
    return true;
}

// Everything written to std::cout goes to prvr_debug.log, in every build: players attach it to a
// report. It starts over with each game start and is flushed after every write, so it survives a
// crash. Log events, not frames, to keep it small.
static void SetupLog()
{
    static std::ofstream logFile("prvr_debug.log", std::ios::trunc);
    static TeeBuf teeBuf(nullptr, logFile.rdbuf());
#ifdef PRVR_DIAGNOSTICS
    // Only if the environment variable PRVR_CONSOLE is set: the window takes the focus from the
    // game, and with Steam Link that made SteamVR open its dashboard right when the game menu
    // appeared.
    if (GetEnvironmentVariableA("PRVR_CONSOLE", nullptr, 0) > 0)
    {
        AllocConsole();
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        teeBuf.SetFirst(std::cout.rdbuf());
    }
#endif
    std::cout.rdbuf(&teeBuf);
    std::cout << std::unitbuf;
    AddVectoredExceptionHandler(1, CrashLogger);
}

DWORD WINAPI InitPRVR(HMODULE hModule)
{
    g_Game->init();
    return 0;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
            if (!AddLaunchOptions())
            {
                // Without tier0 there's no way to add -insecure, so the player has to.
                if (!wcsstr(GetCommandLineW(), L"-insecure"))
                    ExitProcess(0);
                g_LaunchOptionsLog = "[LaunchOptions] tier0 CommandLine() not found, nothing added\n";
            }
            SetupLog();
            std::cout << "[Mod] Portal Revolution VR " PRVR_VERSION "\n" << g_LaunchOptionsLog;
            // Synchronously: the D3D11 callbacks must be set before shaderapidx11 creates the device.
            g_Game = new Game();
            CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitPRVR, hModule, 0, NULL);
            CreateThread(NULL, 0, ModulePlacement::Install, nullptr, 0, NULL);
            CreateThread(NULL, 0, StrataHooks::HookPanorama, nullptr, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
