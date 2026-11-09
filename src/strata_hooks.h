#pragma once
#include <cstdint>

class Game;

// Engine hooks for Strata Source (Portal: Revolution, x64). Functions are found through RTTI
// (class name -> vtable) and vtable indices checked in the game's DLLs instead of byte signatures.
namespace StrataHooks
{
    // Returns the vtable of the class with this name in the module, or nullptr. With multiple
    // inheritance, offset selects the one of the base class at that offset in the object.
    void **FindVTable(const char *module, const char *className, uint32_t offset = 0);

    // The number of entries: pointers into the module's code, up to the first that isn't one.
    int VTableLength(void *const *vtable, const char *module);

    // FindVTable, but nullptr (and a line in the log) if the vtable doesn't have the length it had
    // in the game version the indices were checked with: after a game update with added or
    // removed entries the indices would point to other functions.
    void **FindCheckedVTable(const char *module, const char *className, int length, uint32_t offset = 0);

    bool Init(Game *game);

    // Thread function, to start before the game loads panorama.dll: the hooks must be in place
    // when Panorama's windows are created.
    unsigned long __stdcall HookPanorama(void *);

    // A full screen video (VGUI VideoPanel) is on the screen.
    bool VideoVisible();

    // The height of the part of the back buffer Panorama lays out for: in the aspect ratio of the
    // video mode, or all of it if the hooks aren't in place. Changes with the video settings.
    int PanoramaHeight(int width, int height);
}
