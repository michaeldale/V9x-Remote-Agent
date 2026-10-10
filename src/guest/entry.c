#include "agent.h"

/* "-hotswap" is how the hot update runs the staged binary as a swapper
   instead of as the agent (see update.c). Anything else, including the
   "-service" RunServices launches with, runs the agent. */
static int v9x_entry_is_hotswap(void)
{
    const char *line = GetCommandLineA();
    static const char marker[] = "-hotswap";
    unsigned long i;
    if (line == 0) return 0;
    for (; *line != '\0'; ++line) {
        for (i = 0ul; marker[i] != '\0' && line[i] == marker[i]; ++i) {}
        if (marker[i] == '\0') return 1;
    }
    return 0;
}

void WINAPI V9xAgentEntry(void)
{
    if (v9x_entry_is_hotswap()) {
        ExitProcess(v9x_hotswap_run());
    }
    v9x_agent_run();
    ExitProcess(0ul);
}

