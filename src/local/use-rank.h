#pragma once
// The order of a hint file's entries (shader-hints.cpp --make-hints): by use in recordings.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ShaderHints {

// How many recordings use each entry, and how early one of them first did (position / length): what
// more of them use, then what was used earlier, comes first; the rest keeps the order it was added
// in.
class UseRank {
public:
	static constexpr uint32_t None = 0xFFFFFFFFu;

	// Entry `id` (None: not an entry) at `position` of the `length` entries of recording `source`.
	void Use(uint32_t id, size_t source, size_t position, size_t length) {
		if (id == None) return;
		auto& count = m_counts[id];
		if (count.last_source != source) {
			count.recordings++;
			count.last_source = source;
		}
		count.first = std::min(count.first, static_cast<double>(position) /
		                                        static_cast<double>(std::max<size_t>(length, 1)));
	}

	// The ids 0 to count - 1, ranked.
	[[nodiscard]] std::vector<uint32_t> Order(size_t count) const {
		std::vector<uint32_t> order(count);
		for (uint32_t i = 0; i < count; i++)
			order[i] = i;
		const auto of = [&](uint32_t id) {
			const auto found = m_counts.find(id);
			return found == m_counts.end() ? Count {} : found->second;
		};
		std::stable_sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
			const auto a = of(left), b = of(right);
			if (a.recordings != b.recordings) return a.recordings > b.recordings;
			return a.first < b.first;
		});
		return order;
	}

private:
	struct Count {
		uint32_t recordings  = 0;
		double   first       = 1.0;
		size_t   last_source = static_cast<size_t>(-1);
	};
	std::unordered_map<uint32_t, Count> m_counts;
};

} // namespace ShaderHints
