#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

#include <stddef.h>

/* gf type matches lib/fec.cpp for GF_BITS=8 */
typedef unsigned char gf;

/* Exposed by lib/fec.cpp when compiled with -DBENCH_EXPOSE_INTERNALS */
extern "C++" void bench_addmul1(gf *dst, gf *src, gf c, int sz);

/* Exposed by packet_cook.cpp when compiled with -DBENCH_EXPOSE_INTERNALS */
extern "C++" void bench_xor_tile(char *data, int len, const char *tile, int tile_len);
extern "C++" int bench_cook_vec_width();
extern "C++" const char *bench_xor_tile_impl();

/* Exposed by lib/fec.cpp when compiled with -DBENCH_EXPOSE_INTERNALS */
extern "C++" const char *bench_addmul1_impl();

/* Packet sizes representative of real traffic */
static const size_t bench_sizes[] = { 64, 256, 1024, 1500 };
static const int bench_sizes_count = sizeof(bench_sizes) / sizeof(bench_sizes[0]);

/* Registration functions called from bench_main.cpp */
void register_fec_benchmarks(void *bench_ptr);
void register_crc32_benchmarks(void *bench_ptr);
void register_packet_benchmarks(void *bench_ptr);

/* Registration functions called from test_main.cpp */
int run_fec_tests();
int run_crc32_tests();
int run_packet_tests();

#endif
