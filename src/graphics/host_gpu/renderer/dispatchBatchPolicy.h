#ifndef KYTY_GRAPHICS_RENDERER_DISPATCH_BATCH_POLICY_H_
#define KYTY_GRAPHICS_RENDERER_DISPATCH_BATCH_POLICY_H_

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// CompleteDispatch's recording boundary. Submission still owns barriers and timeline retirement.
template <typename BatchQuery, typename BusyQuery>
[[nodiscard]] bool ShouldSubmitDispatch(uint32_t& recorded, bool recorder,
                                        BatchQuery&& requested_batch, BusyQuery&& busy) {
	if (recorder) return false;
	const auto batch = std::clamp(requested_batch(), 1u, 65536u);
	if (++recorded < batch) return false;
	// Ask at the first boundary, then every fourth dispatch; bound a busy batch at eight times its
	// size.
	if (recorded < 8u * batch && ((recorded != batch && (recorded & 3u) != 0) || busy())) {
		return false;
	}
	return true;
}

inline void ResetDispatchBatch(uint32_t& recorded) {
	recorded = 0;
}

// Refresh only when work actually handed to the driver is not already retired.
template <typename IsFree, typename Refresh>
[[nodiscard]] bool IsSubmittedWorkBusy(uint64_t handed, IsFree&& is_free, Refresh&& refresh) {
	if (is_free(handed)) return false;
	refresh();
	return !is_free(handed);
}

} // namespace Libs::Graphics

#endif
