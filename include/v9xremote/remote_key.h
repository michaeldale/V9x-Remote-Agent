/*
 * The Ed25519 public key that signs remote-help relay chunks
 * (include\v9xremote\relay.h, docs\remote-help.md).
 *
 * The private half lives only on the developer's PC, in
 * %USERPROFILE%\.v9x\relay-signing.key, made by "v9xsign keygen" on
 * 2026-10-20. tools\v9xsign.c signs each chunk with it; the agent refuses
 * any chunk that does not verify under these bytes, so neither the server
 * nor anyone on the path can command a machine. Rotating it means shipping
 * an agent with the new key; agents still carrying the old one keep
 * accepting only the old key.
 */
#ifndef V9XREMOTE_REMOTE_KEY_H
#define V9XREMOTE_REMOTE_KEY_H

#define V9X_REMOTE_PUBLIC_KEY_HEX \
    "76f9f3963a12c1fad30aa2046c9b11df290a67b3d28c47755d96fe7fbdcaf31f"

#define V9X_REMOTE_PUBLIC_KEY { \
    0x76, 0xf9, 0xf3, 0x96, 0x3a, 0x12, 0xc1, 0xfa, \
    0xd3, 0x0a, 0xa2, 0x04, 0x6c, 0x9b, 0x11, 0xdf, \
    0x29, 0x0a, 0x67, 0xb3, 0xd2, 0x8c, 0x47, 0x75, \
    0x5d, 0x96, 0xfe, 0x7f, 0xbd, 0xca, 0xf3, 0x1f }

#endif
