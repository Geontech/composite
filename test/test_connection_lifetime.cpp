// Connection-layer ownership: an input port deregisters from its producer output's fan-out
// on destruction, and an output clears its consumers' back-pointers on its own destruction —
// so destroying a connected pair (in EITHER order) leaves no dangling pointer and a later
// send touches no freed memory. The forever-dangling-pointer UAF (a producer's fan-out list
// holding a freed input* and dereferencing it on every subsequent send) is the bug this
// closes. Run under ASan/UBSan to actually catch a UAF; the connection_count assertions also
// verify the deregister logic in plain builds. Own main(); explicit checks.
#include <composite/buffers/buffer.hpp>
#include <composite/ports/output_port.hpp>

#include <cstdio>

using namespace composite;

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

int main() {
    // ---- (1) input destroyed BEFORE output: output must drop the (would-be dangling) entry ----
    {
        output_port<immutable_buffer<float>> out{"out"};
        {
            input_port<immutable_buffer<float>> in{"in", 8};
            check(out.connect(&in), "connect out->in");
            check(out.connection_count() == 1, "1 connection after connect");
            out.send_data(make_immutable<float>({1.0F}), timestamp{}); // sanity: send to a live input
        } // ~in -> must deregister from `out`
        check(out.connection_count() == 0, "input deregistered from output on its destruction");
        // The crux: this send must NOT dereference the freed input (ASan would flag a UAF).
        out.send_data(make_immutable<float>({2.0F}), timestamp{});
        check(out.connection_count() == 0, "still 0 connections after a post-destroy send");
        std::puts("case1 (input-first destroy) ok");
    }

    // ---- (2) output destroyed BEFORE input: output must clear the input's back-pointer ----
    {
        input_port<immutable_buffer<float>> in{"in2", 8};
        {
            output_port<immutable_buffer<float>> out{"out2"};
            check(out.connect(&in), "connect out2->in2");
        } // ~out -> clears in.m_producer + releases the claim; ~in (later) must see null and not touch freed `out`
        // The input is reconnectable now that its producer is gone (claim released).
        output_port<immutable_buffer<float>> out2{"out2b"};
        check(out2.connect(&in), "input reconnectable after its producer was destroyed");
        check(out2.connection_count() == 1, "reconnected to a new producer");
        std::puts("case2 (output-first destroy) ok");
    } // ~out2b first (clears in2's back-ptr), then ~in2 (no-op) — no UAF either way

    // ---- (3) fan-out: destroy one consumer; the other + sends stay valid ----
    {
        output_port<immutable_buffer<float>> out{"out3"};
        input_port<immutable_buffer<float>> a{"a", 8};
        {
            input_port<immutable_buffer<float>> b{"b", 8};
            check(out.connect(&a), "connect a");
            check(out.connect(&b), "connect b");
            check(out.connection_count() == 2, "2 connections");
            out.send_data(make_immutable<float>({3.0F}), timestamp{});
        } // ~b -> deregisters, leaving a
        check(out.connection_count() == 1, "fan-out drops only the destroyed consumer");
        out.send_data(make_immutable<float>({4.0F}), timestamp{}); // to `a` only — no UAF on freed `b`
        check(out.connection_count() == 1, "still 1 after post-destroy send");
        std::puts("case3 (fan-out partial destroy) ok");
    }

    if (g_fails == 0) {
        std::puts("CONNECTION LIFETIME OK");
    }
    return g_fails == 0 ? 0 : 1;
}
