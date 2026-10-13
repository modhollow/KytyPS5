#pragma once
#include <cstddef>

namespace Libs::Graphics::PrecompileSchedule {
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
