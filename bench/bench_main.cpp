#define ANKERL_NANOBENCH_IMPLEMENT
#include "nanobench.h"
#include "bench_common.h"
#include "lib/rs.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

int main(int argc, char *argv[]) {
    bool json_output = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json_output = true;
    }

    /* Force FEC init so addmul1 dispatch is resolved */
    { void *d = fec_new(2, 3); fec_free(d); }

    printf("SIMD: addmul1=%s  xor_cook=%s  vec_width=%d\n",
        bench_addmul1_impl(), bench_xor_tile_impl(), bench_cook_vec_width());

    ankerl::nanobench::Bench bench;
    bench.title("UDPspeeder").warmup(3).epochs(21).relative(false);

    register_fec_benchmarks(&bench);
    register_crc32_benchmarks(&bench);
    register_packet_benchmarks(&bench);

    /* Emit stability warnings for noisy benchmarks */
    {
        auto results = bench.results();
        for (size_t i = 0; i < results.size(); i++) {
            double mdape = results[i].medianAbsolutePercentError(
                ankerl::nanobench::Result::Measure::elapsed);
            if (mdape > 0.05) {
                fprintf(stderr, "WARNING: %s has MdAPE %.1f%% (>5%%)\n",
                    results[i].config().mBenchmarkName.c_str(), mdape * 100.0);
            }
        }
    }

    if (json_output) {
        /* github-action-benchmark customSmallerIsBetter format
         * Mustache templates can't do math, so we extract results manually */
        std::ofstream out("bench_results.json");
        auto results = bench.results();
        out << "[\n";
        for (size_t i = 0; i < results.size(); i++) {
            double ns = results[i].median(ankerl::nanobench::Result::Measure::elapsed) * 1e9;
            out << "  {\n"
                << "    \"name\": \"" << results[i].config().mBenchmarkName << "\",\n"
                << "    \"unit\": \"ns/op\",\n"
                << "    \"value\": " << ns << "\n"
                << "  }";
            if (i + 1 < results.size()) out << ",";
            out << "\n";
        }
        out << "]\n";
    }

    return 0;
}
