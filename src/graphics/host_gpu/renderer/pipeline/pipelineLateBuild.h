#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// Pipelines that commands already recorded wait for. A draw or dispatch whose pipeline no
// cache holds used to compile it on the render thread before going on, so the pipelines a
// submission needs were compiled one after the other (a fog load: minutes of single-thread
// compiles). With the Vulkan recording worker the render thread only encodes commands: a missing
// pipeline is started on a worker, the bind is recorded as a wait for it, and the render thread
// goes on to the next draws, whose missing pipelines start too. The recording worker binds each one
// in command order once it is built: they compile together, nothing is skipped or reordered.
namespace Libs::Graphics::PipelineLateBuild {

// Eligibility of the caller's recorded commands for worker-built pipelines.
inline bool CanStart(bool scoped, bool recording_packets) {
	return scoped && recording_packets;
}

// One build a worker makes: its result is written once and published by `done`.
template <typename Result>
class Build {
public:
	[[nodiscard]] bool Done() const noexcept { return m_done.load(std::memory_order_acquire); }
	// Blocks until the build is published.
	const Result& Wait() const noexcept {
		m_done.wait(false, std::memory_order_acquire);
		return m_result;
	}
	void Publish(Result result) noexcept {
		m_result = std::move(result);
		m_done.store(true, std::memory_order_release);
		m_done.notify_all();
	}
	// (For a caller that waits for it on its own terms.)
	[[nodiscard]] const std::atomic<bool>& DoneFlag() const noexcept { return m_done; }

private:
	Result            m_result {};
	std::atomic<bool> m_done {false};
};

// Starts a build: on `pool` (Push(job)) when its commands can wait for it where they bind it
// (`late`), else here, before the caller goes on.
template <typename Result, typename Pool, typename Make>
std::shared_ptr<Build<Result>> Start(bool late, Pool& pool, Make make) {
	auto build = std::make_shared<Build<Result>>();
	if (late) {
		pool.Push([build, make = std::move(make)]() mutable { build->Publish(make()); });
	} else {
		build->Publish(make());
	}
	return build;
}

// Binds a build's result: `defer(build)` queues a bind that waits for it in command order when it
// is not done yet; when it is done, or nothing could be queued, `bind(result)` binds it now
// (waiting for it here).
template <typename Result, typename BindNow, typename Defer>
void Bind(const std::shared_ptr<Build<Result>>& build, BindNow bind, Defer defer) {
	if (!build->Done() && defer(build)) return;
	bind(build->Wait());
}

// The pipelines whose builds were started this way, until their owner (a pipeline cache) takes the
// finished ones, in the order they were started. A few hundred at most (a load's burst); while
// there are none, Take costs one relaxed load.
template <typename Pipeline, typename Result>
class Pending {
public:
	struct Entry {
		const void*                    owner    = nullptr;
		Pipeline*                      pipeline = nullptr;
		std::shared_ptr<Build<Result>> build;
		std::function<void(Pipeline&)> optimize; // its optimized build, once taken
	};
	void Add(Entry entry) {
		std::lock_guard lock(m_mutex);
		m_entries.push_back(std::move(entry));
		m_count.store(m_entries.size(), std::memory_order_relaxed);
		m_started++;
	}
	[[nodiscard]] std::shared_ptr<Build<Result>> Find(const Pipeline& pipeline) {
		std::lock_guard lock(m_mutex);
		for (const auto& entry: m_entries)
			if (entry.pipeline == &pipeline) return entry.build;
		return nullptr;
	}
	// `owner`'s finished entries (with `all`, every one of them) leave in start order, through
	// `take(entry)`.
	template <typename Taker>
	void Take(const void* owner, bool all, Taker take) {
		if (m_count.load(std::memory_order_relaxed) == 0) return;
		const auto taken = [&](const Entry& entry) {
			return entry.owner == owner && (all || entry.build->Done());
		};
		std::vector<Entry> finished;
		{
			std::lock_guard lock(m_mutex);
			if (std::none_of(m_entries.begin(), m_entries.end(), taken)) return;
			auto kept = m_entries.begin();
			for (auto entry = m_entries.begin(); entry != m_entries.end(); ++entry) {
				if (taken(*entry)) {
					finished.push_back(std::move(*entry));
				} else {
					if (kept != entry) *kept = std::move(*entry);
					++kept;
				}
			}
			m_entries.erase(kept, m_entries.end());
			m_count.store(m_entries.size(), std::memory_order_relaxed);
		}
		for (auto& entry: finished)
			take(entry);
	}
	[[nodiscard]] size_t   Count() const { return m_count.load(std::memory_order_relaxed); }
	[[nodiscard]] uint64_t Started() {
		std::lock_guard lock(m_mutex);
		return m_started;
	}

private:
	std::mutex          m_mutex;
	std::vector<Entry>  m_entries;
	std::atomic<size_t> m_count {0};
	uint64_t            m_started = 0;
};

} // namespace Libs::Graphics::PipelineLateBuild
