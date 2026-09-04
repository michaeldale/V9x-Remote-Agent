/* Entry point and accept loop for the DOS agent.

   DOS is single-tasking, so this is deliberately simple: bring up the Watt-32
   stack, open one listening socket, and serve exactly one client at a time to
   completion before accepting the next. There are no threads, no connection
   pool, and no singleton mutex - the boot counter file is the only persistent
   state carried across restarts. The agent is launched from AUTOEXEC.BAT after
   the packet driver; see docs\dos-agent.md and packaging\dos\. */

#include <string.h>

#include "dosagent.h"

/* One large static state block (~85 KiB of buffers). Fine under DOS/4GW's flat
   32-bit model; this is exactly why the protected-mode target was chosen over a
   real-mode one with 64 KiB segment limits. */
static V9xDos g_dos;

int main(void)
{
    int listener;
    int client;
    unsigned long peer_ip;
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
        for (;;) {
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
        }
        v9x_net_close(listener);
    }
}
