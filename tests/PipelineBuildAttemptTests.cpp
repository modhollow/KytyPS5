#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "graphics/host_gpu/renderer/pipeline/pipelineAttemptTrace.h"

namespace {
unsigned failures = 0;
void     Check(bool condition, const std::string& message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
}
template <typename Handle>
Handle HandleValue(uintptr_t value) {
	using Raw = typename Handle::CType;
	if constexpr (std::is_pointer_v<Raw>)
		return Handle(reinterpret_cast<Raw>(value));
	else
		return Handle(static_cast<Raw>(value));
}
template <typename Handle>
uint64_t HandleNumber(Handle value) {
	using Raw = typename Handle::CType;
	if constexpr (std::is_pointer_v<Raw>)
		return reinterpret_cast<uintptr_t>(static_cast<Raw>(value));
	else
		return static_cast<uint64_t>(static_cast<Raw>(value));
}
const VkBaseInStructure* Find(const void* next, VkStructureType type) {
	for (auto* item = static_cast<const VkBaseInStructure*>(next); item != nullptr;
	     item       = item->pNext) {
		if (item->sType == type) return item;
	}
	return nullptr;
}
} // namespace

namespace Libs::Graphics {
namespace PipelineBinaries {
struct Key {
	std::vector<uint64_t> state;
	bool                  operator<(const Key& other) const { return state < other.state; }
	bool                  operator==(const Key&) const = default;
};
struct Handles {
	std::vector<vk::PipelineBinaryKHR> binaries;
};
template <typename Device>
bool PipelineKey(Device& device, const void* info, Key& key) {
	return device.KeyOf(info, key);
}
} // namespace PipelineBinaries
namespace PipelineCache {
struct Pipeline {
	vk::Pipeline pipeline {};
	bool         unoptimized = false;
};
} // namespace PipelineCache
enum class PipelineBuild { Full, Fast, Optimize };
struct TestStore {
	std::set<PipelineBinaries::Key> entries;
	mutable unsigned                loads = 0;
	PipelineBinaries::Handles       Load(const PipelineBinaries::Key& key) const {
		++loads;
		if (entries.contains(key)) return {{HandleValue<vk::PipelineBinaryKHR>(77)}};
		return {};
	}
};
struct TestWriter {
	bool     claimed  = false;
	unsigned captures = 0, copies = 0;
	bool     Claimed(const PipelineBinaries::Key&) const { return claimed; }
	void     Copy(const PipelineBinaries::Key&, const TestStore&) { ++copies; }
	bool     Capture(const PipelineBinaries::Key&, vk::Pipeline, const void* = nullptr) {
		++captures;
		return true;
	}
};
struct TestDevice {
	std::function<PipelineBinaries::Key(const void*)> state_of;
	bool                                              key_success = true;
	unsigned                                          queries = 0, destroys = 0;
	PipelineBinaries::Key                             last_key;
	bool KeyOf(const void* info, PipelineBinaries::Key& key) {
		++queries;
		if (!key_success) return false;
		key      = state_of(info);
		last_key = key;
		return true;
	}
	void destroyPipeline(vk::Pipeline, std::nullptr_t) { ++destroys; }
};
struct GraphicContext {
	TestDevice        device;
	TestStore*        pipeline_binaries      = nullptr;
	TestWriter*       pipeline_binary_writer = nullptr;
	vk::PipelineCache static_pipeline_cache {};
	bool              pipeline_cache_control_enabled = true;
};
bool PipelineKeyLog() {
	return false;
}
template <typename Info>
void LogPipelineKey(const char*, const PipelineBinaries::Key&, const Info&) {}

// Actual production owners, with only external context/store/device adapters.
// No Vulkan command or driver is invoked by this CPU test.
#include "graphics/host_gpu/renderer/pipeline/pipelineBinaryCapture.inc"
#include "graphics/host_gpu/renderer/pipeline/pipelineBuildAttempt.inc"
} // namespace Libs::Graphics

namespace {
using namespace Libs::Graphics;

void AddStage(PipelineBinaries::Key& key, const vk::PipelineShaderStageCreateInfo& stage) {
	key.state.push_back(static_cast<uint32_t>(stage.stage));
	key.state.push_back(HandleNumber(stage.module));
	if (stage.pName != nullptr) {
		for (const char* p = stage.pName; *p != 0; ++p)
			key.state.push_back(static_cast<uint8_t>(*p));
	}
	key.state.push_back(0);
	const auto* spec = stage.pSpecializationInfo;
	key.state.push_back(spec != nullptr ? spec->mapEntryCount : 0);
	if (spec != nullptr) {
		for (uint32_t n = 0; n < spec->mapEntryCount; ++n) {
			const auto& entry = spec->pMapEntries[n];
			key.state.insert(key.state.end(), {entry.constantID, entry.offset, entry.size});
		}
		key.state.push_back(spec->dataSize);
		const auto* bytes = static_cast<const uint8_t*>(spec->pData);
		for (size_t n = 0; n < spec->dataSize; ++n)
			key.state.push_back(bytes[n]);
	}
}

// Independent explicit application-cache model: successful ordinary creation
// records exact fixture state only with a non-null cache. A compile-forbidden
// request can use only such a recorded entry. This model does not copy the
// production policy and does not promise identical real-driver hit behavior.
template <typename Info>
PipelineBinaries::Key StateOf(const Info& info) {
	PipelineBinaries::Key key;
	const auto            flags = static_cast<VkPipelineCreateFlags>(info.flags) &
	                              ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
	key.state                   = {std::is_same_v<Info, vk::ComputePipelineCreateInfo> ? 1u : 2u,
	                               HandleNumber(info.layout), flags};
	if constexpr (std::is_same_v<Info, vk::ComputePipelineCreateInfo>) {
		AddStage(key, info.stage);
	} else {
		key.state.push_back(info.stageCount);
		for (uint32_t n = 0; n < info.stageCount; ++n)
			AddStage(key, info.pStages[n]);
		if (const auto* base = Find(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)) {
			const auto& rendering = *reinterpret_cast<const VkPipelineRenderingCreateInfo*>(base);
			key.state.insert(key.state.end(),
			                 {rendering.viewMask, rendering.colorAttachmentCount,
			                  static_cast<uint32_t>(rendering.depthAttachmentFormat),
			                  static_cast<uint32_t>(rendering.stencilAttachmentFormat)});
			for (uint32_t n = 0; n < rendering.colorAttachmentCount; ++n)
				key.state.push_back(rendering.pColorAttachmentFormats[n]);
		}
	}
	return key;
}

struct ApplicationBackend {
	struct Request {
		uint64_t                cache;
		vk::PipelineCreateFlags flags;
		PipelineBinaries::Key   state;
		vk::Result              result = vk::Result::eErrorUnknown;
		uint64_t effective_flags = 0, output_handle = 0;
		std::vector<uint64_t> chain_types;
		bool operator==(const Request&) const = default;
	};
	std::map<uint64_t, std::set<PipelineBinaries::Key>> caches;
	unsigned              compiled_outputs = 0, ordinary_calls = 0, probes = 0, binary_calls = 0,
	                      capture_calls  = 0;
	bool                  binary_success = true, creation_error = false;
	uint64_t              next_pipeline = 100;
	std::vector<uint64_t> cache_arguments;
	std::vector<Request>  requests;
	std::function<void(bool, bool)> advance;
	template <typename Info>
	vk::Result Create(PipelineCache::Pipeline& pipeline, vk::PipelineCache cache,
	                  const Info& info) {
		cache_arguments.push_back(HandleNumber(cache));
		const auto key      = StateOf(info);
		auto&      observed = requests.emplace_back(Request {HandleNumber(cache), info.flags, key});
		observed.effective_flags = PipelineAttemptTrace::EffectiveFlags(info);
		for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next != nullptr; next = next->pNext) {
			observed.chain_types.push_back(static_cast<uint32_t>(next->sType));
			if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)
				observed.chain_types.push_back(reinterpret_cast<const VkPipelineCreateFlags2CreateInfoKHR*>(next)->flags);
			if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR) {
				const auto* binary = reinterpret_cast<const VkPipelineBinaryInfoKHR*>(next);
				observed.chain_types.push_back(binary->binaryCount);
				for (uint32_t n = 0; n < binary->binaryCount; ++n)
					observed.chain_types.push_back(HandleNumber(vk::PipelineBinaryKHR(binary->pPipelineBinaries[n])));
			}
		}
		if (advance) advance(bool(info.flags & vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired),
		                     bool(info.flags & vk::PipelineCreateFlagBits::eDisableOptimization));
		const auto completed = [&](vk::Result result) {
			observed.result = result;
			observed.output_handle = HandleNumber(pipeline.pipeline);
			return result;
		};
		if (const auto* base = Find(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR)) {
			const auto* binaries = reinterpret_cast<const VkPipelineBinaryInfoKHR*>(base);
			if (binaries->binaryCount != 0) {
				++binary_calls;
				Check(cache == nullptr,
				      "binary recreation must keep a null application cache (VUID09616)");
				if (!binary_success) return completed(vk::Result::eErrorUnknown);
				pipeline.pipeline = HandleValue<vk::Pipeline>(next_pipeline++);
				return completed(vk::Result::eSuccess);
			}
		}
		if (const auto* base =
		        Find(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)) {
			const auto* flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfoKHR*>(base);
			if ((flags->flags & VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR) != 0) {
				++capture_calls;
				Check(cache == nullptr,
				      "binary capture must keep a null application cache (VUID09617)");
			}
		}
		const auto number   = HandleNumber(cache);
		const bool retained = cache != nullptr && caches[number].contains(key);
		if (bool(info.flags & vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired)) {
			++probes;
			if (!retained) return completed(vk::Result::ePipelineCompileRequired);
		} else {
			++ordinary_calls;
			if (creation_error) return completed(vk::Result::eErrorDeviceLost);
			if (!retained) ++compiled_outputs;
			if (cache != nullptr) caches[number].insert(key);
		}
		pipeline.pipeline = HandleValue<vk::Pipeline>(next_pipeline++);
		return completed(vk::Result::eSuccess);
	}
};

template <typename Info>
struct Harness {
	GraphicContext             graphics;
	ApplicationBackend         backend;
	vk::PipelineCache          driver_cache   = HandleValue<vk::PipelineCache>(1);
	vk::PipelineCache          static_cache   = HandleValue<vk::PipelineCache>(2);
	uint32_t                   specialization = 7;
	vk::SpecializationMapEntry map {9, 0, sizeof(specialization)};
	vk::SpecializationInfo     spec {1, &map, sizeof(specialization), &specialization};
	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	std::array<vk::Format, 1>                        formats {vk::Format::eR8G8B8A8Unorm};
	vk::PipelineRenderingCreateInfo                  rendering {};
	Info                                             info {};
	Harness() {
		stages[0].module              = HandleValue<vk::ShaderModule>(11);
		stages[0].pName               = "main";
		stages[0].pSpecializationInfo = &spec;
		stages[0].stage               = vk::ShaderStageFlagBits::eCompute;
		info.layout                   = HandleValue<vk::PipelineLayout>(12);
		info.flags                    = vk::PipelineCreateFlagBits::eAllowDerivatives;
		if constexpr (std::is_same_v<Info, vk::ComputePipelineCreateInfo>)
			info.stage = stages[0];
		else {
			stages[0].stage                   = vk::ShaderStageFlagBits::eVertex;
			stages[1]                         = stages[0];
			stages[1].stage                   = vk::ShaderStageFlagBits::eFragment;
			stages[1].module                  = HandleValue<vk::ShaderModule>(13);
			info.stageCount                   = stages.size();
			info.pStages                      = stages.data();
			rendering.colorAttachmentCount    = formats.size();
			rendering.pColorAttachmentFormats = formats.data();
			info.pNext                        = &rendering;
		}
		graphics.device.state_of = [](const void* pointer) {
			return StateOf(*static_cast<const Info*>(pointer));
		};
	}
	std::pair<vk::Result, PipelineCache::Pipeline> Attempt(PipelineBuild mode) {
		PipelineCache::Pipeline pipeline;
		const auto              flags = info.flags;
		const auto*             next  = info.pNext;
		const auto bytes = [](const auto& value) {
			std::array<unsigned char, sizeof(value)> out {};
			std::memcpy(out.data(), &value, sizeof(value));
			return out;
		};
		const auto before_info = bytes(info);
		const auto before_stages = bytes(stages);
		const auto before_rendering = bytes(rendering);
		const auto before_spec = bytes(spec);
		const auto              result =
		    CreatePipelineHandle(graphics, pipeline, info, driver_cache, mode,
		                         [&](vk::PipelineCache cache, const Info& requested) {
			                         return backend.Create(pipeline, cache, requested);
		                         });
		Check(info.flags == flags && info.pNext == next,
		      "caller flags and pNext must remain unchanged");
		Check(bytes(info) == before_info && bytes(stages) == before_stages &&
		          bytes(rendering) == before_rendering && bytes(spec) == before_spec,
		      "caller input objects and pointer bytes remain unchanged");
		return {result, pipeline};
	}
};

template <typename Info>
void Exercise(const char* stage) {
	const std::string prefix = std::string(stage) + ": ";
	{
		Harness<Info> h;
		const auto    first = h.Attempt(PipelineBuild::Fast);
		Check(first.first == vk::Result::eSuccess && first.second.pipeline &&
		          first.second.unoptimized,
		      prefix + "first Fast output is valid and promotion-eligible");
		Check(h.backend.caches[HandleNumber(h.driver_cache)].size() == 1,
		      prefix + "first exact Fast output participates in the retained application cache");
		const auto compiled      = h.backend.compiled_outputs;
		const auto ordinary      = h.backend.ordinary_calls;
		const auto request_begin = h.backend.requests.size();
		const auto second = h.Attempt(PipelineBuild::Fast); // fresh runtime object; cache retained
		Check(second.first == vk::Result::eSuccess && second.second.pipeline &&
		          second.second.unoptimized,
		      prefix + "retained Fast output remains promotion-eligible");
		Check(h.backend.compiled_outputs == compiled,
		      prefix + "repeat exact Fast state reuses retained output before optimization");
		Check(h.backend.ordinary_calls == ordinary,
		      prefix + "repeat exact Fast state makes no ordinary-create call");
		auto fast = h.info;
		fast.flags |= vk::PipelineCreateFlagBits::eDisableOptimization;
		const auto fast_state = StateOf(fast);
		const auto lookup_flags =
		    fast.flags | vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
		bool exact_successful_probe = false;
		for (size_t n = request_begin; n < h.backend.requests.size(); ++n) {
			const auto& request = h.backend.requests[n];
			if (request.cache == HandleNumber(h.driver_cache) && request.flags == lookup_flags &&
			    request.state == fast_state && request.result == vk::Result::eSuccess)
				exact_successful_probe = true;
		}
		Check(exact_successful_probe,
		      prefix + "repeat succeeds through exact compile-forbidden Fast cache lookup");
		// Distinct specialization/layout/mode cannot be mistaken for the retained state.
		for (int variation = 0; variation < 4; ++variation) {
			const auto before = h.backend.compiled_outputs;
			if (variation == 0) ++h.specialization;
			if (variation == 1) h.info.layout = HandleValue<vk::PipelineLayout>(22);
			if (variation == 2) h.info.flags = {};
			if (variation == 3) {
				if constexpr (std::is_same_v<Info, vk::ComputePipelineCreateInfo>)
					h.info.stage.module = HandleValue<vk::ShaderModule>(24);
				else
					h.formats[0] = vk::Format::eB8G8R8A8Unorm;
			}
			const auto different = h.Attempt(PipelineBuild::Fast);
			Check(different.first == vk::Result::eSuccess &&
			          h.backend.compiled_outputs == before + 1,
			      prefix + "different complete fixture state requires its own output");
		}
	}
	{
		Harness<Info> h;
		h.backend.caches[HandleNumber(h.driver_cache)].insert(StateOf(h.info));
		const auto result = h.Attempt(PipelineBuild::Fast);
		Check(result.first == vk::Result::eSuccess && !result.second.unoptimized &&
		          h.backend.compiled_outputs == 0,
		      prefix + "optimized application hit wins before Fast policy");
	}
	{
		Harness<Info> h;
		h.graphics.static_pipeline_cache = h.static_cache;
		h.backend.caches[HandleNumber(h.static_cache)].insert(StateOf(h.info));
		const auto result = h.Attempt(PipelineBuild::Fast);
		Check(result.first == vk::Result::eSuccess && !result.second.unoptimized &&
		          h.backend.cache_arguments == std::vector<uint64_t> {HandleNumber(h.static_cache)},
		      prefix + "existing optimized static-cache hit retains priority");
	}
	for (bool binary_success: {true, false}) {
		Harness<Info> h;
		TestStore     store;
		const auto    full_key = StateOf(h.info);
		store.entries.insert(full_key);
		h.graphics.pipeline_binaries = &store;
		h.backend.binary_success     = binary_success;
		if (!binary_success) h.backend.caches[HandleNumber(h.driver_cache)].insert(full_key);
		const auto result = h.Attempt(PipelineBuild::Fast);
		Check(result.first == vk::Result::eSuccess && !result.second.unoptimized &&
		          h.backend.compiled_outputs == 0 && h.backend.binary_calls == 1,
		      prefix + "full binaries or valid full fallback retain priority");
		Check(h.graphics.device.last_key == full_key,
		      prefix + "binary key query uses full state before Fast flag mutation");
	}
	for (bool disabled: {true, false}) {
		Harness<Info> h;
		if (disabled)
			h.graphics.pipeline_cache_control_enabled = false;
		else
			h.driver_cache = nullptr;
		const auto result = h.Attempt(PipelineBuild::Fast);
		Check(result.first == vk::Result::eSuccess && result.second.unoptimized &&
		          h.backend.probes == 0 && h.backend.cache_arguments == std::vector<uint64_t> {0},
		      prefix + "disabled or null cache retains original Fast fallback");
	}
	for (const auto mode: {PipelineBuild::Full, PipelineBuild::Optimize}) {
		Harness<Info> h;
		TestStore     store;
		h.graphics.pipeline_binaries = &store;
		auto fast                    = h.info;
		fast.flags |= vk::PipelineCreateFlagBits::eDisableOptimization;
		h.backend.caches[HandleNumber(h.driver_cache)].insert(StateOf(fast));
		const auto result = h.Attempt(mode);
		Check(result.first == vk::Result::eSuccess && !result.second.unoptimized &&
		          h.backend.compiled_outputs == 1 && h.backend.probes == 0,
		      prefix + "Full/Optimize do not accept Fast entries or add Fast attempts");
		Check(h.graphics.device.queries == (mode == PipelineBuild::Optimize ? 0u : 1u),
		      prefix + "Optimize keeps binary lookup bypass");
	}
	for (const auto mode: {PipelineBuild::Fast, PipelineBuild::Full, PipelineBuild::Optimize}) {
		Harness<Info> h;
		h.backend.creation_error = true;
		const auto result        = h.Attempt(mode);
		Check(result.first == vk::Result::eErrorDeviceLost && result.second.pipeline == nullptr &&
		          h.backend.compiled_outputs == 0,
		      prefix + "final creation failure is exposed without fabricated output");
	}
	{
		Harness<Info> h;
		TestStore     store;
		TestWriter    writer;
		store.entries.insert(StateOf(h.info));
		h.graphics.pipeline_binaries      = &store;
		h.graphics.pipeline_binary_writer = &writer;
		const auto result                 = h.Attempt(PipelineBuild::Fast);
		Check(
		    result.first == vk::Result::eSuccess && !result.second.unoptimized &&
		        writer.copies == 1 && writer.captures == 0 &&
		        h.backend.cache_arguments == std::vector<uint64_t> {0},
		    prefix +
		        "full binary recreation copies to writer without new capture or application cache");
	}
	{
		Harness<Info> h;
		TestStore     store;
		TestWriter    writer;
		h.graphics.pipeline_binaries      = &store;
		h.graphics.pipeline_binary_writer = &writer;
		h.graphics.device.key_success     = false;
		const auto result                 = h.Attempt(PipelineBuild::Full);
		Check(result.first == vk::Result::eSuccess && !result.second.unoptimized &&
		          writer.captures == 0 && writer.copies == 0 && h.backend.compiled_outputs == 1,
		      prefix + "failed binary key query preserves ordinary Full fallback");
	}
	for (bool claimed: {false, true}) {
		Harness<Info> h;
		TestWriter    writer;
		writer.claimed                    = claimed;
		h.graphics.pipeline_binary_writer = &writer;
		const auto result                 = h.Attempt(PipelineBuild::Fast);
		if (claimed)
			Check(result.first == vk::Result::ePipelineCompileRequired &&
			          result.second.pipeline == nullptr && h.backend.cache_arguments.empty(),
			      prefix + "claimed shard does not create or fabricate output");
		else
			Check(result.first == vk::Result::eSuccess && !result.second.unoptimized &&
			          h.backend.capture_calls == 1 && writer.captures == 1 &&
			          h.backend.cache_arguments == std::vector<uint64_t> {0},
			      prefix + "binary capture retains full policy and mandatory null cache");
	}
}

struct AttributionClock {
	uint64_t wall = 0, cpu = 0;
	bool available = true;
	unsigned samples = 0;
	std::vector<PipelineAttemptTrace::Record> records;
	static PipelineAttemptTrace::Sample Sample(void* pointer) noexcept {
		auto& clock = *static_cast<AttributionClock*>(pointer);
		++clock.samples;
		return {clock.wall, clock.cpu, clock.available};
	}
	static void Emit(void* pointer, const PipelineAttemptTrace::Record& record) noexcept {
		static_cast<AttributionClock*>(pointer)->records.push_back(record);
	}
	void Advance(bool probe, bool fast) {
		// Independent scripted durations. No real sleep and no prediction of a driver.
		wall += probe ? (fast ? 1000000u : 600000000u) : 3000000u;
		cpu += probe ? (fast ? 0u : 450000000u) : 2000000u;
	}
	PipelineAttemptTrace::Observer Observer() {
		records.reserve(64);
		PipelineAttemptTrace::Observer observer;
		observer.context.shaders[0] = {VK_SHADER_STAGE_COMPUTE_BIT, 0x123456789abcdef0ull, true};
		observer.context.shader_count = 1;
		observer.context.caller = PipelineAttemptTrace::Caller::Foreground;
		observer.context.driver_cache = PipelineAttemptTrace::DriverCache::Main;
		observer.context.mode = PipelineAttemptTrace::Mode::Fast;
		observer.context.cache_control = true;
		observer.user = this;
		observer.sample = Sample;
		observer.emit = Emit;
		return observer;
	}
};

template <typename Info>
void ExerciseAttribution(const char* stage) {
	namespace Trace = PipelineAttemptTrace;
	const std::string prefix = std::string(stage) + " attribution: ";
	{
		Harness<Info> h;
		AttributionClock clock;
		auto observer = clock.Observer();
		h.backend.advance = [&](bool probe, bool fast) { clock.Advance(probe, fast); };
		std::pair<vk::Result, PipelineCache::Pipeline> result;
		{
			Trace::ScopedObserver active(&observer);
			result = h.Attempt(PipelineBuild::Fast);
		}
		Check(result.first == vk::Result::eSuccess && result.second.pipeline != nullptr && result.second.unoptimized,
		      prefix + "scripted cold Fast request retains its real output");
		Check(clock.records.size() == 3, prefix + "three real attempts have separate records (honest RED oracle)");
		if (clock.records.size() == 3) {
			const std::array roles {Trace::Operation::OptimizedProbe, Trace::Operation::FastProbe, Trace::Operation::DriverCreate};
			const std::array<uint64_t, 3> walls {600000000u, 1000000u, 3000000u};
			const std::array<uint64_t, 3> cpus {450000000u, 0u, 2000000u};
			for (size_t n = 0; n < 3; ++n) {
				const auto& record = clock.records[n];
				Check(record.operation == roles[n] && record.cache == Trace::Cache::Driver &&
				          record.context == observer.context && record.flags_available &&
				          record.flags == h.backend.requests[n].effective_flags &&
				          record.result_kind == Trace::ResultKind::Vulkan &&
				          record.result == static_cast<int32_t>(h.backend.requests[n].result),
				      prefix + "role/hash/cache/mode/flags/result agree with each real callback");
				Check(record.wall_available && record.wall_ns == walls[n] &&
				          record.thread_cpu_available && record.thread_cpu_ns == cpus[n],
				      prefix + "slow optimized probe and cheap Fast work remain distinct");
			}
			Check(clock.wall == 604000000u && clock.records[0].wall_ns == 600000000u,
			      prefix + "604ms total cannot be mislabeled as final 3ms driver-create time");
		}
		Check(Trace::active == nullptr, prefix + "thread-local observer is restored");
	}
	{
		Harness<Info> h;
		AttributionClock clock;
		clock.available = false;
		auto observer = clock.Observer();
		h.backend.advance = [&](bool probe, bool fast) { clock.Advance(probe, fast); };
		{
			Trace::ScopedObserver active(&observer);
			h.Attempt(PipelineBuild::Fast);
		}
		Check(clock.records.size() == 3, prefix + "unavailable CPU clock still reports attempts");
		for (const auto& record : clock.records)
			Check(!record.thread_cpu_available && record.thread_cpu_ns == 0 && record.wall_available,
			      prefix + "unavailable CPU time is explicit, never wall time");
	}
	{
		AttributionClock outer, inner;
		auto first = outer.Observer();
		auto second = inner.Observer();
		{
			Trace::ScopedObserver a(&first);
			{
				Trace::ScopedObserver b(&second);
				Trace::Scope scope(Trace::Operation::Bookkeeping);
				inner.wall = 10;
				inner.cpu = 2;
			}
			Check(Trace::active == &first, prefix + "nested observer restores its parent");
			{
				outer.cpu = 100;
				Trace::Scope scope(Trace::Operation::Bookkeeping);
				outer.wall = 20;
				outer.cpu = 99;
			}
		}
		Check(Trace::active == nullptr && inner.records.size() == 1 && outer.records.size() == 1,
		      prefix + "nested records stay with their own observer");
		if (outer.records.size() == 1)
			Check(outer.records[0].wall_available && !outer.records[0].thread_cpu_available,
			      prefix + "regressing CPU clock is unavailable, never an unsigned duration");
	}

	enum class Case { Cold, OptimizedHit, FastHit, StaticHit, DisabledControl, NullCache,
	                  Full, Optimize, Error, BinarySuccess, BinaryFallback, BinaryCapture, BinaryClaimed, BinaryKeyFailed };
	struct Outcome {
		vk::Result result;
		uint64_t pipeline;
		bool unoptimized;
		std::vector<ApplicationBackend::Request> requests;
		std::map<uint64_t, std::set<PipelineBinaries::Key>> caches;
		std::array<unsigned, 10> counts;
		PipelineBinaries::Key last_key;
		bool operator==(const Outcome&) const = default;
	};
	for (const auto scenario : {Case::Cold, Case::OptimizedHit, Case::FastHit, Case::StaticHit,
	                           Case::DisabledControl, Case::NullCache, Case::Full, Case::Optimize,
	                           Case::Error, Case::BinarySuccess, Case::BinaryFallback,
	                           Case::BinaryCapture, Case::BinaryClaimed, Case::BinaryKeyFailed}) {
		const auto run = [&](bool enabled) {
			Harness<Info> h;
			TestStore store;
			TestWriter writer;
			AttributionClock clock;
			auto observer = clock.Observer();
			PipelineBuild mode = PipelineBuild::Fast;
			auto fast = h.info;
			fast.flags |= vk::PipelineCreateFlagBits::eDisableOptimization;
			switch (scenario) {
				case Case::OptimizedHit: h.backend.caches[HandleNumber(h.driver_cache)].insert(StateOf(h.info)); break;
				case Case::FastHit: h.backend.caches[HandleNumber(h.driver_cache)].insert(StateOf(fast)); break;
				case Case::StaticHit:
					h.graphics.static_pipeline_cache = h.static_cache;
					h.backend.caches[HandleNumber(h.static_cache)].insert(StateOf(h.info)); break;
				case Case::DisabledControl: h.graphics.pipeline_cache_control_enabled = false; break;
				case Case::NullCache: h.driver_cache = nullptr; break;
				case Case::Full: mode = PipelineBuild::Full; break;
				case Case::Optimize: mode = PipelineBuild::Optimize; break;
				case Case::Error: h.backend.creation_error = true; break;
				case Case::BinarySuccess:
				case Case::BinaryFallback:
					h.graphics.pipeline_binaries = &store;
					store.entries.insert(StateOf(h.info));
					h.backend.binary_success = scenario == Case::BinarySuccess; break;
				case Case::BinaryCapture:
				case Case::BinaryClaimed:
				case Case::BinaryKeyFailed:
					h.graphics.pipeline_binary_writer = &writer;
					writer.claimed = scenario == Case::BinaryClaimed;
					h.graphics.device.key_success = scenario != Case::BinaryKeyFailed; break;
				case Case::Cold: break;
			}
			observer.context.mode = mode == PipelineBuild::Fast ? Trace::Mode::Fast :
			                        mode == PipelineBuild::Optimize ? Trace::Mode::Optimize : Trace::Mode::Full;
			observer.context.cache_control = h.graphics.pipeline_cache_control_enabled;
			h.backend.advance = [&](bool probe, bool fast_probe) { clock.Advance(probe, fast_probe); };
			std::pair<vk::Result, PipelineCache::Pipeline> result;
			{
				Trace::ScopedObserver active(enabled ? &observer : nullptr);
				result = h.Attempt(mode);
			}
			Check(Trace::active == nullptr, prefix + "each test restores trace state");
			if (!enabled) {
				Check(clock.samples == 0 && clock.records.empty(), prefix + "trace off performs no clock or sink calls");
			} else {
				std::vector<Trace::Record> calls;
				for (const auto& record : clock.records) if (record.flags_available) calls.push_back(record);
				Check(calls.size() == h.backend.requests.size(), prefix + "every real create call is recorded once");
				if (calls.size() == h.backend.requests.size()) {
					for (size_t n = 0; n < calls.size(); ++n)
						Check(calls[n].context == observer.context && calls[n].flags == h.backend.requests[n].effective_flags &&
						          calls[n].result_kind == Trace::ResultKind::Vulkan &&
						          calls[n].result == static_cast<int32_t>(h.backend.requests[n].result),
						      prefix + "trace agrees with callback sequence without feedback injection");
				}
			}
			return Outcome {result.first, HandleNumber(result.second.pipeline), result.second.unoptimized,
			                h.backend.requests, h.backend.caches,
			                {h.backend.compiled_outputs, h.backend.ordinary_calls, h.backend.probes,
			                 h.backend.binary_calls, h.backend.capture_calls, store.loads,
			                 writer.captures, writer.copies, h.graphics.device.queries, h.graphics.device.destroys},
			                h.graphics.device.last_key};
		};
		Check(run(false) == run(true), prefix + "trace off/on conserve calls, cache keys, pNext chain, results and outputs");
	}
}

} // namespace

int main() {
	Exercise<vk::ComputePipelineCreateInfo>("compute");
	Exercise<vk::GraphicsPipelineCreateInfo>("graphics");
	ExerciseAttribution<vk::ComputePipelineCreateInfo>("compute");
	ExerciseAttribution<vk::GraphicsPipelineCreateInfo>("graphics");
	std::printf("PipelineBuildAttemptTests: %u failures (CPU model; no Vulkan driver).\n",
	            failures);
	return failures == 0 ? 0 : 1;
}
