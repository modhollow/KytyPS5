#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace Libs::Graphics::PrecompileSchedule {
struct ComputeUnit {
	size_t seed, begin, count;
};

// Whether item `index` of `count` belongs to shard `shard` of `shards`: every n-th item, or with
// `blocks` the shard-th contiguous slice, so that a ranked list's first shards hold its first items
// (a precompile stopped early has them). Either way shard i of n is exactly shards i and i + n of
// 2n (every n-th), or 2i and 2i + 1 of 2n (slices): a shard that left pipelines out runs again as
// two.
inline bool InShard(size_t index, size_t count, size_t shard, size_t shards, bool blocks) {
	if (!blocks) return index % shards == shard;
	return count != 0 && index * shards / count == shard;
}

// Original seed indices still determine shard membership. The executor joins
// this bounded queue before graphics capture starts.
template <class VariantCount>
std::vector<ComputeUnit> ComputeUnits(size_t seeds, size_t shard, size_t shards,
                                      VariantCount variants, bool blocks = false) {
	std::vector<ComputeUnit> units;
	for (size_t seed = 0; seed < seeds; ++seed) {
		if (!InShard(seed, seeds, shard, shards, blocks)) continue;
		const auto count = variants(seed);
		for (size_t variant = 0; variant < count; ++variant)
			units.push_back({seed, variant, 1});
	}
	return units;
}

template <class ModuleSize>
void PrioritizeComputeUnits(std::vector<ComputeUnit>& units, bool parallel, ModuleSize words) {
	if (!parallel) return;
	// Module size is an inexpensive estimate, not a driver timing guarantee.
	// Offer large work before cheap jobs leave a costly tail on one worker.
	std::stable_sort(units.begin(), units.end(), [&](const auto& left, const auto& right) {
		return words(left) > words(right);
	});
}

// The executor joins all workers before returning. Indices remain the original
// combined seed/recipe indices used for shard membership.
// Finish compute captures before graphics can train a driver compression dictionary;
// non-binary cache paths keep the combined queue.
template <class Execute>
void ForEachPipelineRange(size_t seeds, size_t recipes, bool compute_first, Execute execute) {
	if (compute_first) {
		execute(0, seeds);
		execute(seeds, recipes);
	} else {
		execute(0, seeds + recipes);
	}
}
} // namespace Libs::Graphics::PrecompileSchedule
