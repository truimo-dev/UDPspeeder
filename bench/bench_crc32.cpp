#include "nanobench.h"
#include "bench_common.h"
#include "crc32c.h"
#include "crc32/Crc32.h"
#include <cstdlib>
#include <string>

void register_crc32_benchmarks(void *bench_ptr) {
    auto &bench = *static_cast<ankerl::nanobench::Bench *>(bench_ptr);

    /* Fill a buffer with pseudo-random data */
    static char buf[1500];
    for (int i = 0; i < 1500; i++)
        buf[i] = (char)(rand() & 0xFF);

    /* --- Old CRC32 (zlib polynomial) baseline --- */
    for (int i = 0; i < bench_sizes_count; i++) {
        size_t sz = bench_sizes[i];
        std::string name = "crc32_old/" + std::to_string(sz) + "B";

        bench.run(name, [sz]() {
            auto r = crc32_fast(buf, sz);
            ankerl::nanobench::doNotOptimizeAway(r);
        });
    }

    /* --- CRC32C software --- */
    for (int i = 0; i < bench_sizes_count; i++) {
        size_t sz = bench_sizes[i];
        std::string name = "crc32c_sw/" + std::to_string(sz) + "B";

        bench.run(name, [sz]() {
            auto r = crc32c_sw(buf, sz);
            ankerl::nanobench::doNotOptimizeAway(r);
        });
    }

    /* --- CRC32C hardware (may be same as sw if no hw support) --- */
    if (crc32c_has_hw()) {
        for (int i = 0; i < bench_sizes_count; i++) {
            size_t sz = bench_sizes[i];
            std::string name = "crc32c_hw/" + std::to_string(sz) + "B";

            bench.run(name, [sz]() {
                auto r = crc32c_hw(buf, sz);
                ankerl::nanobench::doNotOptimizeAway(r);
            });
        }
    }

    /* --- CRC32C dispatched (production path) --- */
    for (int i = 0; i < bench_sizes_count; i++) {
        size_t sz = bench_sizes[i];
        std::string name = "crc32c/" + std::to_string(sz) + "B";

        bench.run(name, [sz]() {
            auto r = crc32c(buf, sz);
            ankerl::nanobench::doNotOptimizeAway(r);
        });
    }
}
