#pragma once
#include <string>

// Adds a VR tab to the game's settings, without changing the game's files.
namespace SettingsMenu
{
    // Thread function, to start before panorama.dll loads the menu.
    unsigned long __stdcall Install(void *);

    // A text of VR\localization (UTF-8) in the game's language, in English until Steam has told
    // the language; the key if there is none.
    std::string Text(const char *key);
}
