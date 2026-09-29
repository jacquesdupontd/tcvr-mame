// license:BSD-3-Clause
// TCVR: exact wall-clock accumulators for the big blocks of the emulation loop (sound update, screen update,
// scheduler timeslice), reported once a second by schedule.cpp next to the sampled per-device figures.
// Sampling 1/1024 was 5x wrong on the Z80s (15/09); a clock around a block that runs 60 times a second is exact.
#pragma once
#if defined(__ANDROID__)
#include <atomic>
#include <chrono>
extern std::atomic<unsigned long long> g_tcvr_exact_ns[8];
extern bool g_tcvr_exact_on;
struct tcvr_exact_scope
{
	int idx;
	std::chrono::steady_clock::time_point t0;
	explicit tcvr_exact_scope(int i) : idx(i) { if (g_tcvr_exact_on) t0 = std::chrono::steady_clock::now(); }
	~tcvr_exact_scope()
	{
		if (g_tcvr_exact_on)
			g_tcvr_exact_ns[idx].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
	}
};
#define TCVR_EXACT(i) tcvr_exact_scope tcvr_exact_scope_##i(i)
#else
#define TCVR_EXACT(i)
#endif
