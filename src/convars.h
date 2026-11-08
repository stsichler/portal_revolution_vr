#pragma once

class VR;

// The mod's options as console variables, for the game's settings menu. config.txt stays their
// source: a change of a variable is written to it, a change of the file goes to the variable.
namespace ConVars
{
    // Registers the variables at the first call, then brings variables, VR and config.txt in line.
    // On the game's main thread (the variables aren't thread-safe), once per frame.
    void Sync(VR &vr);
}
