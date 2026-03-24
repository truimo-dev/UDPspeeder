#include "bench_common.h"
#include "crc32c.h"
#include "crc32/Crc32.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TEST(name, expr) do { \
    if (!(expr)) { printf("  FAIL: %s\n", name); failures++; } \
    else { printf("  ok:   %s\n", name); } \
} while(0)

/* --- Old CRC32 (zlib polynomial) baseline regression anchor --- */

static int test_crc32_old_known_answer() {
    int failures = 0;

    /* Standard test vector: CRC32 of "123456789" = 0xCBF43926 */
    const char *tv = "123456789";
    uint32_t got = crc32_fast(tv, 9);
    char msg[128];
    snprintf(msg, sizeof(msg), "crc32_old(\"123456789\") = 0x%08X (expected 0xCBF43926)", got);
    TEST(msg, got == 0xCBF43926);

    /* Empty input */
    uint32_t empty = crc32_fast("", 0);
    snprintf(msg, sizeof(msg), "crc32_old(\"\") = 0x%08X (expected 0x00000000)", empty);
    TEST(msg, empty == 0x00000000);

    return failures;
}

/* --- CRC32C (Castagnoli) known-answer tests --- */

static int test_crc32c_known_answer() {
    int failures = 0;
    char msg[128];

    /* IETF/SCTP standard test vector: CRC32C of "123456789" = 0xE3069283 */
    const char *tv = "123456789";
    uint32_t sw = crc32c_sw(tv, 9);
    snprintf(msg, sizeof(msg), "crc32c_sw(\"123456789\") = 0x%08X (expected 0xE3069283)", sw);
    TEST(msg, sw == 0xE3069283);

    /* Empty input */
    uint32_t empty = crc32c_sw("", 0);
    snprintf(msg, sizeof(msg), "crc32c_sw(\"\") = 0x%08X (expected 0x00000000)", empty);
    TEST(msg, empty == 0x00000000);

    /* Dispatched version should agree */
    uint32_t dispatched = crc32c(tv, 9);
    snprintf(msg, sizeof(msg), "crc32c(\"123456789\") = 0x%08X (expected 0xE3069283)", dispatched);
    TEST(msg, dispatched == 0xE3069283);

    return failures;
}

/* --- Hardware vs software agreement --- */

static int test_crc32c_hw_sw_agree() {
    int failures = 0;
    char msg[128];

    if (!crc32c_has_hw()) {
        printf("  skip: no CRC32C hardware support detected\n");
        return 0;
    }

    /* Test across various sizes and data patterns */
    for (int i = 0; i < bench_sizes_count; i++) {
        size_t sz = bench_sizes[i];
        char *buf = (char *)malloc(sz);
        for (size_t j = 0; j < sz; j++)
            buf[j] = (char)((j * 13 + 7) & 0xFF);

        uint32_t sw = crc32c_sw(buf, sz);
        uint32_t hw = crc32c_hw(buf, sz);

        snprintf(msg, sizeof(msg), "crc32c hw==sw at %zu bytes (sw=0x%08X hw=0x%08X)",
                 sz, sw, hw);
        TEST(msg, sw == hw);

        free(buf);
    }

    /* Also test odd sizes that stress alignment/tail handling */
    int odd_sizes[] = {1, 3, 7, 15, 31, 63, 127, 255, 1023, 1499};
    for (int s = 0; s < (int)(sizeof(odd_sizes)/sizeof(odd_sizes[0])); s++) {
        int sz = odd_sizes[s];
        char *buf = (char *)malloc(sz);
        for (int j = 0; j < sz; j++)
            buf[j] = (char)((j * 41 + 3) & 0xFF);

        uint32_t sw = crc32c_sw(buf, sz);
        uint32_t hw = crc32c_hw(buf, sz);

        snprintf(msg, sizeof(msg), "crc32c hw==sw at %d bytes (odd)", sz);
        TEST(msg, sw == hw);

        free(buf);
    }

    return failures;
}

/* --- Incremental chaining --- */

static int test_crc32c_chaining() {
    int failures = 0;
    char msg[128];
    const int sz = 1024;
    char buf[1024];

    for (int i = 0; i < sz; i++)
        buf[i] = (char)((i * 17 + 5) & 0xFF);

    /* Full CRC in one shot */
    uint32_t full = crc32c(buf, sz);

    /* CRC in two halves, chained */
    uint32_t first_half = crc32c(buf, sz / 2);
    uint32_t chained = crc32c(buf + sz / 2, sz / 2, first_half);

    snprintf(msg, sizeof(msg),
             "crc32c chaining: full=0x%08X chained=0x%08X", full, chained);
    TEST(msg, full == chained);

    return failures;
}

static int test_crc32c_unaligned() {
    int failures = 0;
    char msg[128];

    int sizes[] = {64, 256, 1500};
    int offsets[] = {1, 3};

    for (int si = 0; si < 3; si++) {
        int sz = sizes[si];
        /* Allocate with extra headroom for offsets */
        char *raw = (char *)malloc(sz + 8);
        for (int i = 0; i < sz + 8; i++)
            raw[i] = (char)((i * 41 + 3) & 0xFF);

        for (int oi = 0; oi < 2; oi++) {
            int off = offsets[oi];
            int len = sz - off;

            uint32_t sw = crc32c_sw(raw + off, len);
            uint32_t dispatched = crc32c(raw + off, len);

            snprintf(msg, sizeof(msg),
                     "crc32c unaligned off=%d len=%d: dispatched==sw (0x%08X)",
                     off, len, sw);
            TEST(msg, dispatched == sw);

            if (crc32c_has_hw()) {
                uint32_t hw = crc32c_hw(raw + off, len);
                snprintf(msg, sizeof(msg),
                         "crc32c unaligned off=%d len=%d: hw==sw (0x%08X)",
                         off, len, sw);
                TEST(msg, hw == sw);
            }
        }
        free(raw);
    }

    return failures;
}

int run_crc32_tests() {
    int failures = 0;

    printf("[CRC32 old known-answer]\n");
    failures += test_crc32_old_known_answer();

    printf("[CRC32C known-answer]\n");
    failures += test_crc32c_known_answer();

    printf("[CRC32C hw vs sw agreement]\n");
    failures += test_crc32c_hw_sw_agree();

    printf("[CRC32C incremental chaining]\n");
    failures += test_crc32c_chaining();

    printf("[CRC32C unaligned input]\n");
    failures += test_crc32c_unaligned();

    return failures;
}
