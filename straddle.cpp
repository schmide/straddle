// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Andrew Hallendorff
//
// straddle: does a loop's speed depend on where its code lands in memory?
//
// The loop is the one from "More than a taken branch per cycle?" (lemire.me,
// 2026-09-21): load a byte, compare it with a threshold, write it if larger. All data
// is below the threshold, so the write never happens and every iteration takes two
// jumps: over the skipped block, and back to the top of the loop.
//
// The sweep grows the skipped block one step at a time. That never changes the work
// done per iteration; it only slides the bottom of the loop (the increment, the
// compare, and the jump back) to new positions relative to 64-byte boundaries.
//
// Two modes, picked automatically:
//
//   asm mode  (GCC or Clang on x86-64 or ARM64)
//     The loop is written in inline assembly, so the compiler cannot reshape it.
//     The skipped block is GAP bytes of padding, and the program reads back the
//     exact address of each part of the loop. Every row reports whether either
//     straight run of code between taken jumps crosses a 64-byte boundary.
//
//   C++ mode  (MSVC, or any other compiler/CPU, or -DSTRADDLE_FORCE_CXX)
//     The skipped block is K volatile writes. Layout is up to the compiler, so check
//     the disassembly: the jump after the compare must go FORWARD over the writes,
//     with a separate jump back at the bottom. (GCC reshapes the loop from about
//     K = 16 on, which is why GCC and Clang default to asm mode.)
//
// Output: CSV on stdout, notes on stderr. Paste into a spreadsheet and chart
// "cycles" against the first column.
//
// Build: see README.md, or directly:
//   g++ -O2 -std=c++20 straddle.cpp -o straddle         (Linux / macOS)
//   cl /O2 /std:c++20 /EHsc straddle.cpp                 (MSVC)

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define NOINLINE __declspec(noinline)
#define FORCEINLINE __forceinline
#else
#define NOINLINE __attribute__((noinline))
#define FORCEINLINE inline __attribute__((always_inline))
#endif

#if !defined(STRADDLE_FORCE_CXX) && !(defined(_MSC_VER) && !defined(__clang__)) && \
    (defined(__x86_64__) || defined(__aarch64__))
#define STRADDLE_ASM 1
#else
#define STRADDLE_ASM 0
#endif

// Bytes of padding placed before the loop, after aligning to a 128-byte boundary
// (asm mode only). Changing it moves the whole loop. ARM64: use a multiple of 4.
#ifndef STRADDLE_START
#define STRADDLE_START 0
#endif

constexpr size_t N_ELEMS = size_t(1) << 16;   // 64 KiB of data per call
constexpr int REPS = 256;                       // calls per timing trial
constexpr int TRIALS = 7;                       // best of this many trials

// ---------------------------------------------------------------------------------
// Layout report filled in by asm-mode kernels (C++ mode leaves it empty).
// The loop runs as two straight stretches of code between taken jumps:
//   top run:    loop top .. end of the jump over the skipped block
//   bottom run: where that jump lands .. end of the jump back to the top
struct Layout {
    uintptr_t top = 0;        // first instruction of the loop
    uintptr_t top_end = 0;    // one past the jump over the skipped block
    uintptr_t land = 0;       // where that jump lands (increment, compare, ...)
    uintptr_t end = 0;        // one past the jump back to the top
};

using Kernel = void (*)(const uint8_t*, size_t, uint8_t, uint8_t*, Layout*);

#if STRADDLE_ASM
// =================================================================================
// asm mode
// =================================================================================
#if defined(__x86_64__)
constexpr int GAP_STEP = 1;     // any byte count works on x86
constexpr int N_STEPS = 256;    // gaps 0 .. 255 bytes
static const char* MODE_NAME = "asm, x86-64";

template <int GAP>
NOINLINE void kernel(const uint8_t* p, size_t n, uint8_t t, uint8_t* last, Layout* lay) {
    const uint8_t* end = p + n;
    uintptr_t top, te, ld, en;
    asm volatile(
        "leaq 1f(%%rip), %[top]\n\t"      // record addresses (runs once per call)
        "leaq 5f(%%rip), %[te]\n\t"
        "leaq 2f(%%rip), %[ld]\n\t"
        "leaq 4f(%%rip), %[en]\n\t"
        ".p2align 7\n\t"                   // 128-byte boundary (also a 64-byte one)
        ".rept %c[start]\n\tnop\n\t.endr\n"  // optional shift of the whole loop
        "1:\n\t"
        "movzbl (%[p]), %%eax\n\t"         // v = *p
        "cmpb %b[t], %%al\n\t"
        "jbe 2f\n"                         // miss: jump over the skipped block (TAKEN)
        "5:\n\t"
        "movb %%al, (%[last])\n\t"         // *last = v (never runs on this data)
        ".rept %c[gap]\n\tnop\n\t.endr\n"    // the skipped block: GAP one-byte nops
        "2:\n\t"
        "addq $1, %[p]\n\t"
        "cmpq %[end], %[p]\n\t"
        "jne 1b\n"                         // jump back to the top (TAKEN)
        "4:\n\t"
        : [p] "+r"(p), [top] "=&r"(top), [te] "=&r"(te), [ld] "=&r"(ld), [en] "=&r"(en)
        : [end] "r"(end), [t] "r"(t), [last] "r"(last),
          [gap] "i"(GAP), [start] "i"(STRADDLE_START)
        : "rax", "cc", "memory");
    if (lay) *lay = {top, te, ld, en};
}
#elif defined(__aarch64__)
constexpr int GAP_STEP = 4;     // ARM64 instructions are 4 bytes
constexpr int N_STEPS = 128;    // gaps 0 .. 508 bytes
static const char* MODE_NAME = "asm, ARM64";

template <int GAP>
NOINLINE void kernel(const uint8_t* p, size_t n, uint8_t t, uint8_t* last, Layout* lay) {
    const uint8_t* end = p + n;
    const uint32_t tt = t;
    uintptr_t top, te, ld, en;
    asm volatile(
        "adr %[top], 1f\n\t"               // record addresses (runs once per call)
        "adr %[te], 5f\n\t"
        "adr %[ld], 2f\n\t"
        "adr %[en], 4f\n\t"
        ".p2align 7\n\t"                   // 128-byte boundary
        ".rept %c[start] / 4\n\tnop\n\t.endr\n"
        "1:\n\t"
        "ldrb w9, [%[p]]\n\t"              // v = *p
        "cmp w9, %w[t]\n\t"
        "b.ls 2f\n"                        // miss: jump over the skipped block (TAKEN)
        "5:\n\t"
        "strb w9, [%[last]]\n\t"           // *last = v (never runs on this data)
        ".rept %c[gap] / 4\n\tnop\n\t.endr\n"
        "2:\n\t"
        "add %[p], %[p], #1\n\t"
        "cmp %[p], %[end]\n\t"
        "b.ne 1b\n"                        // jump back to the top (TAKEN)
        "4:\n\t"
        : [p] "+r"(p), [top] "=&r"(top), [te] "=&r"(te), [ld] "=&r"(ld), [en] "=&r"(en)
        : [end] "r"(end), [t] "r"(tt), [last] "r"(last),
          [gap] "i"(GAP), [start] "i"(STRADDLE_START)
        : "x9", "cc", "memory");
    if (lay) *lay = {top, te, ld, en};
}
#endif

template <size_t... I>
constexpr std::array<Kernel, sizeof...(I)> make_table(std::index_sequence<I...>) {
    return {&kernel<int(I) * GAP_STEP>...};
}
static const char* STEP_NAME = "gap_bytes";

#else
// =================================================================================
// C++ mode: the skipped block is K volatile writes
// =================================================================================
constexpr int GAP_STEP = 1;
constexpr int N_STEPS = 128;    // K = 0 .. 127
static const char* MODE_NAME = "C++ (check the disassembly)";
static const char* STEP_NAME = "K_writes";

volatile uint8_t sink[N_STEPS];

FORCEINLINE void put(size_t i, uint8_t v) { sink[i] = v; }

template <size_t... I>
FORCEINLINE void pad_writes(uint8_t v, std::index_sequence<I...>) {
    (void)v;   // unused when K = 0
    (put(I, v), ...);
}

template <size_t K>
NOINLINE void kernel(const uint8_t* p, size_t n, uint8_t thresh, uint8_t* last, Layout*) {
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = p[i];
        if (v > thresh) {
            *last = v;
            pad_writes(v, std::make_index_sequence<K>{});
        }
    }
}

template <size_t... I>
constexpr std::array<Kernel, sizeof...(I)> make_table(std::index_sequence<I...>) {
    return {&kernel<I>...};
}
#endif

// ---------------------------------------------------------------------------------
static double time_ns(Kernel f, const uint8_t* d, uint8_t thresh, uint8_t* last) {
    using clk = std::chrono::steady_clock;
    double best = 1e30;
    f(d, N_ELEMS, thresh, last, nullptr);   // warm-up
    for (int trial = 0; trial < TRIALS; trial++) {
        const auto t0 = clk::now();
        for (int r = 0; r < REPS; r++) f(d, N_ELEMS, thresh, last, nullptr);
        const auto t1 = clk::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                          (double(N_ELEMS) * REPS);
        if (ns < best) best = ns;
    }
    return best;
}

int main() {
    const std::vector<uint8_t> miss(N_ELEMS, 10);   // all below thresh: the write never runs
    const uint8_t thresh = 100;
    uint8_t last = 0;

    static constexpr auto table = make_table(std::make_index_sequence<N_STEPS>{});

    std::array<double, N_STEPS> ns{};
    std::array<Layout, N_STEPS> lay{};
    double fastest = 1e30;
    for (int i = 0; i < N_STEPS; i++) {
        table[i](miss.data(), N_ELEMS, thresh, &last, &lay[i]);   // record layout
        ns[i] = time_ns(table[i], miss.data(), thresh, &last);
        if (ns[i] < fastest) fastest = ns[i];
    }

    std::fprintf(stderr, "mode: %s   start offset: %d\n", MODE_NAME, STRADDLE_START);
    std::fprintf(stderr, "cycles = ns / fastest row (the fastest row is ~1 cycle/iter)\n");

    // top64 / land64: where each run starts within its 64-byte block.
    // crosses64: 1 if either run spans two 64-byte blocks (the predicted-slow case).
    // asm mode knows the loop's exact layout and prints it; C++ mode doesn't, so it
    // prints only the columns it can fill (read positions from the disassembly).
    if (STRADDLE_ASM)
        std::printf("%s,fn_mod64,top64,top_len,land64,bottom_len,crosses64,ns_per_iter,cycles\n",
                    STEP_NAME);
    else
        std::printf("%s,fn_mod64,ns_per_iter,cycles\n", STEP_NAME);

    auto crosses = [](uintptr_t a, uintptr_t b) { return (a / 64) != ((b - 1) / 64); };
    int slow = 0, cross = 0, cross_slow = 0, slow_nocross = 0;
    for (int i = 0; i < N_STEPS; i++) {
        const double cyc = ns[i] / fastest;
        const bool is_slow = cyc > 1.5;
        slow += is_slow;
        const unsigned fn64 = unsigned(reinterpret_cast<uintptr_t>(table[i]) & 63);
        std::printf("%d,%u,", i * GAP_STEP, fn64);
        if (STRADDLE_ASM) {
            const Layout& L = lay[i];
            const bool c = crosses(L.top, L.top_end) || crosses(L.land, L.end);
            cross += c;
            cross_slow += c && is_slow;
            slow_nocross += !c && is_slow;
            std::printf("%u,%u,%u,%u,%d,", unsigned(L.top & 63), unsigned(L.top_end - L.top),
                        unsigned(L.land & 63), unsigned(L.end - L.land), int(c));
        }
        std::printf("%.3f,%.2f\n", ns[i], cyc);
    }

    std::fprintf(stderr, "%d of %d rows slow (>1.5 cycles)\n", slow, N_STEPS);
    if (STRADDLE_ASM) {
        std::fprintf(stderr, "a run crosses a 64-byte boundary in %d rows; %d of those are slow\n",
                     cross, cross_slow);
        std::fprintf(stderr, "slow rows where no run crosses: %d\n", slow_nocross);
    }
}
