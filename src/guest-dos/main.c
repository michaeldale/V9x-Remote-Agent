/* Entry point and accept loop for the DOS agent.

   DOS is single-tasking, so this is deliberately simple: bring up the Watt-32
   stack, open one listening socket, and serve exactly one client at a time to
   completion before accepting the next. There are no threads, no connection
   pool, and no singleton mutex - the boot counter file is the only persistent
   state carried across restarts. The agent is launched from AUTOEXEC.BAT after
   the packet driver; see docs\dos-agent.md and packaging\dos\. */

#include <conio.h>
#include <stdio.h>
#include <string.h>

#include "dosagent.h"

/* One large static state block (~85 KiB of buffers). Fine under DOS/4GW's flat
   32-bit model; this is exactly why the protected-mode target was chosen over a
   real-mode one with 64 KiB segment limits. */
static V9xDos g_dos;

int v9x_console_esc(void)
{
    return kbhit() && getch() == 27;
}

int main(void)
{
    int listener;
    int client;
    int waiting;
    unsigned long peer_ip;
    unsigned long ip;
    unsigned long allowed_ip = 0ul;
    int restrict_client;

    memset(&g_dos, 0, sizeof(g_dos));
    g_dos.socket = -1;
    g_dos.write_state.handle = -1;

    g_dos.boot_counter = v9x_dos_increment_boot_counter();
    v9x_log_set_boot(g_dos.boot_counter);
    g_dos.start_ms = v9x_dos_now_ms();
    v9x_dos_load_config(&g_dos);
    v9x_log_line("agent-start");
    v9x_dos_ensure_install_id(&g_dos);

    if (!v9x_net_init()) {
        v9x_log_line("net-init-failed");
        return 1;
    }

    restrict_client = g_dos.allowed_client[0] != '\0';
    if (restrict_client) {
        allowed_ip = v9x_net_inet_addr(g_dos.allowed_client);
        if (allowed_ip == 0xfffffffful) {
            v9x_log_line("allowed-client-invalid");
            restrict_client = 0;
        }
    }

    for (;;) {
        listener = v9x_net_listen(g_dos.listen_port);
        if (listener < 0) {
            v9x_log_line("listen-failed");
            return 1;
        }
        v9x_log_line("listening");
        ip = v9x_net_local_ip();
        printf("V9x DOS agent on %lu.%lu.%lu.%lu:%u - Esc for a DOS prompt\n",
               (ip >> 24) & 0xfful, (ip >> 16) & 0xfful, (ip >> 8) & 0xfful,
               ip & 0xfful, (unsigned)g_dos.listen_port);
        for (;;) {
            /* Wait in slices so a person at the machine can take it back:
               Esc ends the agent and V9XLOOP.BAT drops to a prompt. */
            waiting = v9x_net_wait(listener, 250ul);
            if (waiting < 0) break;
            if (waiting == 0) {
                if (v9x_console_esc()) {
                    v9x_net_close(listener);
                    v9x_log_line("console-quit");
                    return V9X_DOS_EXIT_CONSOLE;
                }
                continue;
            }
            peer_ip = 0ul;
            client = v9x_net_accept(listener, &peer_ip);
            if (client < 0) break;
            if (restrict_client && peer_ip != allowed_ip) {
                v9x_log_line("client-rejected");
                v9x_net_close(client);
                continue;
            }
            v9x_log_line("client-connected");
            g_dos.socket = client;
            v9x_serve_client(&g_dos);
            v9x_files_disconnect(&g_dos);
            v9x_net_close(client);
            g_dos.socket = -1;
            v9x_log_line("client-disconnected");
            if (g_dos.console_quit) {
                v9x_net_close(listener);
                v9x_log_line("console-quit");
                return V9X_DOS_EXIT_CONSOLE;
            }
            if (g_dos.reboot) {
                v9x_net_close(listener);
                v9x_net_drain(1000ul);
                v9x_log_line("reboot");
                v9x_dos_reboot(g_dos.reboot == 2);
            }
            if (g_dos.handoff) {
                v9x_net_close(listener);
                v9x_net_drain(1000ul);
                v9x_log_line("handoff");
                return V9X_DOS_EXIT_HANDOFF;
            }
        }
        v9x_net_close(listener);
    }
}
