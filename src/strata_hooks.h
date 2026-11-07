#pragma once

class Game;

// Engine hooks for Strata Source (Portal: Revolution, x64). Functions are found through RTTI
// (class name -> vtable) and checked vtable indices instead of byte signatures; see dev-notes,
// VTABLES.
namespace StrataHooks
{
    // Returns the vtable of the class with this name in the module, or nullptr.
    void **FindVTable(const char *module, const char *className);

    bool Init(Game *game);

    // Thread function, to start before the game loads panorama.dll: the hooks must be in place
    // when Panorama's windows are created.
    unsigned long __stdcall HookPanorama(void *);

    // The height of the part of the back buffer Panorama lays out for: 16:9, or all of it if the
    // hooks aren't in place.
    int PanoramaHeight(int width, int height);
}
