#include "bench_common.h"
#include "packet_cook.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Stubs for packet_cook.cpp dependencies — production uses common.cpp */
#ifndef BENCH_PACKET_STUBS_DEFINED
#define BENCH_PACKET_STUBS_DEFINED
void get_fake_random_chars(char *s, int len) {
    for (int i = 0; i < len; i++)
        s[i] = (char)(rand() & 0xFF);
}

int random_between(unsigned int a, unsigned int b) {
    if (a == b) return (int)a;
    return (int)(a + (unsigned int)rand() % (b + 1 - a));
}
#endif

#define TEST(name, expr) do { \
    if (!(expr)) { printf("  FAIL: %s\n", name); failures++; } \
    else { printf("  ok:   %s\n", name); } \
} while(0)

static int test_cook_roundtrip() {
    int failures = 0;
    cook_ctx_t ctx = {};
    strcpy(ctx.key, "testkey123");
    cook_ctx_prepare_key(&ctx);
    ctx.iv_min = 4;
    ctx.iv_max = 32;

    int sizes[] = { 1, 16, 64, 256, 1024, 1500 };
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int s = 0; s < nsizes; s++) {
        int sz = sizes[s];
        char orig[4096], buf[4096];

        /* Fill with pattern */
        for (int i = 0; i < sz; i++)
            orig[i] = (char)((i * 37 + 11) & 0xFF);
        memcpy(buf, orig, sz);

        int len = sz;
        int rc = do_cook(&ctx, buf, len);

        char label[80];
        snprintf(label, sizeof(label), "do_cook succeeds at %d bytes", sz);
        TEST(label, rc == 0 && len > sz);

        rc = de_cook(&ctx, buf, len);
        snprintf(label, sizeof(label), "de_cook succeeds at %d bytes", sz);
        TEST(label, rc == 0 && len == sz);

        snprintf(label, sizeof(label), "round-trip data matches at %d bytes", sz);
        TEST(label, memcmp(buf, orig, sz) == 0);
    }

    return failures;
}

static int test_cook_checksum_only() {
    int failures = 0;
    cook_ctx_t ctx = {};
    ctx.iv_min = 4;
    ctx.iv_max = 32;
    ctx.disable_obscure = 1;
    ctx.disable_xor = 1;

    char orig[1600], buf[1600];
    int sz = 100;
    for (int i = 0; i < sz; i++) orig[i] = (char)i;
    memcpy(buf, orig, sz);

    int len = sz;
    do_cook(&ctx, buf, len);
    TEST("checksum adds 4 bytes", len == sz + 4);

    int rc = de_cook(&ctx, buf, len);
    TEST("checksum round-trip succeeds", rc == 0 && len == sz);
    TEST("checksum data matches", memcmp(buf, orig, sz) == 0);

    /* Corrupt a byte and verify detection */
    memcpy(buf, orig, sz);
    len = sz;
    do_cook(&ctx, buf, len);
    buf[0] ^= 0x01;
    rc = de_cook(&ctx, buf, len);
    TEST("checksum detects corruption", rc != 0);

    return failures;
}

static int test_cook_disabled() {
    int failures = 0;
    cook_ctx_t ctx = {};
    ctx.iv_min = 4;
    ctx.iv_max = 32;
    ctx.disable_checksum = 1;
    ctx.disable_obscure = 1;
    ctx.disable_xor = 1;

    char buf[256];
    int sz = 100;
    for (int i = 0; i < sz; i++) buf[i] = (char)i;
    char orig[256];
    memcpy(orig, buf, sz);

    int len = sz;
    do_cook(&ctx, buf, len);
    TEST("all disabled: length unchanged", len == sz);
    TEST("all disabled: data unchanged", memcmp(buf, orig, sz) == 0);

    return failures;
}

static int test_xor_tile_roundtrip() {
    int failures = 0;
    int vec_w = bench_cook_vec_width();
    char label[128];

    int tile_lens[] = {vec_w, vec_w * 2, vec_w * 5};
    int num_tiles = 3;
    int data_lens[] = {1, 7, 8, 15, 16, 31, 32, 63, 64, 1500};
    int num_datas = 10;
    int offsets[] = {0, 1, 3, 7};
    int num_offsets = 4;

    for (int tl = 0; tl < num_tiles; tl++) {
        int tile_len = tile_lens[tl];
        char tile[256];
        for (int i = 0; i < tile_len; i++)
            tile[i] = (char)((i * 37 + 11) & 0xFF);

        for (int dl = 0; dl < num_datas; dl++) {
            int data_len = data_lens[dl];
            for (int ol = 0; ol < num_offsets; ol++) {
                int offset = offsets[ol];
                char backing[2048];
                char orig[2048];
                char *data = backing + offset;
                for (int i = 0; i < data_len; i++)
                    data[i] = (char)((i * 13 + 7) & 0xFF);
                memcpy(orig, data, data_len);

                /* XOR once should change data (tile is non-zero) */
                bench_xor_tile(data, data_len, tile, tile_len);
                int changed = (memcmp(data, orig, data_len) != 0);

                /* XOR again should restore original */
                bench_xor_tile(data, data_len, tile, tile_len);

                snprintf(label, sizeof(label),
                    "xor_tile tile=%d data=%d off=%d", tile_len, data_len, offset);
                TEST(label, changed && memcmp(data, orig, data_len) == 0);
            }
        }
    }
    return failures;
}

static int test_cook_combo(int disable_checksum, int disable_obscure, int disable_xor,
                           int sz) {
    int failures = 0;
    char label[128];
    const char *cs = disable_checksum ? "off" : "on";
    const char *ob = disable_obscure ? "off" : "on";
    const char *xr = disable_xor ? "off" : "on";

    cook_ctx_t ctx = {};
    strcpy(ctx.key, "testkey123");
    cook_ctx_prepare_key(&ctx);
    ctx.iv_min = 4;
    ctx.iv_max = 32;
    ctx.disable_checksum = disable_checksum;
    ctx.disable_obscure = disable_obscure;
    ctx.disable_xor = disable_xor;

    char orig[4096], buf[4096];
    for (int i = 0; i < sz; i++)
        orig[i] = (char)((i * 37 + 11) & 0xFF);
    memcpy(buf, orig, sz);

    int len = sz;
    int rc = do_cook(&ctx, buf, len);

    snprintf(label, sizeof(label), "cook cs=%s ob=%s xr=%s sz=%d: encode ok", cs, ob, xr, sz);
    TEST(label, rc == 0);

    rc = de_cook(&ctx, buf, len);
    snprintf(label, sizeof(label), "cook cs=%s ob=%s xr=%s sz=%d: decode ok", cs, ob, xr, sz);
    TEST(label, rc == 0 && len == sz);

    snprintf(label, sizeof(label), "cook cs=%s ob=%s xr=%s sz=%d: data matches", cs, ob, xr, sz);
    TEST(label, memcmp(buf, orig, sz) == 0);

    return failures;
}

static int test_cook_all_combos() {
    int failures = 0;
    int sizes[] = {64, 1500};
    for (int s = 0; s < 2; s++) {
        for (int cs = 0; cs <= 1; cs++)
            for (int ob = 0; ob <= 1; ob++)
                for (int xr = 0; xr <= 1; xr++)
                    failures += test_cook_combo(cs, ob, xr, sizes[s]);
    }
    return failures;
}

static int test_cook_unaligned() {
    int failures = 0;
    char label[128];
    int offsets[] = {0, 1, 3, 5, 7};
    int num_offsets = 5;
    int sizes[] = {64, 256, 1500};
    int nsizes = 3;

    for (int ol = 0; ol < num_offsets; ol++) {
        int offset = offsets[ol];
        for (int s = 0; s < nsizes; s++) {
            int sz = sizes[s];
            /* +offset for misalignment, +200 for cook overhead */
            char backing[4096];
            char orig[4096];
            char *buf = backing + offset;

            cook_ctx_t ctx = {};
            strcpy(ctx.key, "testkey123");
            cook_ctx_prepare_key(&ctx);
            ctx.iv_min = 4;
            ctx.iv_max = 32;

            for (int i = 0; i < sz; i++)
                buf[i] = (char)((i * 37 + 11) & 0xFF);
            memcpy(orig, buf, sz);

            int len = sz;
            do_cook(&ctx, buf, len);
            int rc = de_cook(&ctx, buf, len);

            snprintf(label, sizeof(label),
                "cook unaligned off=%d sz=%d: round-trip", offset, sz);
            TEST(label, rc == 0 && len == sz && memcmp(buf, orig, sz) == 0);
        }
    }
    return failures;
}

int run_packet_tests() {
    int failures = 0;

    printf("[cook round-trip]\n");
    failures += test_cook_roundtrip();

    printf("[cook checksum only]\n");
    failures += test_cook_checksum_only();

    printf("[cook all disabled]\n");
    failures += test_cook_disabled();

    printf("[xor_tile round-trip]\n");
    failures += test_xor_tile_roundtrip();

    printf("[cook all 8 enable/disable combos]\n");
    failures += test_cook_all_combos();

    printf("[cook unaligned buffers]\n");
    failures += test_cook_unaligned();

    return failures;
}
