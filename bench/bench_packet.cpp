#include "nanobench.h"
#include "bench_common.h"
#include "packet_cook.h"
#include <cstdlib>
#include <cstring>
#include <string>

/* Stubs for packet_cook.cpp dependencies — production uses common.cpp */
void get_fake_random_chars(char *s, int len) {
    for (int i = 0; i < len; i++)
        s[i] = (char)(rand() & 0xFF);
}

int random_between(unsigned int a, unsigned int b) {
    if (a == b) return (int)a;
    return (int)(a + (unsigned int)rand() % (b + 1 - a));
}

static cook_ctx_t make_ctx(int checksum, int obscure, int xor_enc) {
    cook_ctx_t ctx = {};
    strcpy(ctx.key, "benchmarkkey1234");
    cook_ctx_prepare_key(&ctx);
    ctx.iv_min = 16;
    ctx.iv_max = 16;
    ctx.disable_checksum = !checksum;
    ctx.disable_obscure = !obscure;
    ctx.disable_xor = !xor_enc;
    return ctx;
}

void register_packet_benchmarks(void *bench_ptr) {
    auto &bench = *static_cast<ankerl::nanobench::Bench *>(bench_ptr);

    /* Full pipeline: do_cook at all sizes */
    for (int i = 0; i < bench_sizes_count; i++) {
        int sz = (int)bench_sizes[i];
        std::string name = "do_cook/" + std::to_string(sz) + "B";
        cook_ctx_t ctx = make_ctx(1, 1, 1);

        bench.run(name, [sz, &ctx]() {
            static char buf[4096];
            memset(buf, 0xAB, sz);
            int len = sz;
            do_cook(&ctx, buf, len);
            ankerl::nanobench::doNotOptimizeAway(buf[0]);
        });
    }

    /* Full pipeline: de_cook at all sizes */
    for (int i = 0; i < bench_sizes_count; i++) {
        int sz = (int)bench_sizes[i];
        std::string name = "de_cook/" + std::to_string(sz) + "B";
        cook_ctx_t ctx = make_ctx(1, 1, 1);

        /* Prepare a cooked buffer */
        static char cooked[4096];
        memset(cooked, 0xAB, sz);
        int cooked_len = sz;
        do_cook(&ctx, cooked, cooked_len);
        int saved_len = cooked_len;

        bench.run(name, [saved_len, &ctx]() {
            static char buf[4096];
            memcpy(buf, cooked, saved_len);
            int len = saved_len;
            de_cook(&ctx, buf, len);
            ankerl::nanobench::doNotOptimizeAway(buf[0]);
        });
    }

    /* Component: checksum only at 1500B */
    {
        cook_ctx_t ctx = make_ctx(1, 0, 0);
        bench.run("cook_crc32_only/1500B", [&ctx]() {
            static char buf[4096];
            memset(buf, 0xAB, 1500);
            int len = 1500;
            do_cook(&ctx, buf, len);
            ankerl::nanobench::doNotOptimizeAway(buf[0]);
        });
    }

    /* Component: obscure only at 1500B */
    {
        cook_ctx_t ctx = make_ctx(0, 1, 0);
        bench.run("cook_obscure_only/1500B", [&ctx]() {
            static char buf[4096];
            memset(buf, 0xAB, 1500);
            int len = 1500;
            do_cook(&ctx, buf, len);
            ankerl::nanobench::doNotOptimizeAway(buf[0]);
        });
    }

    /* Component: xor only at 1500B */
    {
        cook_ctx_t ctx = make_ctx(0, 0, 1);
        bench.run("cook_xor_only/1500B", [&ctx]() {
            static char buf[4096];
            memset(buf, 0xAB, 1500);
            int len = 1500;
            do_cook(&ctx, buf, len);
            ankerl::nanobench::doNotOptimizeAway(buf[0]);
        });
    }
}
