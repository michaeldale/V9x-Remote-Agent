#include "agent.h"
#include "http.h"
#include "installid_win.h"
#include "platform_win.h"
#include "v9xremote/relay.h"
#include "v9xremote/remote_key.h"
#include "v9xremote/update_proto.h"
#include "v9xremote/version.h"

/* Remote help (docs\remote-help.md): register with the update server, check
   in, and run relay sessions - an opaque byte tunnel between the developer's
   PC and this agent's own protocol handlers, carried over plain HTTP
   polling. Every chunk from the server is Ed25519-signed by the developer
   and checked here against remote_key.h before it reaches a handler; the
   loopback connection it feeds is served by v9x_serve_client exactly like a
   LAN client. Also the quiet update check ([update] check_on_startup). */

static const char v9x_remote_ini[] = "C:\\V9XREMOTE\\AGENT.INI";
static const char v9x_remote_status_ini[] = "C:\\V9XREMOTE\\STATUS.INI";

/* A reply holds a head and at most one down batch: about 1 MiB of records
   plus their headers. Allocated once; Open Watcom would put a static
   buffer of this size in the image. */
#define V9X_REMOTE_REPLY_MAX (V9X_RELAY_CHUNK_MAX + 2ul * V9X_RELAY_RECORD_HEADER + 16384ul)
#define V9X_REMOTE_UP_MAX 262144ul
#define V9X_REMOTE_TIMEOUT_MS 30000ul
#define V9X_REMOTE_IDLE_POLL 60ul
#define V9X_REMOTE_HEARTBEAT_MS 15000ul
#define V9X_REMOTE_BODY_MAX 4096ul
#define V9X_REMOTE_URL_MAX 512ul

typedef struct V9xRemote {
    V9xAgentState *state;
    char server[128];
    char machine[16];
    char secret[40];
    char display_name[72];
    char github[48];
    char email[96];
    unsigned char *reply;
    unsigned char *up;
    char body[V9X_REMOTE_BODY_MAX];
    char url[V9X_REMOTE_URL_MAX];
    /* Last check-in reply. */
    unsigned long next_poll;
    char session_id[40];
    char session_state[16];
    int stop;
} V9xRemote;

/* ---- small text helpers (no C runtime) ------------------------------ */

static void v9x_rt_copy(char *target, unsigned long capacity, const char *text)
{
    lstrcpynA(target, text, (int)capacity);
}

static int v9x_rt_append(char *target, unsigned long capacity, const char *text)
{
    unsigned long at = (unsigned long)lstrlenA(target);
    while (*text != '\0') {
        if (at + 1ul >= capacity) {
            target[at] = '\0';
            return 0;
        }
        target[at++] = *text++;
    }
    target[at] = '\0';
    return 1;
}

static int v9x_rt_append_number(char *target, unsigned long capacity, long value)
{
    char digits[12];
    char forward[13];
    unsigned long count = 0ul;
    unsigned long index = 0ul;
    unsigned long magnitude = value < 0l ? (unsigned long)(-value) : (unsigned long)value;
    do {
        digits[count++] = (char)('0' + (int)(magnitude % 10ul));
        magnitude /= 10ul;
    } while (magnitude != 0ul);
    if (value < 0l) forward[index++] = '-';
    while (count != 0ul) forward[index++] = digits[--count];
    forward[index] = '\0';
    return v9x_rt_append(target, capacity, forward);
}

/* key=percent-encoded value CRLF. */
static int v9x_rt_append_field(char *target, unsigned long capacity,
                               const char *key, const char *value)
{
    char encoded[V9X_CHAT_LINE * 3 + 4];
    if (!v9x_update_url_encode(value, encoded, sizeof(encoded))) encoded[0] = '\0';
    return v9x_rt_append(target, capacity, key) &&
           v9x_rt_append(target, capacity, "=") &&
           v9x_rt_append(target, capacity, encoded) &&
           v9x_rt_append(target, capacity, "\r\n");
}

static int v9x_rt_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode in place, dropping control characters except CR/LF/TAB. */
static void v9x_rt_decode(char *text)
{
    unsigned long in = 0ul;
    unsigned long out = 0ul;
    while (text[in] != '\0') {
        unsigned char ch = (unsigned char)text[in];
        if (ch == '%' && v9x_rt_hex(text[in + 1ul]) >= 0 &&
            v9x_rt_hex(text[in + 2ul]) >= 0) {
            ch = (unsigned char)((v9x_rt_hex(text[in + 1ul]) << 4) |
                                 v9x_rt_hex(text[in + 2ul]));
            in += 3ul;
        } else {
            ++in;
        }
        if (ch < 0x20u && ch != '\r' && ch != '\n' && ch != '\t') continue;
        text[out++] = (char)ch;
    }
    text[out] = '\0';
}

static int v9x_rt_ci_find(const unsigned char *data, unsigned long length,
                          const char *needle)
{
    unsigned long n = (unsigned long)lstrlenA(needle);
    unsigned long index;
    unsigned long k;
    for (index = 0ul; index + n <= length; ++index) {
        for (k = 0ul; k < n; ++k) {
            unsigned char a = data[index + k];
            unsigned char b = (unsigned char)needle[k];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32u);
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32u);
            if (a != b) break;
        }
        if (k == n) return 1;
    }
    return 0;
}

/* ---- state shared with the tray and protocol threads ---------------- */

void v9x_note_command(V9xConnection *conn, unsigned short type)
{
    V9xAgentState *state = conn->machine;
    if (conn->peer[0] == '\0') {
        if (conn->is_relay) {
            v9x_rt_copy(conn->peer, sizeof(conn->peer), "relay");
        } else {
            struct sockaddr_in peer;
            int length = sizeof(peer);
            char *text;
            if (getpeername(conn->socket, (struct sockaddr *)&peer, &length) == 0 &&
                (text = inet_ntoa(peer.sin_addr)) != 0) {
                v9x_rt_copy(conn->peer, sizeof(conn->peer), text);
            } else {
                v9x_rt_copy(conn->peer, sizeof(conn->peer), "lan");
            }
        }
    }
    state->last_command_tick = GetTickCount();
    state->last_command_type = type;
    v9x_rt_copy(state->last_command_source, sizeof(state->last_command_source),
                conn->peer);
    InterlockedExchange(&state->last_command_set, 1l);
}

long v9x_last_command_age(V9xAgentState *state)
{
    if (!v9x_flag_read(&state->last_command_set)) return -1l;
    return (long)((GetTickCount() - state->last_command_tick) / 1000ul);
}

/* Append "who: text" to the conversation, dropping the oldest lines when
   it is full. */
void v9x_chat_append(V9xAgentState *state, const char *who, const char *text)
{
    char line[V9X_CHAT_LINE + 32];
    unsigned long length;
    SYSTEMTIME now;
    char clock[8];
    GetLocalTime(&now);
    clock[0] = (char)('0' + now.wHour / 10u);
    clock[1] = (char)('0' + now.wHour % 10u);
    clock[2] = ':';
    clock[3] = (char)('0' + now.wMinute / 10u);
    clock[4] = (char)('0' + now.wMinute % 10u);
    clock[5] = ' ';
    clock[6] = '\0';
    line[0] = '\0';
    (void)v9x_rt_append(line, sizeof(line), clock);
    (void)v9x_rt_append(line, sizeof(line), who);
    (void)v9x_rt_append(line, sizeof(line), ": ");
    (void)v9x_rt_append(line, sizeof(line), text);
    (void)v9x_rt_append(line, sizeof(line), "\r\n");
    length = (unsigned long)lstrlenA(line);
    EnterCriticalSection(&state->chat_lock);
    while (state->chat_log_length + length + 1ul > V9X_CHAT_LOG_SIZE) {
        /* Drop the first line. */
        unsigned long cut = 0ul;
        unsigned long index;
        while (cut < state->chat_log_length && state->chat_log[cut] != '\n') ++cut;
        if (cut < state->chat_log_length) ++cut;
        if (cut == 0ul) cut = state->chat_log_length;
        for (index = cut; index <= state->chat_log_length; ++index) {
            state->chat_log[index - cut] = state->chat_log[index];
        }
        state->chat_log_length -= cut;
    }
    for (length = 0ul; line[length] != '\0'; ++length) {
        state->chat_log[state->chat_log_length++] = line[length];
    }
    state->chat_log[state->chat_log_length] = '\0';
    LeaveCriticalSection(&state->chat_lock);
}

/* The owner typed a line in the chat window: queue it for the next
   check-in and check in now. */
void v9x_chat_send(V9xAgentState *state, const char *text)
{
    if (text[0] == '\0') return;
    EnterCriticalSection(&state->chat_lock);
    if (state->chat_outgoing_count < V9X_CHAT_QUEUE) {
        v9x_rt_copy(state->chat_outgoing[state->chat_outgoing_count],
                    V9X_CHAT_LINE, text);
        ++state->chat_outgoing_count;
    }
    LeaveCriticalSection(&state->chat_lock);
    v9x_chat_append(state, "You", text);
    if (state->remote_wake != 0) (void)SetEvent(state->remote_wake);
}

/* ---- configuration --------------------------------------------------- */

void v9x_remote_load_config(V9xAgentState *state)
{
    char mode[16];
    (void)GetPrivateProfileStringA("remote", "mode", "off", mode, sizeof(mode),
                                   v9x_remote_ini);
    state->remote_on = lstrcmpiA(mode, "on") == 0;
    (void)GetPrivateProfileStringA("remote", "machine_id", "",
                                   state->remote_machine,
                                   sizeof(state->remote_machine), v9x_remote_ini);
    state->update_check_on_startup =
        GetPrivateProfileIntA("update", "check_on_startup", 0, v9x_remote_ini) != 0;
    state->remote_status = state->remote_on ? V9X_REMOTE_CONNECTING : V9X_REMOTE_OFF;
    (void)WritePrivateProfileStringA("remote", "status",
                                     state->remote_on ? "connecting" : "off",
                                     v9x_remote_status_ini);
}

static void v9x_remote_read_config(V9xRemote *remote)
{
    (void)GetPrivateProfileStringA("remote", "server", V9X_UPDATE_SERVER,
                                   remote->server, sizeof(remote->server),
                                   v9x_remote_ini);
    (void)GetPrivateProfileStringA("remote", "machine_id", "", remote->machine,
                                   sizeof(remote->machine), v9x_remote_ini);
    (void)GetPrivateProfileStringA("remote", "secret", "", remote->secret,
                                   sizeof(remote->secret), v9x_remote_ini);
    (void)GetPrivateProfileStringA("remote", "display_name", "",
                                   remote->display_name,
                                   sizeof(remote->display_name), v9x_remote_ini);
    (void)GetPrivateProfileStringA("remote", "github", "", remote->github,
                                   sizeof(remote->github), v9x_remote_ini);
    (void)GetPrivateProfileStringA("remote", "email", "", remote->email,
                                   sizeof(remote->email), v9x_remote_ini);
    {
        /* A trailing slash would double up with the path. */
        int length = lstrlenA(remote->server);
        while (length > 0 && remote->server[length - 1] == '/') {
            remote->server[--length] = '\0';
        }
    }
    if (!v9x_relay_machine_valid(remote->machine)) remote->machine[0] = '\0';
}

static void v9x_remote_status(V9xRemote *remote, LONG status, const char *text)
{
    SYSTEMTIME now;
    char stamp[24];
    InterlockedExchange(&remote->state->remote_status, status);
    GetLocalTime(&now);
    stamp[0] = '\0';
    (void)v9x_rt_append_number(stamp, sizeof(stamp), (long)now.wYear);
    (void)v9x_rt_append(stamp, sizeof(stamp), now.wMonth < 10u ? "-0" : "-");
    (void)v9x_rt_append_number(stamp, sizeof(stamp), (long)now.wMonth);
    (void)v9x_rt_append(stamp, sizeof(stamp), now.wDay < 10u ? "-0" : "-");
    (void)v9x_rt_append_number(stamp, sizeof(stamp), (long)now.wDay);
    (void)v9x_rt_append(stamp, sizeof(stamp), now.wHour < 10u ? " 0" : " ");
    (void)v9x_rt_append_number(stamp, sizeof(stamp), (long)now.wHour);
    (void)v9x_rt_append(stamp, sizeof(stamp), now.wMinute < 10u ? ":0" : ":");
    (void)v9x_rt_append_number(stamp, sizeof(stamp), (long)now.wMinute);
    (void)v9x_rt_append(stamp, sizeof(stamp), now.wSecond < 10u ? ":0" : ":");
    (void)v9x_rt_append_number(stamp, sizeof(stamp), (long)now.wSecond);
    (void)WritePrivateProfileStringA("remote", "status", text, v9x_remote_status_ini);
    (void)WritePrivateProfileStringA("remote", "machine_id", remote->machine,
                                     v9x_remote_status_ini);
    (void)WritePrivateProfileStringA("remote", "updated", stamp, v9x_remote_status_ini);
    if (status == V9X_REMOTE_ONLINE) {
        (void)WritePrivateProfileStringA("remote", "last_contact", stamp,
                                         v9x_remote_status_ini);
    }
    {
        char age[16];
        long seconds = v9x_last_command_age(remote->state);
        age[0] = '\0';
        (void)v9x_rt_append_number(age, sizeof(age), seconds);
        (void)WritePrivateProfileStringA("remote", "last_command_age", age,
                                         v9x_remote_status_ini);
        (void)WritePrivateProfileStringA("remote", "last_command_source",
                                         remote->state->last_command_source,
                                         v9x_remote_status_ini);
    }
}

static void v9x_remote_os(char *out, unsigned long capacity)
{
    OSVERSIONINFOA info;
    info.dwOSVersionInfoSize = sizeof(info);
    out[0] = '\0';
    if (!GetVersionExA(&info)) {
        v9x_rt_copy(out, capacity, "Windows");
        return;
    }
    if (info.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        v9x_rt_copy(out, capacity,
                    info.dwMajorVersion == 5ul && info.dwMinorVersion == 0ul ? "Windows 2000 " :
                    info.dwMajorVersion == 5ul && info.dwMinorVersion == 1ul ? "Windows XP " :
                    info.dwMajorVersion == 4ul ? "Windows NT 4.0 " : "Windows NT ");
    } else {
        v9x_rt_copy(out, capacity,
                    info.dwMinorVersion == 0ul ? "Windows 95 " :
                    info.dwMinorVersion == 10ul ? "Windows 98 " :
                    info.dwMinorVersion == 90ul ? "Windows ME " : "Windows 9x ");
    }
    (void)v9x_rt_append_number(out, capacity, (long)info.dwMajorVersion);
    (void)v9x_rt_append(out, capacity, ".");
    (void)v9x_rt_append_number(out, capacity, (long)info.dwMinorVersion);
    (void)v9x_rt_append(out, capacity, ".");
    (void)v9x_rt_append_number(out, capacity, (long)(info.dwBuildNumber & 0xFFFFul));
    {
        /* Windows 98 SE reports " A " here; keep only the letters. */
        char *csd = info.szCSDVersion;
        int length;
        while (*csd == ' ') ++csd;
        length = lstrlenA(csd);
        while (length > 0 && csd[length - 1] == ' ') csd[--length] = '\0';
        if (length > 0) {
            (void)v9x_rt_append(out, capacity, " ");
            (void)v9x_rt_append(out, capacity, csd);
        }
    }
}

/* ---- HTTP calls ------------------------------------------------------ */

/* server + /index.php?bt_type=v9xa_<op>&machine=<id> + extra. */
static int v9x_remote_url(V9xRemote *remote, const char *op, const char *extra)
{
    remote->url[0] = '\0';
    return v9x_rt_append(remote->url, sizeof(remote->url), remote->server) &&
           v9x_rt_append(remote->url, sizeof(remote->url), "/index.php?bt_type=v9xa_") &&
           v9x_rt_append(remote->url, sizeof(remote->url), op) &&
           (remote->machine[0] == '\0' ||
            (v9x_rt_append(remote->url, sizeof(remote->url), "&machine=") &&
             v9x_rt_append(remote->url, sizeof(remote->url), remote->machine))) &&
           (extra == 0 || v9x_rt_append(remote->url, sizeof(remote->url), extra));
}

/* One call; the reply is in remote->reply. Returns 1 on any HTTP reply. */
static int v9x_remote_call(V9xRemote *remote, const char *method,
                           const unsigned char *body, unsigned long body_length,
                           V9xHttpResponse *response)
{
    V9xHttpRequest request;
    char headers[64];
    DWORD native_error = 0ul;
    headers[0] = '\0';
    if (remote->secret[0] != '\0') {
        (void)v9x_rt_append(headers, sizeof(headers), "X-V9X-Secret: ");
        (void)v9x_rt_append(headers, sizeof(headers), remote->secret);
        (void)v9x_rt_append(headers, sizeof(headers), "\r\n");
    }
    request.method = method;
    request.url = remote->url;
    request.headers = headers;
    request.body = body;
    request.body_length = body_length;
    request.timeout_ms = V9X_REMOTE_TIMEOUT_MS;
    if (!v9x_http_request(&request, remote->reply, V9X_REMOTE_REPLY_MAX, response,
                          &native_error)) {
        return 0;
    }
    /* INI replies are read as text. */
    if (response->body_offset + response->body_length < V9X_REMOTE_REPLY_MAX) {
        remote->reply[response->body_offset + response->body_length] = '\0';
    }
    return 1;
}

static int v9x_remote_value(V9xRemote *remote, const V9xHttpResponse *response,
                          const char *section, const char *key,
                          char *out, unsigned long capacity)
{
    out[0] = '\0';
    return v9x_update_ini_value((const char *)remote->reply + response->body_offset,
                                response->body_length, section, key, out,
                                capacity) != 0;
}

static int v9x_remote_register(V9xRemote *remote)
{
    V9xHttpResponse response;
    char value[64];
    char machine[16];
    char secret[40];
    char os[96];
    char computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD computer_length = sizeof(computer);

    if (!GetComputerNameA(computer, &computer_length)) computer[0] = '\0';
    v9x_remote_os(os, sizeof(os));
    remote->body[0] = '\0';
    (void)v9x_rt_append(remote->body, sizeof(remote->body), "[register]\r\n");
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "install_id",
                              remote->state->install_id);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "display_name",
                              remote->display_name);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "github",
                              remote->github);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "email",
                              remote->email);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "version",
                              V9X_AGENT_VERSION);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "os", os);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "computer",
                              computer);
    remote->machine[0] = '\0';
    remote->secret[0] = '\0';
    if (!v9x_remote_url(remote, "register", 0) ||
        !v9x_remote_call(remote, "POST", (const unsigned char *)remote->body,
                         (unsigned long)lstrlenA(remote->body), &response)) {
        v9x_log_line("remote-register-unreachable");
        return 0;
    }
    if (!v9x_remote_value(remote, &response, "register", "status", value, sizeof(value)) ||
        lstrcmpA(value, "ok") != 0 ||
        !v9x_remote_value(remote, &response, "register", "machine", machine, sizeof(machine)) ||
        !v9x_remote_value(remote, &response, "register", "secret", secret, sizeof(secret)) ||
        !v9x_relay_machine_valid(machine) || lstrlenA(secret) != 32) {
        v9x_log_event("remote-register-refused", value);
        return 0;
    }
    v9x_rt_copy(remote->machine, sizeof(remote->machine), machine);
    v9x_rt_copy(remote->secret, sizeof(remote->secret), secret);
    v9x_rt_copy(remote->state->remote_machine, sizeof(remote->state->remote_machine),
                machine);
    (void)WritePrivateProfileStringA("remote", "machine_id", machine, v9x_remote_ini);
    (void)WritePrivateProfileStringA("remote", "secret", secret, v9x_remote_ini);
    (void)WritePrivateProfileStringA(0, 0, 0, v9x_remote_ini);
    v9x_log_event("remote-registered", machine);
    return 1;
}

static void v9x_remote_forget(V9xRemote *remote)
{
    remote->machine[0] = '\0';
    remote->secret[0] = '\0';
    remote->state->remote_machine[0] = '\0';
    (void)WritePrivateProfileStringA("remote", "machine_id", "", v9x_remote_ini);
    (void)WritePrivateProfileStringA("remote", "secret", "", v9x_remote_ini);
}

/* Check in. Fills next_poll, session_id/state; delivers chat both ways.
   Returns 1 on an ok reply. */
static int v9x_remote_poll(V9xRemote *remote, const char *open_session)
{
    V9xAgentState *state = remote->state;
    V9xHttpResponse response;
    char value[V9X_CHAT_LINE * 3 + 4];
    char key[8];
    char sent[V9X_CHAT_QUEUE][V9X_CHAT_LINE];
    int sent_count;
    int index;
    long count;
    long age = v9x_last_command_age(state);

    remote->body[0] = '\0';
    (void)v9x_rt_append(remote->body, sizeof(remote->body), "[poll]\r\nversion=" V9X_AGENT_VERSION "\r\nstate=");
    (void)v9x_rt_append(remote->body, sizeof(remote->body),
                        v9x_flag_read(&state->remote_paused) ? "paused" : "on");
    (void)v9x_rt_append(remote->body, sizeof(remote->body), "\r\nbusy=");
    (void)v9x_rt_append(remote->body, sizeof(remote->body),
                        (age >= 0l && age < 60l) ? "1" : "0");
    (void)v9x_rt_append(remote->body, sizeof(remote->body), "\r\nlast_command_age=");
    (void)v9x_rt_append_number(remote->body, sizeof(remote->body), age);
    (void)v9x_rt_append(remote->body, sizeof(remote->body), "\r\n");
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "last_command_source",
                              state->last_command_source);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "session",
                              open_session != 0 ? open_session : "");
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "display_name",
                              remote->display_name);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "github", remote->github);
    (void)v9x_rt_append_field(remote->body, sizeof(remote->body), "email", remote->email);

    EnterCriticalSection(&state->chat_lock);
    sent_count = state->chat_outgoing_count;
    for (index = 0; index < sent_count; ++index) {
        v9x_rt_copy(sent[index], V9X_CHAT_LINE, state->chat_outgoing[index]);
    }
    LeaveCriticalSection(&state->chat_lock);
    if (sent_count != 0) {
        (void)v9x_rt_append(remote->body, sizeof(remote->body), "[chat]\r\ncount=");
        (void)v9x_rt_append_number(remote->body, sizeof(remote->body), (long)sent_count);
        (void)v9x_rt_append(remote->body, sizeof(remote->body), "\r\n");
        for (index = 0; index < sent_count; ++index) {
            key[0] = (char)('1' + index);
            key[1] = '\0';
            (void)v9x_rt_append_field(remote->body, sizeof(remote->body), key, sent[index]);
        }
    }

    if (!v9x_remote_url(remote, "poll", 0) ||
        !v9x_remote_call(remote, "POST", (const unsigned char *)remote->body,
                         (unsigned long)lstrlenA(remote->body), &response)) {
        return 0;
    }
    if (!v9x_remote_value(remote, &response, "poll", "status", value, 32ul)) {
        v9x_log_event("remote-poll-bad-reply", "no status");
        return 0;
    }
    if (lstrcmpA(value, "unknown_machine") == 0 || lstrcmpA(value, "bad_secret") == 0) {
        v9x_log_event("remote-poll-identity", value);
        v9x_remote_forget(remote);
        return 0;
    }
    if (lstrcmpA(value, "retired") == 0) {
        v9x_log_line("remote-retired");
        remote->stop = 1;
        return 0;
    }
    if (lstrcmpA(value, "ok") != 0) {
        v9x_log_event("remote-poll-status", value);
        remote->next_poll = 600ul;
        return 0;
    }

    /* The chat we sent arrived: drop it from the queue. */
    if (sent_count != 0) {
        EnterCriticalSection(&state->chat_lock);
        for (index = sent_count; index < state->chat_outgoing_count; ++index) {
            v9x_rt_copy(state->chat_outgoing[index - sent_count], V9X_CHAT_LINE,
                        state->chat_outgoing[index]);
        }
        state->chat_outgoing_count -= sent_count;
        LeaveCriticalSection(&state->chat_lock);
    }

    remote->next_poll = V9X_REMOTE_IDLE_POLL;
    if (v9x_remote_value(remote, &response, "poll", "next_poll", value, 16ul)) {
        unsigned long seconds = 0ul;
        for (index = 0; value[index] >= '0' && value[index] <= '9'; ++index) {
            seconds = seconds * 10ul + (unsigned long)(value[index] - '0');
        }
        if (seconds >= 1ul && seconds <= 3600ul) remote->next_poll = seconds;
    }
    remote->session_id[0] = '\0';
    remote->session_state[0] = '\0';
    if (v9x_remote_value(remote, &response, "session", "id", remote->session_id,
                       sizeof(remote->session_id)) &&
        v9x_relay_hex_id_valid(remote->session_id)) {
        (void)v9x_remote_value(remote, &response, "session", "state",
                             remote->session_state, sizeof(remote->session_state));
    } else {
        remote->session_id[0] = '\0';
    }

    count = 0l;
    if (v9x_remote_value(remote, &response, "chat", "count", value, 8ul)) {
        for (index = 0; value[index] >= '0' && value[index] <= '9'; ++index) {
            count = count * 10l + (long)(value[index] - '0');
        }
    }
    for (index = 1; index <= (int)count && index <= 20; ++index) {
        key[0] = '\0';
        (void)v9x_rt_append_number(key, sizeof(key), (long)index);
        if (v9x_remote_value(remote, &response, "chat", key, value, sizeof(value))) {
            v9x_rt_decode(value);
            v9x_chat_append(state, "Developer", value);
            InterlockedExchange(&state->chat_incoming, 1l);
        }
    }
    v9x_remote_status(remote, V9X_REMOTE_ONLINE,
                      v9x_flag_read(&state->remote_paused) ? "paused" : "online");
    return 1;
}

static void v9x_remote_session_call(V9xRemote *remote, const char *op,
                                    const char *session, const char *extra)
{
    V9xHttpResponse response;
    char query[96];
    query[0] = '\0';
    (void)v9x_rt_append(query, sizeof(query), "&session=");
    (void)v9x_rt_append(query, sizeof(query), session);
    if (extra != 0) (void)v9x_rt_append(query, sizeof(query), extra);
    if (v9x_remote_url(remote, op, query)) {
        (void)v9x_remote_call(remote, "POST", 0, 0ul, &response);
    }
}

/* ---- the relay tunnel ------------------------------------------------ */

/* A connected loopback pair: *outer is ours, the other end is served as a
   relay connection. */
static int v9x_remote_loopback(V9xAgentState *state, SOCKET *outer)
{
    struct sockaddr_in address;
    int length = sizeof(address);
    int no_delay = 1;
    SOCKET listener;
    SOCKET client;
    SOCKET served;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return 0;
    v9x_platform_no_inherit((HANDLE)listener);
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&address, &length) == SOCKET_ERROR) {
        closesocket(listener);
        return 0;
    }
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        return 0;
    }
    v9x_platform_no_inherit((HANDLE)client);
    if (connect(client, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        closesocket(client);
        closesocket(listener);
        return 0;
    }
    length = sizeof(address);
    served = accept(listener, (struct sockaddr *)&address, &length);
    closesocket(listener);
    if (served == INVALID_SOCKET) {
        closesocket(client);
        return 0;
    }
    v9x_platform_no_inherit((HANDLE)served);
    (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
    (void)setsockopt(served, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
    if (!v9x_start_connection(state, served, 1)) {
        closesocket(client);
        return 0;
    }
    *outer = client;
    return 1;
}

/* Whatever the handlers have written: >0 bytes, 0 none, -1 the loopback
   closed. first_wait_ms is how long to wait for the first byte (right
   after feeding a request, so its reply goes up in the same round);
   after that it takes only what is already there. */
static long v9x_remote_drain(SOCKET tunnel, unsigned char *buffer,
                             unsigned long capacity, unsigned long first_wait_ms)
{
    fd_set readable;
    struct timeval wait;
    unsigned long total = 0ul;
    int received;
    while (total < capacity) {
        FD_ZERO(&readable);
        FD_SET(tunnel, &readable);
        wait.tv_sec = 0l;
        wait.tv_usec = total == 0ul ? (long)(first_wait_ms * 1000ul) : 0l;
        if (select(0, &readable, 0, 0, &wait) <= 0) break;
        received = recv(tunnel, (char *)buffer + total, (int)(capacity - total), 0);
        if (received <= 0) return total != 0ul ? (long)total : -1l;
        total += (unsigned long)received;
    }
    return (long)total;
}

static void v9x_remote_nonce(char *nonce)
{
    char uuid[V9X_INSTALL_ID_SIZE];
    unsigned long in;
    unsigned long out = 0ul;
    v9x_install_id_generate(uuid);
    for (in = 0ul; uuid[in] != '\0' && out < V9X_RELAY_HEX_ID_LENGTH; ++in) {
        if (uuid[in] != '-') nonce[out++] = uuid[in];
    }
    nonce[out] = '\0';
}

/* Run one accepted session until it ends. */
static void v9x_remote_session(V9xRemote *remote, const char *session_id)
{
    static const unsigned char public_key[32] = V9X_REMOTE_PUBLIC_KEY;
    V9xAgentState *state = remote->state;
    V9xHttpResponse response;
    struct v9x_relay_record record;
    char session[40];
    char nonce[40];
    char query[160];
    char reason[32];
    SOCKET tunnel = INVALID_SOCKET;
    unsigned long expected = 1ul;
    unsigned long up_seq = 1ul;
    unsigned long offset;
    DWORD last_heartbeat = GetTickCount();
    long drained;
    int moved = 1;
    int fed;
    int tries;
    int status;
    int server_closed = 0;

    v9x_rt_copy(session, sizeof(session), session_id);
    v9x_rt_copy(reason, sizeof(reason), "ended");
    if (v9x_flag_read(&state->remote_paused)) {
        v9x_remote_session_call(remote, "accept", session, "&refuse=paused");
        return;
    }
    v9x_remote_nonce(nonce);
    if (!v9x_remote_loopback(state, &tunnel)) {
        v9x_log_line("relay-loopback-failed");
        v9x_remote_session_call(remote, "accept", session, "&refuse=loopback");
        return;
    }
    query[0] = '\0';
    (void)v9x_rt_append(query, sizeof(query), "&session=");
    (void)v9x_rt_append(query, sizeof(query), session);
    (void)v9x_rt_append(query, sizeof(query), "&nonce=");
    (void)v9x_rt_append(query, sizeof(query), nonce);
    if (!v9x_remote_url(remote, "accept", query) ||
        !v9x_remote_call(remote, "POST", 0, 0ul, &response) ||
        !v9x_remote_value(remote, &response, "session", "status", reason, sizeof(reason)) ||
        lstrcmpA(reason, "ok") != 0) {
        closesocket(tunnel);
        return;
    }
    v9x_rt_copy(reason, sizeof(reason), "ended");
    InterlockedExchange(&state->remote_session, 1l);
    v9x_log_event("relay-session-open", session);

    for (;;) {
        /* After a quiet round, ask the server to hold the request until the
           developer sends something (up to 3 s); otherwise come straight
           back. */
        int quiet = !moved;
        moved = 0;
        fed = 0;
        if (v9x_flag_read(&state->remote_paused)) {
            v9x_rt_copy(reason, sizeof(reason), "paused");
            break;
        }

        /* Down: signed chunks for the handlers. */
        query[0] = '\0';
        (void)v9x_rt_append(query, sizeof(query), "&session=");
        (void)v9x_rt_append(query, sizeof(query), session);
        (void)v9x_rt_append(query, sizeof(query), "&after=");
        (void)v9x_rt_append_number(query, sizeof(query), (long)(expected - 1ul));
        (void)v9x_rt_append(query, sizeof(query), quiet ? "&wait=3" : "&wait=0");
        if (v9x_remote_url(remote, "down", query) &&
            v9x_remote_call(remote, "GET", 0, 0ul, &response) &&
            response.status == 200ul) {
            if (v9x_rt_ci_find(remote->reply, response.body_offset,
                               "x-v9x-session-state: closed")) {
                server_closed = 1;
                break;
            }
            offset = 0ul;
            for (;;) {
                status = v9x_relay_next(remote->reply + response.body_offset,
                                        response.body_length, &offset, &record);
                if (status == V9X_RELAY_RECORD_END) break;
                if (status != V9X_RELAY_RECORD_OK || record.seq != expected ||
                    !v9x_relay_verify(&record, public_key, remote->machine,
                                      state->install_id, nonce, session)) {
                    v9x_log_event("relay-bad-chunk", session);
                    v9x_rt_copy(reason, sizeof(reason), "bad-signature");
                    goto finished;
                }
                if (!v9x_http_send_all(tunnel, (const char *)record.data,
                                       record.length, V9X_REMOTE_TIMEOUT_MS)) {
                    v9x_rt_copy(reason, sizeof(reason), "loopback");
                    goto finished;
                }
                ++expected;
                moved = 1;
                fed = 1;
            }
        }

        /* Up: whatever the handlers produced. */
        drained = v9x_remote_drain(tunnel, remote->up, V9X_REMOTE_UP_MAX,
                                   fed ? 500ul : 0ul);
        if (drained > 0l) {
            query[0] = '\0';
            (void)v9x_rt_append(query, sizeof(query), "&session=");
            (void)v9x_rt_append(query, sizeof(query), session);
            (void)v9x_rt_append(query, sizeof(query), "&seq=");
            (void)v9x_rt_append_number(query, sizeof(query), (long)up_seq);
            for (tries = 0; tries < 4; ++tries) {
                if (v9x_remote_url(remote, "up", query) &&
                    v9x_remote_call(remote, "POST", remote->up, (unsigned long)drained,
                                    &response) &&
                    response.status == 200ul) {
                    break;
                }
                Sleep(1000ul);
            }
            if (tries == 4) {
                v9x_rt_copy(reason, sizeof(reason), "up-failed");
                break;
            }
            if (v9x_rt_ci_find(remote->reply + response.body_offset,
                               response.body_length, "status=closed")) {
                server_closed = 1;
                break;
            }
            ++up_seq;
            moved = 1;
        } else if (drained < 0l) {
            v9x_rt_copy(reason, sizeof(reason), "loopback-closed");
            break;
        }

        /* Heartbeat: chat, pause and whether the session still stands. */
        if (GetTickCount() - last_heartbeat >= V9X_REMOTE_HEARTBEAT_MS) {
            last_heartbeat = GetTickCount();
            if (v9x_remote_poll(remote, session) &&
                (lstrcmpA(remote->session_id, session) != 0 ||
                 lstrcmpA(remote->session_state, "open") != 0)) {
                server_closed = 1;
                break;
            }
            if (remote->stop) break;
        }

        /* The down request itself paces a quiet session (it waits up to
           3 s); this only keeps a server that answers at once from being
           hammered. */
        if (!moved) Sleep(100ul);
    }
finished:
    closesocket(tunnel);
    InterlockedExchange(&state->remote_session, 0l);
    if (!server_closed) {
        query[0] = '\0';
        (void)v9x_rt_append(query, sizeof(query), "&reason=");
        (void)v9x_rt_append(query, sizeof(query), reason);
        v9x_remote_session_call(remote, "close", session, query);
    }
    v9x_log_event("relay-session-closed", reason);
}

static DWORD WINAPI v9x_remote_thread(LPVOID parameter)
{
    V9xRemote *remote;
    unsigned long backoff = 30ul;
    remote = (V9xRemote *)VirtualAlloc(0, sizeof(V9xRemote), MEM_COMMIT, PAGE_READWRITE);
    if (remote == 0) return 0ul;
    remote->reply = (unsigned char *)VirtualAlloc(0, V9X_REMOTE_REPLY_MAX + 1ul,
                                                  MEM_COMMIT, PAGE_READWRITE);
    remote->up = (unsigned char *)VirtualAlloc(0, V9X_REMOTE_UP_MAX, MEM_COMMIT,
                                               PAGE_READWRITE);
    if (remote->reply == 0 || remote->up == 0) {
        v9x_log_line("remote-alloc-failed");
        return 0ul;
    }
    remote->state = (V9xAgentState *)parameter;
    remote->stop = 0;
    remote->next_poll = V9X_REMOTE_IDLE_POLL;
    v9x_remote_read_config(remote);
    while (remote->state->winsock_version == 0u) Sleep(1000ul);
    v9x_log_event("remote-start", remote->server);

    while (!remote->stop) {
        if (remote->machine[0] == '\0' || remote->secret[0] == '\0') {
            v9x_remote_status(remote, V9X_REMOTE_CONNECTING, "registering");
            if (!v9x_remote_register(remote)) {
                v9x_remote_status(remote, V9X_REMOTE_UNREACHABLE, "unreachable");
                (void)WaitForSingleObject(remote->state->remote_wake, backoff * 1000ul);
                if (backoff < 900ul) backoff *= 2ul;
                continue;
            }
            backoff = 30ul;
        }
        if (!v9x_remote_poll(remote, 0)) {
            if (remote->stop) break;
            if (remote->machine[0] == '\0') continue;
            v9x_remote_status(remote, V9X_REMOTE_UNREACHABLE, "unreachable");
            (void)WaitForSingleObject(remote->state->remote_wake, backoff * 1000ul);
            if (backoff < 900ul) backoff *= 2ul;
            continue;
        }
        backoff = 30ul;
        if (remote->session_id[0] != '\0' &&
            lstrcmpA(remote->session_state, "pending") == 0) {
            v9x_remote_session(remote, remote->session_id);
            continue;
        }
        (void)WaitForSingleObject(remote->state->remote_wake, remote->next_poll * 1000ul);
    }
    v9x_remote_status(remote, V9X_REMOTE_STOPPED, "retired");
    return 0ul;
}

int v9x_remote_start(V9xAgentState *state)
{
    HANDLE thread;
    DWORD thread_id;
    state->remote_wake = CreateEventA(0, FALSE, FALSE, 0);
    if (state->remote_wake == 0) return 0;
    thread = CreateThread(0, 65536ul, v9x_remote_thread, state, 0ul, &thread_id);
    if (thread == 0) return 0;
    CloseHandle(thread);
    return 1;
}

/* ---- quiet update check ([update] check_on_startup=1) ---------------- */

static DWORD WINAPI v9x_update_check_thread(LPVOID parameter)
{
    V9xAgentState *state = (V9xAgentState *)parameter;
    V9xHttpRequest request;
    V9xHttpResponse response;
    DWORD native_error = 0ul;
    unsigned char *reply;
    char server[128];
    char url[256];
    char value[32];
    char latest[24];
    int attempt;
    reply = (unsigned char *)VirtualAlloc(0, 16384ul, MEM_COMMIT, PAGE_READWRITE);
    if (reply == 0) return 0ul;
    while (state->winsock_version == 0u) Sleep(1000ul);
    (void)GetPrivateProfileStringA("remote", "server", V9X_UPDATE_SERVER, server,
                                   sizeof(server), v9x_remote_ini);
    url[0] = '\0';
    (void)v9x_rt_append(url, sizeof(url), server);
    (void)v9x_rt_append(url, sizeof(url),
                        "/index.php?bt_type=v9xu_check&app=" V9X_UPDATE_APP
                        "&family=" V9X_UPDATE_FAMILY "&version=" V9X_AGENT_VERSION);
    request.method = "GET";
    request.url = url;
    request.headers = 0;
    request.body = 0;
    request.body_length = 0ul;
    request.timeout_ms = V9X_REMOTE_TIMEOUT_MS;
    /* Give the network a minute to come up after boot; try three times. */
    for (attempt = 0; attempt < 3; ++attempt) {
        Sleep(attempt == 0 ? 60000ul : 300000ul);
        if (!v9x_http_request(&request, reply, 16383ul, &response, &native_error) ||
            response.status != 200ul) {
            continue;
        }
        reply[response.body_offset + response.body_length] = '\0';
        if (v9x_update_ini_value((const char *)reply + response.body_offset,
                                 response.body_length, "update", "status",
                                 value, sizeof(value)) &&
            lstrcmpA(value, "update") == 0 &&
            v9x_update_ini_value((const char *)reply + response.body_offset,
                                 response.body_length, "update", "latest",
                                 latest, sizeof(latest)) &&
            v9x_update_version_compare(latest, V9X_AGENT_VERSION) == 1) {
            v9x_rt_copy(state->update_available, sizeof(state->update_available), latest);
            v9x_log_event("update-available", latest);
        } else {
            v9x_log_line("update-current");
        }
        break;
    }
    VirtualFree(reply, 0ul, MEM_RELEASE);
    return 0ul;
}

int v9x_update_check_start(V9xAgentState *state)
{
    HANDLE thread;
    DWORD thread_id;
    thread = CreateThread(0, 16384ul, v9x_update_check_thread, state, 0ul, &thread_id);
    if (thread == 0) return 0;
    CloseHandle(thread);
    return 1;
}
