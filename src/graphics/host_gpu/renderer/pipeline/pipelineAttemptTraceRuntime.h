#pragma once

#include "graphics/host_gpu/renderer/pipeline/pipelineAttemptTrace.h"
#include "local-platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Libs::Graphics::PipelineAttemptTraceRuntime {
namespace Trace = PipelineAttemptTrace;

inline bool Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_PIPELINE_ATTEMPT_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

inline thread_local Trace::Caller      current_caller       = Trace::Caller::Unknown;
inline thread_local Trace::DriverCache current_driver_cache = Trace::DriverCache::Unknown;
class CallerScope {
public:
	CallerScope(Trace::Caller caller, Trace::DriverCache cache)
	    : caller_(current_caller), cache_(current_driver_cache) {
		if (Enabled()) {
			current_caller       = caller;
			current_driver_cache = cache;
		}
	}
	~CallerScope() {
		if (Enabled()) {
			current_caller       = caller_;
			current_driver_cache = cache_;
		}
	}
	CallerScope(const CallerScope&)            = delete;
	CallerScope& operator=(const CallerScope&) = delete;

private:
	Trace::Caller      caller_;
	Trace::DriverCache cache_;
};

template <typename Build>
Trace::Mode ModeOf(Build build) {
	switch (build) {
		case Build::Fast: return Trace::Mode::Fast;
		case Build::Optimize: return Trace::Mode::Optimize;
		default: return Trace::Mode::Full;
	}
}

template <typename Input, typename Build>
Trace::Context ComputeContext(const Input& input, Build build, bool cache_control,
                              uint32_t api_version = 0) {
	Trace::Context context;
	context.mode                 = ModeOf(build);
	context.cache_control        = cache_control;
	context.feedback_api_version = api_version;
	context.shader_count         = 1;
	context.shaders[0] = {VK_SHADER_STAGE_COMPUTE_BIT,
	                      input.stage.program != nullptr ? input.stage.program->shader_hash : 0,
	                      input.stage.program != nullptr};
	return context;
}
template <typename Vertex, typename Pixel, typename Build>
Trace::Context GraphicsContext(const Vertex& vertex, const Pixel* pixel, Build build,
                               bool cache_control, uint32_t api_version = 0) {
	Trace::Context context;
	context.mode                 = ModeOf(build);
	context.cache_control        = cache_control;
	context.feedback_api_version = api_version;
	context.shader_count         = pixel != nullptr ? 2 : 1;
	const auto* vs               = vertex.stage.program;
	context.shaders[0] = {static_cast<uint32_t>(vs != nullptr && vs->stage == ShaderType::Mesh
	                                                ? VK_SHADER_STAGE_MESH_BIT_EXT
	                                                : VK_SHADER_STAGE_VERTEX_BIT),
	                      vs != nullptr ? vs->shader_hash : 0, vs != nullptr};
	if (pixel != nullptr) {
		const auto* ps     = pixel->stage.program;
		context.shaders[1] = {VK_SHADER_STAGE_FRAGMENT_BIT, ps != nullptr ? ps->shader_hash : 0,
		                      ps != nullptr};
	}
	// Guest stage hashes only. Generated tessellation stages are not invented here.
	return context;
}

inline Trace::Sample Sample(void*) noexcept {
	Trace::Sample sample;
	sample.wall_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                           std::chrono::steady_clock::now().time_since_epoch())
	                                           .count());
	sample.thread_cpu_available = LocalPlatform::CurrentThreadCpuNanoseconds(&sample.thread_cpu_ns);
	return sample;
}

inline const char* Name(Trace::Operation operation) {
	switch (operation) {
		case Trace::Operation::DescriptorBindings: return "descriptor-bindings";
		case Trace::Operation::DescriptorLayout: return "descriptor-layout";
		case Trace::Operation::PipelineLayout: return "pipeline-layout";
		case Trace::Operation::BinaryKey: return "binary-key";
		case Trace::Operation::BinaryLookup: return "binary-lookup";
		case Trace::Operation::BinaryCreate: return "binary-create";
		case Trace::Operation::BinaryCopy: return "binary-copy";
		case Trace::Operation::BinaryCaptureProbe: return "binary-capture-probe";
		case Trace::Operation::BinaryCaptureCreate: return "binary-capture-create";
		case Trace::Operation::BinaryCaptureStore: return "binary-capture-store";
		case Trace::Operation::StaticProbe: return "static-probe";
		case Trace::Operation::OptimizedProbe: return "optimized-probe";
		case Trace::Operation::FastProbe: return "fast-probe";
		case Trace::Operation::DriverCreate: return "driver-create";
		case Trace::Operation::Bookkeeping: return "bookkeeping";
	}
	return "unknown";
}

class Session {
public:
	explicit Session(Trace::Context context) {
		if (!Enabled() || Trace::active != nullptr) return;
		static std::atomic<uint64_t> sequence {0};
		context.thread       = LocalPlatform::ThreadId();
		context.invocation   = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
		context.caller       = current_caller;
		context.driver_cache = current_driver_cache;
#ifdef KYTY_STATIC_PRECOMPILE
		if (context.caller == Trace::Caller::Unknown) {
			context.caller = Trace::Caller::Precompile;
		}
#endif
		observer_.context = context;
		observer_.user    = this;
		observer_.sample  = Sample;
		observer_.emit    = Emit;
		Trace::active     = &observer_;
		engaged_          = true;
	}
	~Session() {
		if (!engaged_) return;
		Trace::active = nullptr;
		if (dropped_ != 0) {
			std::lock_guard lock(LogMutex());
			std::printf("PIPEATTEMPT-DROPPED invocation=%llu count=%u\n",
			            static_cast<unsigned long long>(observer_.context.invocation), dropped_);
		}
	}
	Session(const Session&)            = delete;
	Session& operator=(const Session&) = delete;

private:
	static std::mutex& LogMutex() {
		static std::mutex mutex;
		return mutex;
	}
	static void Emit(void* user, const Trace::Record& record) noexcept {
		auto& session = *static_cast<Session*>(user);
		if (session.emitted_ == 64) {
			++session.dropped_;
			return;
		}
		++session.emitted_;
		const auto&     context = record.context;
		std::lock_guard lock(LogMutex());
		std::printf(
		    "PIPEATTEMPT invocation=%llu thread=%u caller=%u driver-cache=%u mode=%u "
		    "cache-control=%u "
		    "guest-stages=%u stage0=0x%x hash0=%016llx known0=%u stage1=0x%x hash1=%016llx "
		    "known1=%u "
		    "op=%s cache=%u flags=%016llx flags-known=%u result-kind=%u result=%d "
		    "wall-ns=%llu wall-known=%u thread-cpu-ns=%llu cpu-known=%u cpu-source=%s "
		    "feedback-api=%u feedback-requested=%u feedback-valid=%u feedback-flags=0x%x "
		    "driver-duration-ns=%llu app-cache-hit=%u app-cache-hit-known=%u\n",
		    static_cast<unsigned long long>(context.invocation), context.thread,
		    static_cast<unsigned>(context.caller), static_cast<unsigned>(context.driver_cache),
		    static_cast<unsigned>(context.mode), context.cache_control ? 1u : 0u,
		    context.shader_count, context.shaders[0].stage,
		    static_cast<unsigned long long>(context.shaders[0].hash),
		    context.shaders[0].available ? 1u : 0u, context.shaders[1].stage,
		    static_cast<unsigned long long>(context.shaders[1].hash),
		    context.shaders[1].available ? 1u : 0u, Name(record.operation),
		    static_cast<unsigned>(record.cache), static_cast<unsigned long long>(record.flags),
		    record.flags_available ? 1u : 0u, static_cast<unsigned>(record.result_kind),
		    record.result, static_cast<unsigned long long>(record.wall_ns),
		    record.wall_available ? 1u : 0u, static_cast<unsigned long long>(record.thread_cpu_ns),
		    record.thread_cpu_available ? 1u : 0u,
		    record.thread_cpu_available ? "os-thread-time" : "unavailable",
		    context.feedback_api_version, record.feedback_requested ? 1u : 0u,
		    record.feedback_available ? 1u : 0u, record.feedback_flags,
		    static_cast<unsigned long long>(record.driver_duration_ns),
		    record.application_cache_hit ? 1u : 0u, record.feedback_available ? 1u : 0u);
	}
	Trace::Observer observer_ {};
	bool            engaged_ = false;
	unsigned        emitted_ = 0;
	unsigned        dropped_ = 0;
};
} // namespace Libs::Graphics::PipelineAttemptTraceRuntime
