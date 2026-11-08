#pragma once

// Adds a VR tab to the game's settings, without changing the game's files.
namespace SettingsMenu
{
    // Thread function, to start before panorama.dll loads the menu.
    unsigned long __stdcall Install(void *);
}
