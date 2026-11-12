/* CRASHPRB.EXE: a program that ends in a chosen way, for testing
   `v9xctl wait-fault` (and anything else that watches for crashes).

     CRASHPRB            write through a NULL pointer (invalid page fault)
     CRASHPRB div        integer divide by zero
     CRASHPRB exit N     exit cleanly with code N after 2 s
     CRASHPRB box        show an ordinary message box (not a fault), then
                         exit when it is closed
     CRASHPRB hang       sleep for ever (for the deadline path)

   A leading "wait N" (seconds) delays any of them, so a watcher can be
   started first: CRASHPRB wait 5 div.

   GUI subsystem on purpose (no DOS VM, no console). Built with the normal
   Watcom CRT (it is a test tool, not shipped):
     wcl386 -bt=nt -l=nt_win -zq tests\integration\crashprb.c */

#include <stdlib.h>
#include <string.h>
#include <windows.h>

static char *next_word(char **cursor)
{
    char *start = *cursor;
    while (*start == ' ') ++start;
    if (*start == '\0') return NULL;
    *cursor = start;
    while (**cursor != '\0' && **cursor != ' ') ++*cursor;
    if (**cursor != '\0') { **cursor = '\0'; ++*cursor; }
    return start;
}

static volatile int zero = 0;

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command,
                   int show)
{
    char line[256];
    char *cursor = line;
    char *word;
    (void)instance; (void)previous; (void)show;
    strncpy(line, command, sizeof(line) - 1u);
    line[sizeof(line) - 1u] = '\0';
    word = next_word(&cursor);
    if (word != NULL && stricmp(word, "wait") == 0) {
        char *seconds = next_word(&cursor);
        Sleep((DWORD)(seconds != NULL ? atoi(seconds) : 5) * 1000ul);
        word = next_word(&cursor);
    }
    if (word == NULL) {
        *(volatile int *)0 = 1;
        return 1;
    }
    if (stricmp(word, "div") == 0) {
        return 100 / zero;
    }
    if (stricmp(word, "exit") == 0) {
        char *code = next_word(&cursor);
        Sleep(2000ul);
        return code != NULL ? atoi(code) : 0;
    }
    if (stricmp(word, "box") == 0) {
        MessageBoxA(NULL, "An ordinary message, not a fault.", "CRASHPRB",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (stricmp(word, "hang") == 0) {
        for (;;) Sleep(60000ul);
    }
    MessageBoxA(NULL, "usage: CRASHPRB [wait N] [div|exit N|box|hang]",
                "CRASHPRB", MB_OK);
    return 2;
}
