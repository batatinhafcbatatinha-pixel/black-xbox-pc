/* Win32 for Black's game-side native files: <windows.h> on Windows, xboxrecomp's POSIX layer
 * (platform/win32_compat) elsewhere. The renderer files use hle_d3d11_internal.h instead (DXVK native's headers). */
#ifndef BLACK_WIN32_H
#define BLACK_WIN32_H
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/xbox_winnt.h"
#include "platform/win32_compat.h"
#endif
#endif
