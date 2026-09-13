/*
 * Minimal PNG writer (Luma-authored, public domain dedication).
 *
 * Writes 8-bit RGB/RGBA images with uncompressed deflate blocks
 * (valid PNG: zlib wrapper + stored blocks + Adler-32). No
 * compression, no dependencies, no gamma/chroma handling — the
 * caller converts rows first. Deterministic output for a given
 * input (no timestamps in the stream).
 *
 * Scope: screenshots, thumbnails, test captures. NOT a general
 * image library (that stays in Luma Assets / stb).
 *
 * Layout: row-major top-first bytes, `stride` bytes per row
 * (== width * channels for tight rows). Channels: 3 (RGB) or 4
 * (RGBA). Returns 0 on success, nonzero on I/O or arg failure.
 */
#ifndef LUMA_PNG_MINI_H
#define LUMA_PNG_MINI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t lpng_crc_table[256];
static int lpng_crc_ready = 0;

static void lpng_crc_init(void) {
    uint32_t i;
    uint32_t j;
    uint32_t c;

    if (lpng_crc_ready) {
        return;
    }
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        lpng_crc_table[i] = c;
    }
    lpng_crc_ready = 1;
}

static uint32_t lpng_crc_update(uint32_t crc, const unsigned char *data,
                                size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        crc = lpng_crc_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

static void lpng_put_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)((v >> 24) & 0xFFu);
    p[1] = (unsigned char)((v >> 16) & 0xFFu);
    p[2] = (unsigned char)((v >> 8) & 0xFFu);
    p[3] = (unsigned char)(v & 0xFFu);
}

static void lpng_put_u16(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

/* Write one chunk (length + type + data + crc) to f. */
static int lpng_chunk(FILE *f, const char type[4],
                      const unsigned char *data, size_t len) {
    unsigned char head[8];
    unsigned char crcb[4];
    uint32_t crc;

    lpng_put_u32(head, (uint32_t)len);
    memcpy(head + 4, type, 4);
    if (fwrite(head, 1, 8, f) != 8) {
        return -1;
    }
    crc = lpng_crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    if (len > 0) {
        if (fwrite(data, 1, len, f) != len) {
            return -1;
        }
        crc = lpng_crc_update(crc, data, len);
    }
    lpng_put_u32(crcb, crc ^ 0xFFFFFFFFu);
    if (fwrite(crcb, 1, 4, f) != 4) {
        return -1;
    }
    return 0;
}

static int lpng_write(const char *path, uint32_t width, uint32_t height,
                      int channels, const unsigned char *rows,
                      size_t stride) {
    static const unsigned char sig[8] = { 137, 80, 78, 71,
                                          13,  10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *zbuf = NULL;
    size_t rowbytes;
    size_t zlen;
    size_t pos;
    uint32_t y;
    uint32_t adler_s1 = 1;
    uint32_t adler_s2 = 0;
    unsigned char adlerb[4];
    unsigned char zhead[2];
    FILE *f = NULL;
    int rc = -1;

    if (path == NULL || rows == NULL || width == 0 || height == 0 ||
        (channels != 3 && channels != 4)) {
        return -1;
    }
    /* Stored-block rows cap: each zlib stored block holds <= 65535
     * bytes; one row per block keeps indexing trivial. */
    rowbytes = (size_t)width * (uint32_t)channels + 1u;
    if (rowbytes - 1u > 65535u) {
        return -1;
    }
    zlen = 2u + (size_t)height * (rowbytes + 5u) + 4u;
    zbuf = (unsigned char *)malloc(zlen);
    if (zbuf == NULL) {
        return -1;
    }
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "wb") != 0) {
        f = NULL;
    }
#else
    f = fopen(path, "wb");
#endif
    if (f == NULL) {
        free(zbuf);
        return -1;
    }
    if (fwrite(sig, 1, 8, f) != 8) {
        goto done;
    }
    lpng_crc_init();
    lpng_put_u32(ihdr, width);
    lpng_put_u32(ihdr + 4, height);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = (channels == 4) ? 6 : 2; /* RGBA : RGB */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* no interlace */
    ihdr[12] = 0; /* filter method 0 */
    if (lpng_chunk(f, "IHDR", ihdr, 13) != 0) {
        goto done;
    }
    /* zlib stream: header + stored rows + adler. */
    zhead[0] = 0x78;
    zhead[1] = 0x01;
    pos = 0;
    zbuf[pos++] = zhead[0];
    zbuf[pos++] = zhead[1];
    for (y = 0; y < height; y++) {
        const unsigned char *row = rows + (size_t)y * stride;
        uint32_t n = (uint32_t)rowbytes;
        size_t k;

        zbuf[pos++] = (y + 1 == height) ? 0x01 : 0x00; /* final? */
        lpng_put_u16(zbuf + pos, n);
        pos += 2;
        lpng_put_u16(zbuf + pos, n ^ 0xFFFFu);
        pos += 2;
        zbuf[pos++] = 0; /* filter 0 (none) */
        memcpy(zbuf + pos, row, (size_t)n - 1u);
        pos += (size_t)n - 1u;
        /* Adler over filter byte + row bytes (filter byte is 0,
         * handled as k == 0 below). Modulo every step: s2 would
         * overflow 32 bits otherwise. */
        for (k = 0; k < (size_t)n; k++) {
            /* k == 0 is the filter byte (0). */
            unsigned char b = (k == 0) ? 0 : row[k - 1];

            adler_s1 += b;
            if (adler_s1 >= 65521u) {
                adler_s1 -= 65521u;
            }
            adler_s2 += adler_s1;
            if (adler_s2 >= 65521u) {
                adler_s2 -= 65521u;
            }
        }
    }
    adler_s1 %= 65521u;
    adler_s2 %= 65521u;
    lpng_put_u32(adlerb, (adler_s2 << 16) | adler_s1);
    memcpy(zbuf + pos, adlerb, 4);
    pos += 4;
    if (lpng_chunk(f, "IDAT", zbuf, pos) != 0) {
        goto done;
    }
    if (lpng_chunk(f, "IEND", NULL, 0) != 0) {
        goto done;
    }
    rc = 0;
done:
    fclose(f);
    free(zbuf);
    return rc;
}

#endif /* LUMA_PNG_MINI_H */
