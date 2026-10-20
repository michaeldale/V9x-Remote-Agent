#ifndef V9X_VNCZ_H
#define V9X_VNCZ_H

/* A small zlib-format deflate encoder for ZRLE, runtime-free C89.

   ZRLE uses one zlib stream per connection, and every rectangle must end on
   a byte boundary the decoder can consume without the next one, so each
   call emits one fixed-Huffman block over its input followed by a sync
   flush (an empty stored block), exactly like zlib's Z_SYNC_FLUSH. The
   stream is never finished, so no Adler-32 is ever written.

   Matching is greedy LZ77 with one candidate per 3-byte hash, inside the
   current input only (no references into earlier rectangles). That is
   plenty for ZRLE's data, which is already run-length and palette coded. */

#define VNC_DEFLATE_HASH_SIZE 4096

typedef struct VncDeflate {
    int header_written;
    unsigned long *head;   /* VNC_DEFLATE_HASH_SIZE entries, caller-owned */
} VncDeflate;

void vnc_deflate_init(VncDeflate *stream, unsigned long *head);

/* Worst-case output for `length` input bytes (9-bit literals, plus block,
   flush and the two-byte zlib header). */
unsigned long vnc_deflate_bound(unsigned long length);

/* Compress `length` bytes into `out`. Returns the bytes written, or 0 when
   `capacity` is too small (the stream is then unusable). */
unsigned long vnc_deflate_sync(VncDeflate *stream, const unsigned char *in,
                               unsigned long length, unsigned char *out,
                               unsigned long capacity);

#endif
