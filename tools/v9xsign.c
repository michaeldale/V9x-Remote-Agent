/*
 * v9xsign - the operator's signing tool for releases and remote help.
 *
 *   v9xsign keygen  <keyfile>                       new key; print public key
 *   v9xsign public  <seed>                          print the public key
 *   v9xsign sign    <seed> <file>                   print the signature
 *   v9xsign verify  <pubhex64> <sighex128> <file>   exit 0 valid, 1 not
 *   v9xsign relay   <seed> <machine> <install> <nonce> <session>
 *                   <first-seq> <infile> <outfile>
 *
 * <seed> is 64 hex digits, "env" (read V9X_SIGNING_KEY), or "@path" (read
 * the first 64 hex digits of a key file such as the one keygen writes), so
 * a private key need never appear on a command line.
 *
 * relay turns the bytes of infile into a down batch for one relay session
 * (include\v9xremote\relay.h): chunks of at most 1 MiB, each a V9XD record
 * signed over its canonical text with seq first-seq, first-seq + 1, ...
 * It prints the next unused seq. v9x_mcp.py and v9xctl.ps1 call it once per
 * batch, so the private key stays in this process and its key file.
 *
 * The agent checks these signatures with the same src\common\ed25519.c and
 * src\common\relay.c, so a batch this tool writes is a batch the agent
 * accepts. Host-only: it uses stdio and malloc, which the shared modules may
 * not. Signing is not constant-time (include\v9xremote\ed25519.h); run it on
 * your own machine, never a shared one.
 *
 * Exit status: 0 success (or a valid signature), 1 an invalid signature,
 * 2 a usage, argument or file error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "v9xremote/ed25519.h"
#include "v9xremote/relay.h"

#define V9XSIGN_EXIT_OK      0
#define V9XSIGN_EXIT_INVALID 1
#define V9XSIGN_EXIT_ERROR   2

/* Release files are small and relay batches are what one request carries;
 * the cap keeps a mistaken argument from being read whole into memory. */
#define V9XSIGN_MAX_FILE_BYTES (64ul * 1024ul * 1024ul)

static void v9xsign_usage(void)
{
    fputs("usage: v9xsign keygen <keyfile>\n"
          "       v9xsign public <seed>\n"
          "       v9xsign sign <seed> <file>\n"
          "       v9xsign verify <pubhex64> <sighex128> <file>\n"
          "       v9xsign relay <seed> <machine> <install> <nonce> <session>"
          " <first-seq> <infile> <outfile>\n"
          "<seed> is 64 hex digits, env (V9X_SIGNING_KEY) or @keyfile\n",
          stderr);
}

static int v9xsign_hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Exactly 2 * count hex digits into count bytes, or V9X_FALSE. */
static v9x_u16 v9xsign_parse_hex(const char *text, v9x_u8 *out, size_t count)
{
    size_t i;
    int high;
    int low;

    if (strlen(text) != 2u * count) {
        return V9X_FALSE;
    }
    for (i = 0u; i < count; ++i) {
        high = v9xsign_hex_digit(text[2u * i]);
        low = v9xsign_hex_digit(text[2u * i + 1u]);
        if (high < 0 || low < 0) {
            return V9X_FALSE;
        }
        out[i] = (v9x_u8)((high << 4) | low);
    }
    return V9X_TRUE;
}

static void v9xsign_print_hex(const v9x_u8 *bytes, size_t count)
{
    size_t i;

    for (i = 0u; i < count; ++i) {
        printf("%02x", (unsigned int)bytes[i]);
    }
    putchar('\n');
}

/* Read a whole file in binary mode. Returns a malloc'd buffer (at least one
 * byte, so an empty file is not a null) or NULL with a message printed. */
static v9x_u8 *v9xsign_read_file(const char *path, v9x_u32 *length)
{
    FILE *file;
    long size;
    v9x_u8 *buffer;

    file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "v9xsign: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (size = ftell(file)) < 0L ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "v9xsign: cannot size %s\n", path);
        fclose(file);
        return NULL;
    }
    if ((unsigned long)size > V9XSIGN_MAX_FILE_BYTES) {
        fprintf(stderr, "v9xsign: %s is larger than %lu bytes\n", path,
                V9XSIGN_MAX_FILE_BYTES);
        fclose(file);
        return NULL;
    }
    buffer = (v9x_u8 *)malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fprintf(stderr, "v9xsign: out of memory reading %s\n", path);
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1u, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "v9xsign: cannot read %s\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *length = (v9x_u32)size;
    return buffer;
}

/* The seed argument: hex, "env", or "@keyfile". V9X_FALSE with a message
 * if it is not 64 hex digits. */
static v9x_u16 v9xsign_seed(const char *argument, v9x_u8 seed[32])
{
    const char *text = argument;
    char from_file[65];
    v9x_u8 *contents;
    v9x_u32 length;
    v9x_u32 index;
    v9x_u32 used = 0ul;

    if (strcmp(argument, "env") == 0) {
        text = getenv("V9X_SIGNING_KEY");
        if (text == 0) {
            text = "";
        }
    } else if (argument[0] == '@') {
        contents = v9xsign_read_file(argument + 1, &length);
        if (contents == NULL) {
            return V9X_FALSE;
        }
        for (index = 0ul; index < length && used < 64ul; ++index) {
            if (v9xsign_hex_digit((char)contents[index]) >= 0) {
                from_file[used++] = (char)contents[index];
            } else if (used != 0ul) {
                break;
            }
        }
        from_file[used] = '\0';
        free(contents);
        text = from_file;
    }
    if (!v9xsign_parse_hex(text, seed, 32u)) {
        fputs("v9xsign: the seed must be 64 hex digits\n", stderr);
        return V9X_FALSE;
    }
    return V9X_TRUE;
}

typedef BOOLEAN (WINAPI *v9xsign_random_fn)(PVOID, ULONG);

/* 32 bytes from the operating system's CSPRNG (RtlGenRandom). */
static v9x_u16 v9xsign_random(v9x_u8 *out, ULONG count)
{
    HMODULE advapi = LoadLibraryA("advapi32.dll");
    v9xsign_random_fn generate;

    if (advapi == NULL) {
        return V9X_FALSE;
    }
    generate = (v9xsign_random_fn)GetProcAddress(advapi, "SystemFunction036");
    if (generate == NULL || !generate(out, count)) {
        return V9X_FALSE;
    }
    return V9X_TRUE;
}

static int v9xsign_keygen(const char *path)
{
    v9x_u8 seed[32];
    v9x_u8 public_key[32];
    FILE *file;
    size_t i;

    file = fopen(path, "rb");
    if (file != NULL) {
        fclose(file);
        fprintf(stderr, "v9xsign: %s already exists; not replacing a key\n",
                path);
        return V9XSIGN_EXIT_ERROR;
    }
    if (!v9xsign_random(seed, 32ul)) {
        fputs("v9xsign: no random source\n", stderr);
        return V9XSIGN_EXIT_ERROR;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "v9xsign: cannot create %s\n", path);
        return V9XSIGN_EXIT_ERROR;
    }
    for (i = 0u; i < 32u; ++i) {
        fprintf(file, "%02x", (unsigned int)seed[i]);
    }
    fputs("\r\n", file);
    fclose(file);
    v9x_ed25519_public_key(seed, public_key);
    memset(seed, 0, sizeof(seed));
    v9xsign_print_hex(public_key, sizeof(public_key));
    return V9XSIGN_EXIT_OK;
}

static int v9xsign_public(const char *seed_argument)
{
    v9x_u8 seed[32];
    v9x_u8 public_key[32];

    if (!v9xsign_seed(seed_argument, seed)) {
        return V9XSIGN_EXIT_ERROR;
    }
    v9x_ed25519_public_key(seed, public_key);
    v9xsign_print_hex(public_key, sizeof(public_key));
    return V9XSIGN_EXIT_OK;
}

static int v9xsign_sign(const char *seed_argument, const char *path)
{
    v9x_u8 seed[32];
    v9x_u8 public_key[32];
    v9x_u8 signature[64];
    v9x_u8 *message;
    v9x_u32 length;

    if (!v9xsign_seed(seed_argument, seed)) {
        return V9XSIGN_EXIT_ERROR;
    }
    message = v9xsign_read_file(path, &length);
    if (message == NULL) {
        return V9XSIGN_EXIT_ERROR;
    }
    v9x_ed25519_public_key(seed, public_key);
    v9x_ed25519_sign(seed, public_key, message, length, signature);

    /* Check the signature before printing it: a signature that does not
     * verify here would be published and then refused by every client. */
    if (!v9x_ed25519_verify(signature, public_key, message, length)) {
        fputs("v9xsign: internal error, the new signature does not verify\n",
              stderr);
        free(message);
        return V9XSIGN_EXIT_ERROR;
    }
    free(message);
    v9xsign_print_hex(signature, sizeof(signature));
    return V9XSIGN_EXIT_OK;
}

static int v9xsign_verify(const char *public_hex,
                          const char *signature_hex,
                          const char *path)
{
    v9x_u8 public_key[32];
    v9x_u8 signature[64];
    v9x_u8 *message;
    v9x_u32 length;
    v9x_u16 valid;

    if (!v9xsign_parse_hex(public_hex, public_key, sizeof(public_key))) {
        fputs("v9xsign: the public key must be 64 hex digits\n", stderr);
        return V9XSIGN_EXIT_ERROR;
    }
    if (!v9xsign_parse_hex(signature_hex, signature, sizeof(signature))) {
        fputs("v9xsign: the signature must be 128 hex digits\n", stderr);
        return V9XSIGN_EXIT_ERROR;
    }
    message = v9xsign_read_file(path, &length);
    if (message == NULL) {
        return V9XSIGN_EXIT_ERROR;
    }
    valid = v9x_ed25519_verify(signature, public_key, message, length);
    free(message);
    if (!valid) {
        puts("INVALID");
        return V9XSIGN_EXIT_INVALID;
    }
    puts("VALID");
    return V9XSIGN_EXIT_OK;
}

static void v9xsign_put32(v9x_u8 *out, v9x_u32 value)
{
    out[0] = (v9x_u8)value;
    out[1] = (v9x_u8)(value >> 8);
    out[2] = (v9x_u8)(value >> 16);
    out[3] = (v9x_u8)(value >> 24);
}

static int v9xsign_relay(char **argv)
{
    v9x_u8 seed[32];
    v9x_u8 public_key[32];
    v9x_u8 header[V9X_RELAY_RECORD_HEADER];
    char message[V9X_RELAY_MESSAGE_MAX];
    v9x_u32 message_length;
    v9x_u8 *data;
    v9x_u32 length;
    v9x_u32 offset;
    v9x_u32 piece;
    unsigned long seq;
    char *end;
    FILE *out;

    if (!v9xsign_seed(argv[2], seed)) {
        return V9XSIGN_EXIT_ERROR;
    }
    seq = strtoul(argv[7], &end, 10);
    if (*argv[7] == '\0' || *end != '\0' || seq == 0ul) {
        fputs("v9xsign: first-seq must be a decimal number above zero\n",
              stderr);
        return V9XSIGN_EXIT_ERROR;
    }
    data = v9xsign_read_file(argv[8], &length);
    if (data == NULL) {
        return V9XSIGN_EXIT_ERROR;
    }
    out = fopen(argv[9], "wb");
    if (out == NULL) {
        fprintf(stderr, "v9xsign: cannot create %s\n", argv[9]);
        free(data);
        return V9XSIGN_EXIT_ERROR;
    }
    v9x_ed25519_public_key(seed, public_key);
    for (offset = 0ul; offset < length; offset += piece, ++seq) {
        piece = length - offset;
        if (piece > V9X_RELAY_CHUNK_MAX) {
            piece = V9X_RELAY_CHUNK_MAX;
        }
        if (!v9x_relay_message(argv[3], argv[4], argv[5], argv[6],
                               (v9x_u32)seq, data + offset, piece,
                               message, &message_length)) {
            fputs("v9xsign: malformed machine, install, nonce or session\n",
                  stderr);
            fclose(out);
            free(data);
            return V9XSIGN_EXIT_ERROR;
        }
        memcpy(header, V9X_RELAY_DOWN_MAGIC, 4u);
        v9xsign_put32(header + 4, (v9x_u32)seq);
        v9xsign_put32(header + 8, piece);
        v9x_ed25519_sign(seed, public_key, (const v9x_u8 *)message,
                         message_length, header + 12);
        if (fwrite(header, 1u, sizeof(header), out) != sizeof(header) ||
            fwrite(data + offset, 1u, piece, out) != piece) {
            fprintf(stderr, "v9xsign: cannot write %s\n", argv[9]);
            fclose(out);
            free(data);
            return V9XSIGN_EXIT_ERROR;
        }
    }
    memset(seed, 0, sizeof(seed));
    free(data);
    if (fclose(out) != 0) {
        fprintf(stderr, "v9xsign: cannot write %s\n", argv[9]);
        return V9XSIGN_EXIT_ERROR;
    }
    printf("%lu\n", seq);
    return V9XSIGN_EXIT_OK;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "keygen") == 0) {
        return v9xsign_keygen(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "public") == 0) {
        return v9xsign_public(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "sign") == 0) {
        return v9xsign_sign(argv[2], argv[3]);
    }
    if (argc == 5 && strcmp(argv[1], "verify") == 0) {
        return v9xsign_verify(argv[2], argv[3], argv[4]);
    }
    if (argc == 10 && strcmp(argv[1], "relay") == 0) {
        return v9xsign_relay(argv);
    }
    v9xsign_usage();
    return V9XSIGN_EXIT_ERROR;
}
