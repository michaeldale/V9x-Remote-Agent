/*
 * The Ed25519 public key that signs V9x Remote Agent releases (SIGNED.TXT,
 * include\v9xremote\update_release.h).
 *
 * scripts\sign-release.ps1 signs each release's SIGNED.TXT with the private
 * half, which lives only on the developer's PC in
 * %USERPROFILE%\.v9x\release-signing.key (made by "v9xsign keygen" on
 * 2026-10-20) and is checked to derive exactly these bytes before anything
 * is signed. SETUP.EXE /update refuses a release whose signature does not
 * verify under this key. Separate from the relay key (remote_key.h), so one
 * can be rotated without the other.
 */
#ifndef V9XREMOTE_RELEASE_KEY_H
#define V9XREMOTE_RELEASE_KEY_H

#define V9X_RELEASE_PUBLIC_KEY_HEX \
    "98406c9634648a373fd8a14744c92ab826151983ccaef08db41b037cdadef317"

#define V9X_RELEASE_PUBLIC_KEY { \
    0x98, 0x40, 0x6c, 0x96, 0x34, 0x64, 0x8a, 0x37, \
    0x3f, 0xd8, 0xa1, 0x47, 0x44, 0xc9, 0x2a, 0xb8, \
    0x26, 0x15, 0x19, 0x83, 0xcc, 0xae, 0xf0, 0x8d, \
    0xb4, 0x1b, 0x03, 0x7c, 0xda, 0xde, 0xf3, 0x17 }

#endif
