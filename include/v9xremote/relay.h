/*
 * Remote help relay: the pure half (docs\remote-help.md).
 *
 * A relay session is a byte tunnel between the operator's v9x_mcp.py or
 * v9xctl.ps1 and this agent's own protocol port, carried over plain HTTP
 * polling through the update server. The server only stores and forwards
 * bytes. Every chunk travelling towards the agent ("down") is signed with
 * Ed25519 on the operator's PC; the agent checks it against the public key
 * compiled in (include\v9xremote\remote_key.h) before a single byte reaches
 * the protocol handlers, so the server cannot inject commands.
 *
 * What a signature covers is the canonical text below, never the raw
 * transport bytes, so both sides build exactly the same octets:
 *
 *   V9XRELAY1\n
 *   machine=M-XXXXXX\n
 *   install=<install_id UUID>\n
 *   nonce=<32 lowercase hex, chosen by the agent per session>\n
 *   session=<32 lowercase hex, chosen by the server>\n
 *   seq=<decimal, 1 for the first chunk, +1 each chunk>\n
 *   sha256=<64 lowercase hex of the chunk bytes>\n
 *
 * The agent picks the nonce when it accepts a session, so a recorded
 * session cannot be replayed into a new one, and requires seq to be exactly
 * one more than the previous chunk, so chunks cannot be dropped, reordered
 * or repeated within a session.
 *
 * A down batch (the body of GET /v9agent/down) is zero or more records:
 *
 *   "V9XD"  4 bytes
 *   seq     u32 little endian
 *   length  u32 little endian, 1 .. V9X_RELAY_CHUNK_MAX
 *   sig     64 bytes Ed25519 (R || S)
 *   data    length bytes
 *
 * Up chunks (agent to operator) are posted one per request with seq in the
 * query and are not signed: a man in the middle could forge replies, never
 * commands.
 *
 * Pure: no I/O, no allocation, no C runtime.
 */
#ifndef V9XREMOTE_RELAY_H
#define V9XREMOTE_RELAY_H

#include "v9xremote/types.h"

#define V9X_RELAY_DOWN_MAGIC     "V9XD"
#define V9X_RELAY_RECORD_HEADER  76ul
#define V9X_RELAY_CHUNK_MAX      1048576ul

#define V9X_RELAY_MACHINE_LENGTH 8u   /* M- and six Crockford characters */
#define V9X_RELAY_HEX_ID_LENGTH  32u  /* session id and nonce */
#define V9X_RELAY_INSTALL_LENGTH 36u  /* the install_id UUID */

/* The canonical text is never longer than this. */
#define V9X_RELAY_MESSAGE_MAX    256u

/* Status of v9x_relay_next. */
#define V9X_RELAY_RECORD_OK   ((v9x_u16)0u)
#define V9X_RELAY_RECORD_END  ((v9x_u16)1u)
#define V9X_RELAY_RECORD_BAD  ((v9x_u16)2u)

struct v9x_relay_record {
    v9x_u32 seq;
    v9x_u32 length;
    const v9x_u8 *signature;
    const v9x_u8 *data;
};

/* M- followed by six characters of the Crockford set (no I, L, O or U). */
v9x_u16 v9x_relay_machine_valid(const char *machine);

/* Exactly 32 lowercase hex digits. */
v9x_u16 v9x_relay_hex_id_valid(const char *id);

/* Build the canonical text for one chunk into out (capacity at least
 * V9X_RELAY_MESSAGE_MAX). V9X_FALSE if an identifier is malformed. */
v9x_u16 v9x_relay_message(const char *machine,
                          const char *install,
                          const char *nonce,
                          const char *session,
                          v9x_u32 seq,
                          const v9x_u8 *data,
                          v9x_u32 length,
                          char *out,
                          v9x_u32 *out_length);

/* The next record of a down batch starting at *offset. On OK the record
 * points into batch and *offset moves past it. END when *offset is exactly
 * the batch length; BAD for anything malformed or truncated. */
v9x_u16 v9x_relay_next(const v9x_u8 *batch,
                       v9x_u32 batch_length,
                       v9x_u32 *offset,
                       struct v9x_relay_record *record);

/* Verify one record against the session's identity and public key. The
 * caller checks the sequence number itself. */
v9x_u16 v9x_relay_verify(const struct v9x_relay_record *record,
                         const v9x_u8 public_key[32],
                         const char *machine,
                         const char *install,
                         const char *nonce,
                         const char *session);

/* Lowercase hex of count bytes into out (2 * count + 1 with the NUL). */
void v9x_relay_hex(const v9x_u8 *bytes, v9x_u32 count, char *out);

#endif
