#pragma once

// Keeps the game DLLs the mod hooks in a place where MinHook finds memory for its trampolines.
namespace ModulePlacement
{
    // Thread function, to start before the game loads client.dll.
    unsigned long __stdcall Install(void *);
}
