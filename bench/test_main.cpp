#include "bench_common.h"
#include <cstdio>

int main() {
    int failures = 0;

    printf("=== FEC Tests ===\n");
    failures += run_fec_tests();

    printf("\n=== CRC32 Tests ===\n");
    failures += run_crc32_tests();

    printf("\n=== Packet Cook Tests ===\n");
    failures += run_packet_tests();

    printf("\n");
    if (failures == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", failures);

    return failures > 0 ? 1 : 0;
}
