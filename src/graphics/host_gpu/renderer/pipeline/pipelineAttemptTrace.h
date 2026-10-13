#pragma once

#include <array>
#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace Libs::Graphics::PipelineAttemptTrace {

enum class Operation {
	DescriptorBindings,
	DescriptorLayout,
	PipelineLayout,
	BinaryKey,
	BinaryLookup,
	BinaryCreate,
	BinaryCopy,
	BinaryCaptureProbe,
	BinaryCaptureCreate,
	BinaryCaptureStore,
	StaticProbe,
	OptimizedProbe,
	FastProbe,
	DriverCreate,
	Bookkeeping
};
enum class Cache { None, Static, Driver };
enum class Caller {
	Unknown,
	Foreground,
	TableWorker,
	NativeWorker,
	OptimizeWorker,
	Warmup,
	Precompile,
	LoadWorker // a pipeline recorded draws or dispatches wait for (pipelineLateBuild.h)
};
enum class DriverCache { Unknown, Main, Table, Static };
enum class Mode { Full, Fast, Optimize };
enum class ResultKind { Unavailable, Vulkan, Boolean };

struct Shader {
	uint32_t stage                           = 0;
	uint64_t hash                            = 0;
	bool     available                       = false;
	bool     operator==(const Shader&) const = default;
};
struct Context {
	std::array<Shader, 2> shaders {};
	uint32_t              shader_count                     = 0;
	uint32_t              thread                           = 0;
	uint64_t              invocation                       = 0;
	Caller                caller                           = Caller::Unknown;
	DriverCache           driver_cache                     = DriverCache::Unknown;
	Mode                  mode                             = Mode::Full;
	bool                  cache_control                    = false;
	uint32_t              feedback_api_version             = 0;
	bool                  operator==(const Context&) const = default;
};
struct Sample {
	uint64_t wall_ns              = 0;
	uint64_t thread_cpu_ns        = 0;
	bool     thread_cpu_available = false;
};
struct Record {
	Context    context {};
	Operation  operation             = Operation::DriverCreate;
	Cache      cache                 = Cache::None;
	uint64_t   flags                 = 0;
	bool       flags_available       = false;
	ResultKind result_kind           = ResultKind::Unavailable;
	int32_t    result                = 0;
	uint64_t   wall_ns               = 0;
	bool       wall_available        = false;
	uint64_t   thread_cpu_ns         = 0;
	bool       thread_cpu_available  = false;
	bool       feedback_requested    = false;
	bool       feedback_available    = false;
	uint32_t   feedback_flags        = 0;
	uint64_t   driver_duration_ns    = 0;
	bool       application_cache_hit = false;
};
struct Observer {
	Context context {};
	void*   user                                = nullptr;
	Sample (*sample)(void*) noexcept            = nullptr;
	void (*emit)(void*, const Record&) noexcept = nullptr;
};
inline thread_local Observer* active = nullptr;

class ScopedObserver {
public:
	explicit ScopedObserver(Observer* observer): previous_(active), engaged_(observer != nullptr) {
		if (engaged_) active = observer;
	}
	~ScopedObserver() {
		if (engaged_) active = previous_;
	}
	ScopedObserver(const ScopedObserver&)            = delete;
	ScopedObserver& operator=(const ScopedObserver&) = delete;

private:
	Observer* previous_;
	bool      engaged_;
};

class Scope {
public:
	explicit Scope(Operation operation, Cache cache = Cache::None, uint64_t flags = 0,
	               bool flags_available = false)
	    : observer_(active) {
		if (observer_ == nullptr) return;
		if (observer_->sample == nullptr || observer_->emit == nullptr) {
			observer_ = nullptr;
			return;
		}
		record_.context         = observer_->context;
		record_.operation       = operation;
		record_.cache           = cache;
		record_.flags           = flags;
		record_.flags_available = flags_available;
		begin_                  = observer_->sample(observer_->user);
	}
	~Scope() {
		if (observer_ == nullptr) return;
		const auto end         = observer_->sample(observer_->user);
		record_.wall_available = end.wall_ns >= begin_.wall_ns;
		if (record_.wall_available) record_.wall_ns = end.wall_ns - begin_.wall_ns;
		record_.thread_cpu_available = begin_.thread_cpu_available && end.thread_cpu_available &&
		                               end.thread_cpu_ns >= begin_.thread_cpu_ns;
		if (record_.thread_cpu_available)
			record_.thread_cpu_ns = end.thread_cpu_ns - begin_.thread_cpu_ns;
		// Sink overhead is outside this interval. No observer lock spans the driver call.
		observer_->emit(observer_->user, record_);
	}
	void VulkanResult(vk::Result result) {
		record_.result_kind = ResultKind::Vulkan;
		record_.result      = static_cast<int32_t>(result);
	}
	void BooleanResult(bool result) {
		record_.result_kind = ResultKind::Boolean;
		record_.result      = result ? 1 : 0;
	}
	void CreationFeedback(vk::Result result, const vk::PipelineCreationFeedback& feedback) {
		record_.feedback_requested = true;
		if (result != vk::Result::eSuccess ||
		    !(feedback.flags & vk::PipelineCreationFeedbackFlagBits::eValid))
			return;
		record_.feedback_available = true;
		record_.feedback_flags     = static_cast<VkPipelineCreationFeedbackFlags>(feedback.flags);
		record_.driver_duration_ns = feedback.duration;
		record_.application_cache_hit = bool(
		    feedback.flags & vk::PipelineCreationFeedbackFlagBits::eApplicationPipelineCacheHit);
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Observer* observer_;
	Record    record_ {};
	Sample    begin_ {};
};

template <typename Info>
uint64_t EffectiveFlags(const Info& info) {
	// flags2 replaces the legacy flags when present. Read only.
	for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next != nullptr;
	     next       = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)
			return reinterpret_cast<const VkPipelineCreateFlags2CreateInfoKHR*>(next)->flags;
	}
	return static_cast<VkPipelineCreateFlags>(info.flags);
}

template <typename Info, typename Create>
vk::Result Invoke(Operation operation, Cache cache_class, vk::PipelineCache cache, const Info& info,
                  Create& create) {
	if (active == nullptr || active->sample == nullptr || active->emit == nullptr)
		return create(cache, info);
	Scope scope(operation, cache_class, EffectiveFlags(info), true);
	bool  feedback_exists = false;
	if (active->context.feedback_api_version >= VK_API_VERSION_1_3) {
		for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next != nullptr;
		     next       = next->pNext) {
			if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
				feedback_exists = true;
				break;
			}
		}
	}
	vk::Result result;
	if (active->context.feedback_api_version >= VK_API_VERSION_1_3 && !feedback_exists) {
		vk::PipelineCreationFeedback           feedback {};
		vk::PipelineCreationFeedbackCreateInfo request {};
		request.pNext                     = info.pNext;
		request.pPipelineCreationFeedback = &feedback;
		auto observed                     = info;
		observed.pNext                    = &request;
		result                            = create(cache, observed);
		scope.CreationFeedback(result, feedback);
	} else {
		// Do not duplicate or take over a caller-owned feedback request.
		result = create(cache, info);
	}
	scope.VulkanResult(result);
	return result;
}

} // namespace Libs::Graphics::PipelineAttemptTrace
