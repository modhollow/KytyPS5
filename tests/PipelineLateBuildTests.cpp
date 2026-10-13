// Pipelines one submission needs (pipelineLateBuild.h). Built where they are needed, as the render
// thread did, each waits for the one before: wall time is the sum of their compiles. Started on the
// workers and waited for by the in-order replay of the recorded binds, they compile together: wall
// time is the slowest one when there are workers enough. Either way every bind gets the pipeline
// built for it, in recording order.
//
// The model gives every thread a clock (milliseconds): a build advances the clock of the thread it
// runs on, a pool runs its jobs first in, first out on whichever of its workers frees up first, and
// the recording worker binds in order, each bind no earlier than its pipeline was built. A second
// case runs the same policy on real threads.
#include "graphics/host_gpu/renderer/pipeline/pipelineLateBuild.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace {

using namespace Libs::Graphics;

int g_failures = 0;

void Check(bool ok, const char* what, double value = 0.0, double expected = 0.0) {
	if (!ok) {
		std::printf("FAIL: %s (%.1f, expected %.1f)\n", what, value, expected);
		g_failures++;
	}
}

// A built pipeline: which build made it and when it was ready (model milliseconds).
struct Handle {
	uint32_t id    = UINT32_MAX;
	double   ready = 0.0;
};
using Build = PipelineLateBuild::Build<Handle>;

struct Clock {
	double now = 0.0;
};
// The clock of the thread a modelled build runs on.
Clock* g_running = nullptr;

// `count` workers: each job starts when it was pushed or when a worker frees up, whichever is later.
class ModelPool {
public:
	ModelPool(size_t count, const Clock& caller): m_free(count, 0.0), m_caller(caller) {}
	void Push(std::function<void()> job) { m_queue.push_back({std::move(job), m_caller.now}); }
	// Runs what was pushed (in model time the jobs ran from when they were pushed).
	void RunAll() {
		for (auto& [job, pushed]: m_queue) {
			auto  worker = std::min_element(m_free.begin(), m_free.end());
			Clock clock {std::max(*worker, pushed)};
			auto* outer = g_running;
			g_running   = &clock;
			job();
			g_running = outer;
			*worker   = clock.now;
		}
		m_queue.clear();
	}

private:
	struct Queued {
		std::function<void()> job;
		double                pushed = 0.0;
	};
	std::vector<double> m_free;
	std::deque<Queued>  m_queue;
	const Clock&        m_caller;
};

// The recording worker: binds in recording order, each no earlier than its pipeline was built.
class ModelRecorder {
public:
	explicit ModelRecorder(ModelPool& pool): m_pool(pool) {}
	void Bound(const Handle& handle, double recorded) {
		m_ops.push_back({nullptr, handle, recorded});
	}
	bool Defer(const std::shared_ptr<Build>& build, double recorded) {
		m_ops.push_back({build, {}, recorded});
		return true;
	}
	void Replay() {
		m_pool.RunAll();
		for (const auto& op: m_ops) {
			const auto& handle = op.build != nullptr ? op.build->Wait() : op.handle;
			m_clock.now        = std::max({m_clock.now, op.recorded, handle.ready});
			m_bound.push_back(handle.id);
		}
	}
	[[nodiscard]] double                       Now() const { return m_clock.now; }
	[[nodiscard]] const std::vector<uint32_t>& BoundIds() const { return m_bound; }

private:
	struct Op {
		std::shared_ptr<Build> build;
		Handle                 handle;
		double                 recorded = 0.0;
	};
	ModelPool&            m_pool;
	Clock                 m_clock;
	std::vector<Op>       m_ops;
	std::vector<uint32_t> m_bound;
};

struct Outcome {
	double                wall = 0.0;
	std::vector<uint32_t> bound;
};

// One submission: each draw takes `encode` ms of render thread work and needs a new pipeline of
// `costs[i]` ms. `late`: started on `workers` workers, bound by the recording worker.
Outcome Submit(const std::vector<double>& costs, bool late, size_t workers, double encode = 1.0) {
	Clock         render;
	ModelPool     pool(workers, render);
	ModelRecorder recorder(pool);
	g_running = &render;
	for (uint32_t i = 0; i < costs.size(); i++) {
		render.now += encode;
		auto build = PipelineLateBuild::Start<Handle>(late, pool, [i, cost = costs[i]] {
			g_running->now += cost;
			return Handle {i, g_running->now};
		});
		PipelineLateBuild::Bind(
		    build, [&](const Handle& handle) { recorder.Bound(handle, render.now); },
		    [&](const std::shared_ptr<Build>& pending) { return recorder.Defer(pending, render.now); });
	}
	g_running = nullptr;
	recorder.Replay();
	return {std::max(render.now, recorder.Now()), recorder.BoundIds()};
}

bool InOrder(const std::vector<uint32_t>& bound, size_t count) {
	std::vector<uint32_t> expected(count);
	std::iota(expected.begin(), expected.end(), 0u);
	return bound == expected;
}

void ModelledSubmission() {
	// A fog load's kind of mix: compute 0.41 s and graphics 0.20 s average, one of 2.94 s.
	const std::vector<double> costs {410, 200, 2940, 410, 200, 410, 200, 410};
	const double              sum     = std::accumulate(costs.begin(), costs.end(), 0.0);
	const double              slowest = *std::max_element(costs.begin(), costs.end());
	const double              encode  = 1.0 * static_cast<double>(costs.size());

	const auto serial = Submit(costs, false, 8);
	Check(serial.wall >= sum, "built where needed: wall is the sum of the builds", serial.wall, sum);
	Check(InOrder(serial.bound, costs.size()), "built where needed: every bind its own pipeline, in order");

	const auto parallel = Submit(costs, true, 8);
	Check(parallel.wall <= slowest + encode,
	      "started on 8 workers and joined in order: wall is the slowest build", parallel.wall,
	      slowest + encode);
	Check(InOrder(parallel.bound, costs.size()),
	      "started on workers: every bind its own pipeline, in recording order");

	// Fewer workers than pipelines: still no more than the serial sum, and no less than the work
	// spread over the workers.
	const auto four = Submit(costs, true, 4);
	Check(four.wall < serial.wall, "started on 4 workers: shorter than built where needed", four.wall,
	      serial.wall);
	Check(four.wall >= std::max(slowest, sum / 4.0), "started on 4 workers: no less than the work spread",
	      four.wall, std::max(slowest, sum / 4.0));
	Check(InOrder(four.bound, costs.size()), "started on 4 workers: binds in order");
}

// The accepted policy needs no environment setting. A recorded submission's default must start
// its missing builds together; callers that cannot record still receive completed results here.
void DefaultEligibility() {
	const std::vector<double> costs {100.0, 80.0, 100.0};
	const auto recorded = Submit(costs, PipelineLateBuild::CanStart(true, true), 8, 0.0);
	Check(recorded.wall == 100.0, "default recorded submission: worker builds overlap", recorded.wall, 100.0);
	Check(InOrder(recorded.bound, costs.size()), "default recorded submission: each bind its own result in order");
	for (const auto [scoped, packets]: {std::pair {false, true}, std::pair {true, false}, std::pair {false, false}}) {
		const auto direct = Submit(costs, PipelineLateBuild::CanStart(scoped, packets), 8, 0.0);
		Check(direct.wall == 280.0, "unscoped or no recording packets: synchronous completed builds", direct.wall, 280.0);
		Check(InOrder(direct.bound, costs.size()), "synchronous fallback: each bind its own result in order");
	}
}

// Nothing can be queued (no recording worker): bound at once, after waiting for the build.
void NoRecorder() {
	Clock     render;
	ModelPool pool(4, render);
	g_running = &render;
	std::vector<uint32_t> bound;
	for (uint32_t i = 0; i < 3; i++) {
		auto build = PipelineLateBuild::Start<Handle>(true, pool, [i] {
			g_running->now += 100.0;
			return Handle {i, g_running->now};
		});
		PipelineLateBuild::Bind(
		    build,
		    [&](const Handle& handle) {
			    render.now = std::max(render.now, handle.ready);
			    bound.push_back(handle.id);
		    },
		    [&](const std::shared_ptr<Build>&) {
			    pool.RunAll(); // (the real workers go on while the caller waits)
			    return false;
		    });
	}
	g_running = nullptr;
	Check(InOrder(bound, 3), "no recording worker: bound at once, in order");
	Check(render.now >= 300.0, "no recording worker: the caller waits for each build", render.now, 300.0);
}

// The same policy on real threads: the builds of one submission are in flight together, and the
// binds replayed by another thread get their own pipelines in order.
class ThreadPool {
public:
	explicit ThreadPool(size_t count) {
		for (size_t i = 0; i < count; i++)
			m_threads.emplace_back([this] { Run(); });
	}
	~ThreadPool() {
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_wake.notify_all();
		for (auto& thread: m_threads)
			thread.join();
	}
	ThreadPool(const ThreadPool&)            = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
	void        Push(std::function<void()> job) {
        {
            std::lock_guard lock(m_mutex);
            m_jobs.push_back(std::move(job));
        }
        m_wake.notify_one();
	}

private:
	void Run() {
		for (;;) {
			std::function<void()> job;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
				if (m_jobs.empty()) return;
				job = std::move(m_jobs.front());
				m_jobs.pop_front();
			}
			job();
		}
	}
	std::mutex                        m_mutex;
	std::condition_variable           m_wake;
	std::deque<std::function<void()>> m_jobs;
	bool                              m_stop = false;
	std::vector<std::thread>          m_threads;
};

void RealThreads(bool late) {
	constexpr uint32_t      Count = 6;
	std::mutex              mutex;
	std::condition_variable changed;
	uint32_t                in_flight = 0, peak = 0;
	const auto              submitter = std::this_thread::get_id();
	// A build on a worker holds it until all six are in flight (10 s at most): the peak shows how
	// many compile together. One built where it is needed cannot wait for the others (they start
	// after it).
	const auto make = [&](uint32_t id) {
		return [&, id] {
			std::unique_lock lock(mutex);
			peak = std::max(peak, ++in_flight);
			changed.notify_all();
			if (std::this_thread::get_id() != submitter)
				changed.wait_for(lock, std::chrono::seconds(10), [&] { return peak >= Count; });
			in_flight--;
			return Handle {id, 0.0};
		};
	};

	std::vector<uint32_t>              bound;
	std::vector<std::shared_ptr<Build>> deferred;
	{
		ThreadPool pool(Count);
		for (uint32_t i = 0; i < Count; i++) {
			auto build = PipelineLateBuild::Start<Handle>(late, pool, make(i));
			PipelineLateBuild::Bind(
			    build, [&](const Handle& handle) { bound.push_back(handle.id); },
			    [&](const std::shared_ptr<Build>& pending) {
				    deferred.push_back(pending);
				    bound.push_back(UINT32_MAX); // (filled by the replay)
				    return true;
			    });
		}
		// The recording worker: in order, waiting for each build.
		std::thread replay([&] {
			size_t next = 0;
			for (auto& id: bound) {
				if (id == UINT32_MAX) id = deferred[next++]->Wait().id;
			}
		});
		replay.join();
	}
	if (late) {
		Check(peak == Count, "real threads: the submission's builds are in flight together", peak,
		      Count);
	}
	Check(InOrder(bound, Count), late ? "real threads: binds in order" : "real threads, built where needed: binds in order");
}

// The pipelines whose builds are pending until their cache takes the finished ones.
struct FakePipeline {
	uint32_t handle = 0;
};
using Pending = PipelineLateBuild::Pending<FakePipeline, Handle>;

void PendingEntries() {
	Pending                                pending;
	std::array<FakePipeline, 4>            pipelines {};
	std::array<std::shared_ptr<Build>, 4> builds;
	const int                              cache = 0, other_cache = 0;
	for (uint32_t i = 0; i < 4; i++) {
		builds[i] = std::make_shared<Build>();
		pending.Add({i == 3 ? &other_cache : &cache, &pipelines[i], builds[i], nullptr});
	}
	Check(pending.Started() == 4 && pending.Count() == 4, "pending: four started");
	Check(pending.Find(pipelines[2]) == builds[2], "pending: a pipeline's build is found");

	std::vector<uint32_t> taken;
	const auto            take = [&](Pending::Entry& entry) {
        entry.pipeline->handle = entry.build->Wait().id;
        taken.push_back(entry.build->Wait().id);
	};
	pending.Take(&cache, false, take);
	Check(taken.empty() && pending.Count() == 4, "pending: nothing taken before a build is done");

	builds[2]->Publish({2, 0.0});
	builds[3]->Publish({3, 0.0});
	builds[0]->Publish({0, 0.0});
	pending.Take(&cache, false, take);
	Check(taken == std::vector<uint32_t> {0, 2}, "pending: the cache's finished builds, in start order");
	Check(pipelines[0].handle == 0 && pipelines[2].handle == 2, "pending: their pipelines got them");
	Check(pending.Count() == 2 && pending.Find(pipelines[2]) == nullptr &&
	          pending.Find(pipelines[1]) == builds[1] && pending.Find(pipelines[3]) == builds[3],
	      "pending: the unfinished one and the other cache's stay");

	taken.clear();
	builds[1]->Publish({1, 0.0});
	pending.Take(&cache, true, take);
	Check(taken == std::vector<uint32_t> {1}, "pending: all of a cache's builds at its end");
	Check(pending.Count() == 1 && pending.Find(pipelines[3]) == builds[3],
	      "pending: another cache's build is not its to take");
}

// Owner teardown can encounter both an in-flight build and queued builds. Removing them from the
// pending registry must retain their publication objects and wait for every result, in order.
void QueuedOwnerTeardown() {
	using namespace std::chrono_literals;
	Pending pending;
	std::array<FakePipeline, 3> pipelines {};
	const int owner = 0;
	std::mutex gate_mutex;
	std::condition_variable gate_changed;
	bool first_started = false, release = false;
	std::weak_ptr<Build> first_build;
	std::atomic<bool> taking {false}, finished {false};
	std::vector<uint32_t> taken;
	ThreadPool pool(1);
	for (uint32_t i = 0; i < pipelines.size(); ++i) {
		auto build = PipelineLateBuild::Start<Handle>(true, pool, [&, i] {
			if (i == 0) {
				std::unique_lock lock(gate_mutex);
				first_started = true;
				gate_changed.notify_all();
				gate_changed.wait_for(lock, 10s, [&] { return release; });
			}
			return Handle {i, 0.0};
		});
		if (i == 0) first_build = build;
		pending.Add({&owner, &pipelines[i], std::move(build), nullptr});
	}
	{
		std::unique_lock lock(gate_mutex);
		Check(gate_changed.wait_for(lock, 10s, [&] { return first_started; }), "owner teardown: first build in flight");
	}
	std::thread teardown([&] {
		taking = true;
		pending.Take(&owner, true, [&](Pending::Entry& entry) {
			const auto& result = entry.build->Wait();
			entry.pipeline->handle = result.id;
			taken.push_back(result.id);
		});
		finished = true;
	});
	for (int i = 0; i < 2000 && !taking; ++i) std::this_thread::sleep_for(1ms);
	std::this_thread::sleep_for(50ms);
	Check(!finished, "owner teardown: unfinished builds prevent return");
	Check(!first_build.expired(), "owner teardown: publication object retained while waiting");
	{
		std::lock_guard lock(gate_mutex);
		release = true;
	}
	gate_changed.notify_all();
	teardown.join();
	Check(taken == std::vector<uint32_t> {0, 1, 2}, "owner teardown: queued results taken in start order");
	Check(pending.Count() == 0 && pipelines[1].handle == 1 && pipelines[2].handle == 2, "owner teardown: every pipeline published and removed");
}

} // namespace

int main() {
	ModelledSubmission();
	DefaultEligibility();
	NoRecorder();
	RealThreads(false);
	RealThreads(true);
	PendingEntries();
	QueuedOwnerTeardown();
	if (g_failures != 0) {
		std::printf("PipelineLateBuildTests: %d failures\n", g_failures);
		return 1;
	}
	std::printf("PipelineLateBuildTests: all passed\n");
	return 0;
}
