/* Host simulator: run a real .clm light mask through the device's
 * clm_scale_mask (scale + mirror + seam blend) and the runtime's two-band
 * scroll wrap, dumping PGMs of each stage to spot texture-edge artifacts.
 *
 * Build: cc -std=c11 -Wall -Wextra -lm -o /tmp/sim_cloud_blend tools/sim_cloud_blend.c
 * Run:   /tmp/sim_cloud_blend tools/cloudgen/out/cloud_L2_v1.clm 1.13 0 260 /tmp/sim
 *        (args: clm path, scale, mirror(0/1), scroll_px, out prefix)
 * Deps:  liblz4 not needed — uses a tiny LZ4 block decoder below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/eva_cloud_scale.h"

/* Minimal LZ4 block decompressor (safe variant, no deps). */
static int lz4_decomp(const uint8_t *src, int src_len, uint8_t *dst, int dst_cap)
{
    int si = 0, di = 0;
    while (si < src_len) {
        uint8_t token = src[si++];
        int lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do { b = src[si++]; lit += b; } while (b == 255);
        }
        if (di + lit > dst_cap || si + lit > src_len) return -1;
        memcpy(dst + di, src + si, lit);
        di += lit; si += lit;
        if (si >= src_len) break;                 /* last literals */
        int off = src[si] | (src[si + 1] << 8);
        si += 2;
        int mlen = (token & 0x0F) + 4;
        if (mlen == 19) {
            uint8_t b;
            do { b = src[si++]; mlen += b; } while (b == 255);
        }
        if (off == 0 || di - off < 0 || di + mlen > dst_cap) return -1;
        for (int i = 0; i < mlen; ++i) { dst[di] = dst[di - off]; ++di; }
    }
    return di;
}

static void write_pgm(const char *path, const uint8_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, f);
    fclose(f);
    printf("wrote %s (%dx%d)\n", path, w, h);
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s <clm> <scale> <mirror> <scroll> <out_prefix>\n", argv[0]);
        return 1;
    }
    const char *clm_path = argv[1];
    float scale = strtof(argv[2], NULL);
    int mirror = atoi(argv[3]);
    int scroll = atoi(argv[4]);
    const char *pre = argv[5];

    FILE *f = fopen(clm_path, "rb");
    if (!f) { perror("clm"); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(fsz);
    fread(blob, 1, fsz, f);
    fclose(f);

    clm_header_t hdr;
    if (!clm_parse_header(blob, fsz, &hdr)) { fprintf(stderr, "bad clm\n"); return 1; }
    int sw = hdr.w, sh = hdr.h;
    uint8_t *light = malloc((size_t)sw * sh);
    int dec = lz4_decomp(blob + CLM_HEADER_BYTES, (int)hdr.comp_size[0],
                         light, sw * sh);
    if (dec != sw * sh) { fprintf(stderr, "lz4 %d\n", dec); return 1; }

    char path[256];
    snprintf(path, sizeof path, "%s_0_raw.pgm", pre);
    write_pgm(path, light, sw, sh);

    /* Stage 1: device resample (scale + mirror + seam blend). */
    uint8_t *scaled = malloc((size_t)sw * sh);
    clm_scale_mask(light, sw, sh, scaled, sw, sh, scale, mirror != 0);
    snprintf(path, sizeof path, "%s_1_scaled.pgm", pre);
    write_pgm(path, scaled, sw, sh);

    /* Stage 2: runtime two-band wrap at `scroll` (dst = 800-wide viewport
     * rows of the strip, exactly what the PPA bands blend). */
    int W = sw;
    uint8_t *wrapped = malloc((size_t)W * sh);
    int first_w = W - scroll;
    for (int y = 0; y < sh; ++y) {
        memcpy(&wrapped[y * W], &scaled[y * W + scroll], first_w);
        memcpy(&wrapped[y * W + first_w], &scaled[y * W], scroll);
    }
    snprintf(path, sizeof path, "%s_2_wrapped.pgm", pre);
    write_pgm(path, wrapped, W, sh);
    return 0;
}
