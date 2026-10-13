#include "graphics/host_gpu/renderer/pipeline/precompileSchedule.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <functional>

#include <queue>

#include <set>
#include <vector>

namespace {
unsigned failures = 0;
void     Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// Two controlled workers start jobs in index order, as WarmSeeds does. A job
// already in flight still finishes after dictionary onset; subsequent jobs stop.
// Captured keys survive in shared claims across the two smaller shard retries.
// One pipeline per job isolates the phase-boundary bug. Real seed jobs can
// contain multiple variants; this does not claim to stop all overflow work.
struct CaptureModel {
	struct Completion {
		size_t at, index;
		bool   operator>(const Completion& other) const {
            return at != other.at ? at > other.at : index > other.index;
		}
	};
	std::vector<size_t> costs;
	size_t              seeds, capacity;
	std::set<size_t>    retained;
	std::vector<size_t> invocations;
	size_t              total_worker_cost = 0;

	bool Run(bool compute_first, size_t shard = 0, size_t shards = 1) {
		size_t captured   = 0;
		bool   dictionary = false;
		Libs::Graphics::PrecompileSchedule::ForEachPipelineRange(
		    seeds, costs.size() - seeds, compute_first, [&](size_t begin, size_t count) {
			    size_t next = begin;
			    std::priority_queue<Completion, std::vector<Completion>, std::greater<>> pending;
			    const auto start_next = [&](size_t now) {
				    while (next < begin + count) {
					    const auto index = next++;
					    if (index % shards != shard || dictionary || retained.contains(index))
						    continue;
					    ++invocations[index];
					    total_worker_cost += costs[index];
					    pending.push({now + costs[index], index});
					    break;
				    }
			    };
			    start_next(0);
			    start_next(0);
			    while (!pending.empty()) {
				    const auto completed = pending.top();
				    pending.pop();
				    if (captured == capacity) {
					    dictionary = true;
				    } else {
					    retained.insert(completed.index);
					    ++captured;
				    }
				    start_next(completed.at);
			    }
		    });
		return dictionary;
	}
};

CaptureModel RetryPair(bool compute_first) {
	CaptureModel model {{100, 10, 1, 1, 1, 1, 1, 1, 1, 1}, 2, 4, {}, std::vector<size_t>(10)};
	model.Run(compute_first);
	model.Run(compute_first, 0, 2);
	model.Run(compute_first, 1, 2);
	return model;
}

void CaptureStraggler() {
	const auto baseline = RetryPair(false);
	Check(baseline.invocations[0] == 2,
	      "control reproduces expensive in-flight compute recompilation");
	Check(baseline.retained.size() == 10, "control retries eventually retain complete coverage");
	const auto ordered = RetryPair(true);
	Check(ordered.invocations[0] == 1,
	      "compute-first captures the expensive straggler before graphics dictionary onset");
	Check(ordered.total_worker_cost < baseline.total_worker_cost,
	      "compute-first lowers repeated worker cost");
	Check(ordered.retained == baseline.retained,
	      "ordered and control retries retain identical pipeline coverage");
}

void MembershipAndEmptyRanges() {
	for (const auto& sizes:
	     std::array<std::array<size_t, 2>, 4> {{{0, 0}, {0, 7}, {5, 0}, {5, 7}}}) {
		for (bool ordered: {false, true}) {
			std::vector<size_t> visits(sizes[0] + sizes[1]);
			for (size_t shard = 0; shard < 3; ++shard) {
				Libs::Graphics::PrecompileSchedule::ForEachPipelineRange(
				    sizes[0], sizes[1], ordered, [&](size_t begin, size_t count) {
					    for (size_t n = begin; n < begin + count; ++n)
						    if (n % 3 == shard) ++visits[n];
				    });
			}
			for (const auto count: visits)
				Check(count == 1, "ranges preserve every original modulo shard index exactly once");
		}
	}
}

void ComputeDictionaryStillSplits() {
	// Ordering does not suppress dictionary failures when compute alone fills it.
	CaptureModel model {{100, 10, 1, 1, 1, 1}, 6, 2, {}, std::vector<size_t>(6)};
	Check(model.Run(true), "compute-only dictionary overflow reports partial preparation");
	Check(model.retained.size() == 2, "compute-only dictionary limit is still honored");
	Check(model.retained.size() < model.costs.size(),
	      "compute-only overflow still needs real smaller-shard retries");
	Check(!model.Run(true, 0, 2), "compute-only lower retry completes within capture capacity");
	Check(!model.Run(true, 1, 2), "compute-only upper retry completes within capture capacity");
	Check(model.retained.size() == 6, "compute-only modulo retries recover every pipeline");
}
} // namespace

int main() {
	CaptureStraggler();
	MembershipAndEmptyRanges();
	ComputeDictionaryStillSplits();
	return failures != 0 ? 1 : 0;
}
