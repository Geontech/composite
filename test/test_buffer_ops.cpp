// Standalone buffer tests (ops-table + truncate + overflow guards).
// Runs clean under ASan/UBSan.
#include <composite/buffers/buffer.hpp>
#include <composite/buffers/slab_pool.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>
int main() {
    using namespace composite;
    // ops-table: deep-copy independence, clear, aligned resize, promote, move
    auto b = make_mutable<float>(8);
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = float(i);
    b.resize(16); assert(b.size() == 16);
    b.reserve(64); assert(b.capacity() >= 64);
    auto c = b.copy(); c[3] = 999.0f;
    assert(b[3] == 3.0f && c[3] == 999.0f && c.size() == 16);
    auto cap = c.capacity(); c.clear(); assert(c.size() == 0 && c.capacity() == cap);
    auto ab = make_aligned_buffer<float>(64, 10);
    assert((reinterpret_cast<std::uintptr_t>(ab.data()) % 64) == 0);
    ab.resize(20); assert((reinterpret_cast<std::uintptr_t>(ab.data()) % 64) == 0);
    static_assert(sizeof(mutable_buffer<float>) <= 64, "ops-table buffer must be small");
    // truncate: shrink logical size, capacity preserved, grow rejected, promote
    auto t = make_mutable<int>(10);
    for (int i = 0; i < 10; ++i) t[i] = i;
    auto tcap = t.capacity(); t.truncate(4);
    assert(t.size() == 4 && t.capacity() == tcap && t[3] == 3);
    bool threw = false;
    try { t.truncate(8); } catch (const std::out_of_range&) { threw = true; }
    assert(threw);
    auto im = std::move(b).to_immutable(); assert(im.size() == 16 && im[3] == 3.0f);

    // --- Regression: integer-overflow guards (explicit checks; NDEBUG-safe,
    //     since assert() is voided under -DNDEBUG / RelWithDebInfo) ---
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++fails; }
    };
    constexpr auto kHuge = std::numeric_limits<std::size_t>::max();
    auto throws_oor = [](auto&& fn) {
        try { fn(); return false; } catch (const std::out_of_range&) { return true; } catch (...) { return false; }
    };
    auto throws_overflow = [](auto&& fn) {
        try { fn(); return false; } catch (const std::overflow_error&) { return true; } catch (...) { return false; }
    };
    // (1) immutable_buffer::slice: a near-SIZE_MAX count (e.g. an underflowed
    //     end-start from a parser) must be rejected, not wrap the bounds check
    //     into a multi-exabyte out-of-bounds view.
    {
        auto imm = make_immutable<float>({1, 2, 3, 4, 5, 6, 7, 8});
        check(throws_oor([&] { (void)imm.slice(2, kHuge - 1); }),
              "slice(2, ~SIZE_MAX) must throw out_of_range (overflow bypass)");
        auto s = imm.slice(2, 4);  // legal slice still works
        check(s.size() == 4 && s[0] == 3.0f, "legal slice(2,4) intact");
    }
    // (2) slab_pool: buffer_size * sizeof(T) overflow must throw, not yield stride
    //     0 (SIGFPE) or an undersized slab.
    check(throws_overflow([] { (void)slab_pool<std::uint64_t>::create(kHuge / 4, 1); }),
          "slab_pool buffer_size*sizeof(T) overflow must throw overflow_error");
    // (3) aligned_mem: count * sizeof(T) overflow must throw, not undersize-then-
    //     heap-overflow on the init loop.
    check(throws_overflow([] { (void)make_aligned_buffer<std::uint64_t>(64, kHuge / 4); }),
          "aligned_mem count*sizeof(T) overflow must throw overflow_error");
    // (4) immutable_buffer(external_buffer) adopts a caller-supplied size; an
    //     overflowing one must throw, not build a wrapped-length byte span.
    {
        std::vector<float> backing(4, 1.0f);
        check(throws_overflow([&] {
            external_buffer<float> eb(backing.data(), kHuge / 2, [](float*) {});  // bogus huge length
            (void)immutable_buffer<float>(std::move(eb));
        }), "immutable_buffer(external_buffer) overflowing size must throw overflow_error");
    }
    if (fails) { std::fprintf(stderr, "%d buffer-safety check(s) FAILED\n", fails); return 1; }

    std::printf("BUFFER OPS TESTS PASSED (sizeof mutable_buffer<float>=%zu)\n", sizeof(mutable_buffer<float>));
    return 0;
}
