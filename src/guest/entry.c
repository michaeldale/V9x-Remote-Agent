#include "agent.h"

void WINAPI V9xAgentEntry(void)
{
    v9x_agent_run();
    ExitProcess(0ul);
}

