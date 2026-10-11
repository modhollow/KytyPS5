// Pipeline compile worker threads: LocalPlatform::MakeCompileThread runs a worker at reduced power
// (Windows: EcoQoS) only when asked to and not turned off with KYTY_COMPILE_EFFICIENCY=0; a worker the game
// may wait for keeps full power. Where the host has no per-thread power state the worker is background
// priority only and says so.
#include "local/local-platform.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

struct Outcome {
	bool reported  = false;
	bool throttled = false;
};

// MakeCompileThread on a fresh thread, as a compile worker calls it first thing.
Outcome OnNewThread(bool efficient) {
	Outcome outcome;
	std::thread([&] {
		outcome.reported  = LocalPlatform::MakeCompileThread(nullptr, efficient);
		outcome.throttled = LocalPlatform::ThreadPowerThrottled();
	}).join();
	return outcome;
}

void SetEfficiencySwitch(const char* value) {
#ifdef _WIN32
	(void)_putenv_s("KYTY_COMPILE_EFFICIENCY", value);
#else
	(void)setenv("KYTY_COMPILE_EFFICIENCY", value, 1);
#endif
}

} // namespace

int main() {
	bool fresh = true;
	std::thread([&] { fresh = LocalPlatform::ThreadPowerThrottled(); }).join();
	Check(!fresh, "a new thread starts at full power");

	const auto efficient = OnNewThread(true);
#ifdef _WIN32
	Check(efficient.reported, "an optimize-pool worker reports reduced power");
	Check(efficient.throttled, "an optimize-pool worker runs power-throttled (EcoQoS)");
#else
	Check(!efficient.reported && !efficient.throttled,
	      "no per-thread power state on this host: background priority only, reported as such");
#endif

	const auto waited = OnNewThread(false);
	Check(!waited.reported && !waited.throttled, "a worker the game may wait for (native, table) keeps full power");

	SetEfficiencySwitch("0");
	const auto off = OnNewThread(true);
	Check(!off.reported && !off.throttled, "KYTY_COMPILE_EFFICIENCY=0 keeps an optimize-pool worker at full power");

	SetEfficiencySwitch("1");
	const auto on = OnNewThread(true);
	Check(on.reported == efficient.reported && on.throttled == efficient.throttled,
	      "KYTY_COMPILE_EFFICIENCY=1 is the default behaviour");

	if (g_failures != 0) {
		std::printf("CompileThreadPowerTests: %d failures\n", g_failures);
		return 1;
	}
	std::printf("CompileThreadPowerTests: all passed\n");
	return 0;
}
