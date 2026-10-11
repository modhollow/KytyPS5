#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <type_traits>
#include <vulkan/vulkan.hpp>
#include "graphics/host_gpu/renderer/pipeline/pipelineAttemptTrace.h"

namespace Libs::Graphics {

namespace PipelineBinaries {
struct Key {
	uint32_t value = 41;
};
} // namespace PipelineBinaries

namespace PipelineCache {
struct Pipeline {
	vk::Pipeline pipeline {};
};
} // namespace PipelineCache

struct CaptureWriter {
	bool         internal_success = true;
	bool         captured_success = true;
	unsigned     internal_calls   = 0;
	unsigned     captured_calls   = 0;
	const void*  internal_info    = nullptr;
	vk::Pipeline captured_pipeline {};
	uint32_t     seen_key = 0;

	bool Capture(const PipelineBinaries::Key& key, vk::Pipeline pipeline,
	             const void* create_info = nullptr) {
		seen_key = key.value;
		if (create_info != nullptr) {
			++internal_calls;
			internal_info = create_info;
			return internal_success;
		}
		++captured_calls;
		captured_pipeline = pipeline;
		return captured_success;
	}
};

struct DestroyDevice {
	unsigned     calls = 0;
	vk::Pipeline destroyed {};

	void destroyPipeline(vk::Pipeline pipeline, std::nullptr_t) {
		++calls;
		destroyed = pipeline;
	}
};

struct GraphicContext {
	CaptureWriter*    pipeline_binary_writer = nullptr;
	vk::PipelineCache static_pipeline_cache {};
	bool              pipeline_cache_control_enabled = true;
	DestroyDevice     device {};
};

// This is the same owning template included by shaders.cpp, with CPU stand-ins
// at its device/writer boundary. No Vulkan command is executed by this test.
#include "graphics/host_gpu/renderer/pipeline/pipelineBinaryCapture.inc"

} // namespace Libs::Graphics

namespace {

unsigned failures = 0;

void Check(bool condition, const std::string& message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

template <typename Handle>
Handle HandleValue(uintptr_t value) {
	using Raw = typename Handle::CType;
	if constexpr (std::is_pointer_v<Raw>) {
		return Handle(reinterpret_cast<Raw>(value));
	} else {
		return Handle(static_cast<Raw>(value));
	}
}

enum class Scenario {
	ColdCapture,
	LookupHit,
	LookupMiss,
	LookupCaptureMiss,
	LookupDisabled,
	DriverFailure,
	CaptureFailure,
};

template <typename Info>
void Exercise(const char* stage, Scenario scenario) {
	using namespace Libs::Graphics;
	const std::string prefix =
	    std::string(stage) + " scenario " + std::to_string(static_cast<int>(scenario));
	const auto    driver_cache = HandleValue<vk::PipelineCache>(1);
	const auto    static_cache = HandleValue<vk::PipelineCache>(2);
	const auto    produced     = HandleValue<vk::Pipeline>(3);
	CaptureWriter writer;
	writer.internal_success = scenario != Scenario::LookupCaptureMiss;
	writer.captured_success = scenario != Scenario::CaptureFailure;
	GraphicContext graphics;
	graphics.pipeline_binary_writer = &writer;
	if (scenario == Scenario::LookupHit || scenario == Scenario::LookupMiss ||
	    scenario == Scenario::LookupCaptureMiss || scenario == Scenario::LookupDisabled) {
		graphics.static_pipeline_cache = static_cache;
	}
	graphics.pipeline_cache_control_enabled = scenario != Scenario::LookupDisabled;
	PipelineCache::Pipeline                pipeline;
	PipelineBinaries::Key                  key;
	vk::PipelineCreationFeedbackCreateInfo feedback {};
	Info                                   info {};
	info.flags                   = vk::PipelineCreateFlagBits::eDisableOptimization;
	info.pNext                   = &feedback;
	unsigned lookup_calls        = 0;
	unsigned capture_calls       = 0;
	bool     wrong_capture_cache = false;
	auto     create              = [&](vk::PipelineCache cache, const Info& requested) {
		const bool capture = requested.pNext != nullptr && requested.pNext != &feedback;
		if (capture) {
			const auto* flags =
			    static_cast<const vk::PipelineCreateFlags2CreateInfoKHR*>(requested.pNext);
			++capture_calls;
			Check(flags->sType == vk::StructureType::ePipelineCreateFlags2CreateInfoKHR,
			      prefix + " capture uses the flags2 structure");
			Check(bool(flags->flags & vk::PipelineCreateFlagBits2KHR::eCaptureDataKHR),
			      prefix + " capture flag is present");
			Check(bool(flags->flags & vk::PipelineCreateFlagBits2KHR::eDisableOptimization),
			      prefix + " original flags survive the flags2 wrapper");
			Check(flags->pNext == &feedback, prefix + " original pNext survives capture");
			wrong_capture_cache = wrong_capture_cache || cache != nullptr;
			// Enforce the actual Vulkan creation contract at the owning callback.
			// The original helper must fail here for compute AND graphics.
			if (cache != nullptr) return vk::Result::eErrorValidationFailedEXT;
			if (scenario == Scenario::DriverFailure) return vk::Result::eErrorDeviceLost;
			pipeline.pipeline = produced;
			return vk::Result::eSuccess;
		}
		++lookup_calls;
		Check(cache == static_cache, prefix + " uncaptured lookup keeps its static cache");
		Check(requested.pNext == &feedback, prefix + " uncaptured lookup keeps original pNext");
		Check(bool(requested.flags & vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired),
		      prefix + " lookup forbids compilation");
		Check(bool(requested.flags & vk::PipelineCreateFlagBits::eDisableOptimization),
		      prefix + " lookup preserves original flags");
		if (scenario == Scenario::LookupMiss) return vk::Result::ePipelineCompileRequired;
		pipeline.pipeline = produced;
		return vk::Result::eSuccess;
	};
	const auto result =
	    CapturePipelineBinaries(graphics, pipeline, info, driver_cache, key, create);
	Check(!wrong_capture_cache,
	      prefix + " capture callback must receive VK_NULL_HANDLE (VUID09617)");
	Check(info.pNext == &feedback && info.flags == vk::PipelineCreateFlagBits::eDisableOptimization,
	      prefix + " caller's create info is unchanged");
	const bool lookup = scenario == Scenario::LookupHit || scenario == Scenario::LookupMiss ||
	                    scenario == Scenario::LookupCaptureMiss;
	Check(lookup_calls == (lookup ? 1u : 0u), prefix + " expected lookup callback count");
	Check(capture_calls == (scenario == Scenario::LookupHit ? 0u : 1u),
	      prefix + " expected capture callback count");
	Check(result == (scenario == Scenario::DriverFailure ? vk::Result::eErrorDeviceLost
	                                                     : vk::Result::eSuccess),
	      prefix + " driver result is propagated");
	Check(graphics.device.calls == (scenario == Scenario::LookupCaptureMiss ? 1u : 0u),
	      prefix + " failed internal capture destroys exactly the lookup pipeline");
	Check(writer.internal_calls ==
	          ((scenario == Scenario::LookupHit || scenario == Scenario::LookupCaptureMiss) ? 1u
	                                                                                        : 0u),
	      prefix + " internal capture callback count");
	if (scenario != Scenario::LookupHit && scenario != Scenario::DriverFailure &&
	    !wrong_capture_cache) {
		Check(writer.captured_calls == 1 && writer.captured_pipeline == produced &&
		          writer.seen_key == key.value,
		      prefix + " successful creation reaches the same writer/key/pipeline");
	}
	if (scenario == Scenario::DriverFailure) {
		Check(writer.captured_calls == 0 && pipeline.pipeline == nullptr,
		      prefix + " failed creation does not capture or fabricate a pipeline");
	}
	if (scenario == Scenario::LookupHit) {
		Check(writer.captured_calls == 0 && writer.internal_info != nullptr &&
		          writer.seen_key == key.value,
		      prefix + " ordinary lookup hit uses internal capture without compile");
	}
}

template <typename Info>
void ExerciseStage(const char* stage) {
	for (const auto scenario: {Scenario::ColdCapture, Scenario::LookupHit, Scenario::LookupMiss,
	                           Scenario::LookupCaptureMiss, Scenario::LookupDisabled,
	                           Scenario::DriverFailure, Scenario::CaptureFailure}) {
		Exercise<Info>(stage, scenario);
	}
}

} // namespace

int main() {
	ExerciseStage<vk::ComputePipelineCreateInfo>("compute");
	ExerciseStage<vk::GraphicsPipelineCreateInfo>("graphics");
	if (failures != 0) {
		std::cerr << failures << " capture contract checks failed\n";
		return 1;
	}
	std::cout << "Compute and graphics capture contract checks passed (14 scenarios, no GPU).\n";
	return 0;
}
