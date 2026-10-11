#include "graphics/host_gpu/renderer/dispatchBatchPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
using namespace Libs::Graphics;
template <typename Busy>
bool Policy(uint32_t& recorded, uint32_t batch, bool recorder, Busy&& busy) {
	return ShouldSubmitDispatch(recorded, recorder, [&] { return batch; }, busy);
}
void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "DispatchBatchPolicyTests: %s\n", message);
		std::abort();
	}
}

void CheckBoundary(uint32_t requested, uint32_t expected, bool busy, uint32_t expected_queries) {
	uint32_t recorded = 0;
	uint32_t queries = 0;
	for (uint32_t dispatch = 1; dispatch <= expected; ++dispatch) {
		const auto submit = Policy(recorded, requested, false, [&] { ++queries; return busy; });
		Check(submit == (dispatch == expected), "submission boundary differs from controlled oracle");
		Check(recorded == dispatch, "recorded count changes once per dispatch");
	}
	Check(queries == expected_queries, "unexpected number of busy queries");
	ResetDispatchBatch(recorded);
	Check(recorded == 0, "submission reset clears batch");
	queries = 0;
	uint32_t batch_reads = 0;
	Check(!ShouldSubmitDispatch(recorded, true, [&] { ++batch_reads; return 32u; }, [&] { ++queries; return false; }) &&
	          recorded == 0 && queries == 0 && batch_reads == 0, "recorder bypass reads policy or mutates count");
	for (uint32_t dispatch = 1; dispatch <= 32; ++dispatch) {
		Check(Policy(recorded, 32, false, [&] { ++queries; return false; }) == (dispatch == 32),
		      "reset does not begin a fresh idle batch");
	}
	Check(queries == 1, "reset batch queried before its boundary");
}

void CheckQueryCadence(uint32_t batch, uint32_t idle_at, const std::vector<uint32_t>& expected_queries) {
	uint32_t recorded = 0;
	std::vector<uint32_t> queried;
	for (uint32_t dispatch = 1; dispatch <= idle_at; ++dispatch) {
		const auto submit = Policy(recorded, batch, false, [&] {
			queried.push_back(recorded);
			return recorded < idle_at;
		});
		Check(submit == (dispatch == idle_at), "busy-to-idle transition submitted at wrong dispatch");
	}
	Check(queried == expected_queries, "busy queries do not follow boundary/four-dispatch cadence");
}

void CheckDriverTimeline() {
	uint64_t known = 0;
	uint32_t queries = 0, refreshes = 0;
	const auto free = [&](uint64_t tick) { ++queries; return tick <= known; };
	const auto refresh = [&] { ++refreshes; };
	Check(!IsSubmittedWorkBusy(0, free, refresh) && queries == 1 && refreshes == 0,
	      "first unsubmitted tick causes a timeline refresh or reports busy");
	queries = refreshes = 0;
	Check(IsSubmittedWorkBusy(1, free, refresh) && queries == 2 && refreshes == 1,
	      "unretired handed work does not refresh and remain busy");
	queries = refreshes = 0;
	Check(!IsSubmittedWorkBusy(1, free, [&] { ++refreshes; known = 1; }) && queries == 2 && refreshes == 1,
	      "refreshed retirement is not observed");
	queries = refreshes = 0;
	Check(!IsSubmittedWorkBusy(1, free, refresh) && queries == 1 && refreshes == 0,
	      "already retired handed work refreshes unnecessarily");
}
}

int main() {
	CheckBoundary(32, 32, false, 1);
	CheckBoundary(32, 256, true, 56); // Forced cap does not ask at dispatch256.
	CheckQueryCadence(32, 36, {32, 36});
	CheckQueryCadence(5, 8, {5, 8}); // Four-dispatch cadence is absolute, not batch+4.
	CheckBoundary(5, 40, true, 9);
	CheckBoundary(0, 1, false, 1);
	CheckBoundary(1, 8, true, 2);
	CheckBoundary(65536, 65536, false, 1);
	CheckBoundary(std::numeric_limits<uint32_t>::max(), 65536, false, 1);
	CheckBoundary(std::numeric_limits<uint32_t>::max(), 524288, true, 114688);
	CheckDriverTimeline();
	std::puts("DispatchBatchPolicyTests: all controlled policy cases passed");
}
