/* Host test: CLP2 pack TOC parser.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_clp_toc tools/test_clp_toc.c && /tmp/test_clp_toc
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/eva_clp_toc.h"

static uint8_t blob[256];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static size_t build_blob(uint16_t count)
{
    memcpy(blob, "CLP2", 4);
    blob[4] = (uint8_t)count;
    blob[5] = (uint8_t)(count >> 8);
    size_t off = 6;
    for (int i = 0; i < count; ++i) {
        blob[off + 0] = (uint8_t)(i % 3);      /* layer / subtype */
        blob[off + 1] = (uint8_t)(i % 2);      /* pool  */
        blob[off + 2] = (uint8_t)i;            /* variant */
        blob[off + 3] = (uint8_t)(i % 6);      /* type */
        put32(&blob[off + 4], 6u + (uint32_t)count * 12u + i * 10u);
        put32(&blob[off + 8], 10u);
        off += 12;
    }
    return off + count * 10u;
}

static void test_parse_ok(void)
{
    size_t total = build_blob(4);
    eva_clp_toc_t toc;
    assert(eva_clp_parse(blob, total, &toc));
    assert(toc.count == 4);
    const eva_clp_entry_t *e = eva_clp_entry(&toc, 1);
    assert(e && e->layer == 1 && e->pool == 1 && e->variant == 1);
    assert(e->offset == 6 + 4 * 12 + 10 && e->size == 10);
    const eva_clp_entry_t *e2 = eva_clp_entry(&toc, 3);
    assert(e2 && e2->type == 3);
}

static void test_bad_magic(void)
{
    size_t total = build_blob(1);
    blob[0] = 'X';
    eva_clp_toc_t toc;
    assert(!eva_clp_parse(blob, total, &toc));
}

static void test_out_of_range_entry_rejected(void)
{
    size_t total = build_blob(2);
    /* Corrupt entry 0's size so offset+size overruns the blob. */
    put32(&blob[6 + 8], 100000u);
    eva_clp_toc_t toc;
    assert(!eva_clp_parse(blob, total, &toc));
}

static void test_old_magic_rejected(void)
{
    /* build_blob() rewrites the whole header+TOC (including bytes 0-3),
     * so any corruption a prior test left in `blob` doesn't leak in here. */
    size_t total = build_blob(1);
    memcpy(blob, "CLP1", 4);
    eva_clp_toc_t toc;
    assert(!eva_clp_parse(blob, total, &toc));
}

int main(void)
{
    test_parse_ok();
    test_bad_magic();
    test_out_of_range_entry_rejected();
    test_old_magic_rejected();
    printf("OK\n");
    return 0;
}
