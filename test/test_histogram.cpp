#include <composite/metrics/types.hpp>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
using composite::metrics::histogram;
int main() {
    auto bounds = histogram::power_of_2_boundaries(10);  // {1,2,4,...,256}
    // Equivalence: a pow2-flagged hist and a plain hist with the same boundaries
    // must put every value in the SAME bucket (the old fast path failed for 1.5).
    histogram a{bounds}; histogram b{bounds}; b.enable_power_of_2_lookup();
    for (double v : {0.5, 1.0, 1.5, 2.0, 3.0, 7.9, 8.0, 100.0, 1000.0}) { a.record(v); b.record(v); }
    assert(a.snapshot().bucket_counts == b.snapshot().bucket_counts);
    // Multi-writer: 8 threads x 100k records; after join, sum(buckets) == count.
    histogram h{bounds};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) ts.emplace_back([&]{ for (int i = 0; i < 100000; ++i) h.record(double(i % 300)); });
    for (auto& th : ts) th.join();
    auto snap = h.snapshot();
    uint64_t s = 0; for (auto c : snap.bucket_counts) s += c;
    assert(snap.count == 800000 && s == 800000);
    printf("HISTOGRAM OK (pow2==generic bucketing; multi-writer count=%llu sum(buckets)=%llu)\n",
           (unsigned long long)snap.count, (unsigned long long)s);
    return 0;
}
