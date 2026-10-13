#include "graphics/host_gpu/renderer/pipeline/precompileSchedule.h"

#include <algorithm>
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

void ComputeVariantBalance() {
	// One seed contains three independent expensive pipelines. Other seeds are
	// cheap; grouping by seed leaves two of three available workers idle.
	const std::vector<std::vector<size_t>> costs {{100, 100, 100}, {10}, {10}};
	const auto units = Libs::Graphics::PrecompileSchedule::ComputeUnits(
	    costs.size(), 0, 1, [&](size_t seed) { return costs[seed].size(); });
	std::array<size_t, 3> workers {};
	std::set<std::pair<size_t, size_t>> visited;
	for (const auto& unit: units) {
		auto worker = std::min_element(workers.begin(), workers.end());
		for (size_t i = unit.begin; i < unit.begin + unit.count; ++i) {
			*worker += costs[unit.seed][i];
			Check(visited.emplace(unit.seed, i).second, "compute variants occur exactly once");
		}
	}
	Check(visited.size() == 5, "all intended compute variants remain scheduled");
	Check(*std::max_element(workers.begin(), workers.end()) <= 110,
	      "independent costly variants can use all three bounded workers");
}

void ComputeVariantMembership() {
	const std::array<size_t, 7> counts {0, 3, 1, 0, 2, 4, 0};
	std::set<std::pair<size_t, size_t>> all;
	for (size_t shard = 0; shard < 3; ++shard) {
		const auto units = Libs::Graphics::PrecompileSchedule::ComputeUnits(
		    counts.size(), shard, 3, [&](size_t seed) {
			    Check(seed % 3 == shard, "only this original seed shard is enumerated");
			    return counts[seed];
		    });
		for (const auto& unit: units) {
			Check(unit.seed % 3 == shard, "variant units retain original seed membership");
			Check(unit.count != 0 && unit.begin + unit.count <= counts[unit.seed],
			      "compute work units stay within their selected variant range");
			for (size_t i = unit.begin; i < unit.begin + unit.count; ++i)
				Check(all.emplace(unit.seed, i).second, "shard unions have no duplicated variant");
		}
	}
	Check(all.size() == 10, "shard union includes every variant of nonempty seeds");
	Check(Libs::Graphics::PrecompileSchedule::ComputeUnits(
	          0, 0, 1, [](size_t) { return size_t {1}; }).empty(),
	      "empty compute phase has no work units");
	Check(Libs::Graphics::PrecompileSchedule::ComputeUnits(
	          5, 0, 1, [](size_t) { return size_t {0}; }).empty(),
	      "rejected and noncompute seeds have no work units");
}

void ComputeVariantLimits() {
	using Key = std::pair<size_t, size_t>;
	const std::array<size_t, 2> counts {3, 2};
	std::set<Key> retained;
	const auto run = [&](size_t shard, size_t shards, size_t workers, size_t capacity) {
		const auto units = Libs::Graphics::PrecompileSchedule::ComputeUnits(
		    counts.size(), shard, shards, [&](size_t seed) { return counts[seed]; });
		std::queue<Libs::Graphics::PrecompileSchedule::ComputeUnit> pending;
		size_t next = 0, captured = 0, completed = 0;
		bool partial = false;
		const auto start = [&] {
			while (!partial && next < units.size()) {
				const auto unit = units[next++];
				bool missing = false;
				for (size_t i = unit.begin; i < unit.begin + unit.count; ++i)
					missing |= !retained.contains({unit.seed, i});
				if (missing) {
					pending.push(unit);
					break;
				}
			}
		};
		for (size_t worker = 0; worker < workers; ++worker) start();
		while (!pending.empty()) {
			const auto unit = pending.front();
			pending.pop();
			for (size_t i = unit.begin; i < unit.begin + unit.count; ++i) {
				if (retained.contains({unit.seed, i})) continue;
				++completed;
				if (captured == capacity) {
					partial = true;
				} else {
					retained.emplace(unit.seed, i);
					++captured;
				}
			}
			start();
		}
		return std::pair {partial, completed};
	};
	const auto first = run(0, 1, 3, 1);
	Check(first.first, "variant capture overflow remains partial");
	Check(first.second == 4,
	      "in-flight variant calls finish after overflow; no new work starts afterward");
	Check(retained.size() == 1, "variant overflow never claims uncaptured keys");
	const auto lower = run(0, 2, 1, 8);
	Check(!lower.first && lower.second == 2,
	      "single worker lower retry retains each missing variant of its original seed");
	const auto upper = run(1, 2, 1, 8);
	Check(!upper.first && upper.second == 2,
	      "single worker upper retry retains every variant of the other seed");
	Check(retained.size() == 5, "both original seed halves recover the exact complete variant set");
	Check(run(0, 1, 1, 0) == std::pair {false, size_t {0}},
	      "fully retained variant work completes without new captures even at zero capacity");
}
} // namespace

int main() {
	CaptureStraggler();
	MembershipAndEmptyRanges();
	ComputeDictionaryStillSplits();
	ComputeVariantBalance();
	ComputeVariantMembership();
	ComputeVariantLimits();
	return failures != 0 ? 1 : 0;
}
