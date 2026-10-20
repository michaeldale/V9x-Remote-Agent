/* MODESET.EXE: switch the display resolution for a few seconds, then back.

   Exercises V9XVNC.EXE's mode-change path (the DesktopSize pseudo-encoding)
   without a person in Display Properties. Usage:

     MODESET.EXE <width> <height> [seconds]

   Changes the mode for this session only (no registry write), waits
   `seconds` (default 8), restores the registry mode and exits 0; any failed
   ChangeDisplaySettings returns its DISP_CHANGE code + 100.

   GUI subsystem so it starts no DOS VM. Built with the normal Watcom CRT
   (a test tool, not shipped):
     wcl386 -bt=nt -l=nt_win -zq tests\integration\modeset.c */

#include <stdlib.h>
#include <windows.h>

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    DEVMODE mode;
    LONG result;
    char *next;
    long width;
    long height;
    long seconds;
    (void)instance;
    (void)previous;
    (void)show;
    width = strtol(command, &next, 10);
    height = strtol(next, &next, 10);
    seconds = strtol(next, &next, 10);
    if (width <= 0 || height <= 0) return 1;
    if (seconds <= 0) seconds = 8;
    ZeroMemory(&mode, sizeof(mode));
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = (DWORD)width;
    mode.dmPelsHeight = (DWORD)height;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    result = ChangeDisplaySettings(&mode, 0);
    if (result != DISP_CHANGE_SUCCESSFUL) return (int)(100 + result);
    Sleep((DWORD)seconds * 1000ul);
    result = ChangeDisplaySettings(0, 0);
    if (result != DISP_CHANGE_SUCCESSFUL) return (int)(100 + result);
    return 0;
}
