#pragma once

// prvr_debug.log is std::cout (dllmain.cpp). Diagnostic events that come often in normal play
// (walking, shots, menu clicks) are only logged if this is true: in Debug and RelWithDebInfo, in
// Release with the launch option -prvr_verbose. Otherwise the log grows all the time while playing.
bool VerboseLog();
