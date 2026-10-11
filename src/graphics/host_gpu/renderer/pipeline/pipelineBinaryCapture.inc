// The precompile's capture (PipelineBinaryWriter): a pipeline the static pipeline cache holds (the
// first store is made from it), its binaries then from the driver's own cache (a pipeline created
// with the capture flag is never a cache hit: compiled); else compiled with its binaries kept.
template <typename Info, typename Create>
static vk::Result CapturePipelineBinaries(GraphicContext&          graphics,
                                          PipelineCache::Pipeline& pipeline, Info info,
                                          vk::PipelineCache            driver_cache,
                                          const PipelineBinaries::Key& key, Create create) {
	auto* writer = graphics.pipeline_binary_writer;
	if (graphics.static_pipeline_cache != nullptr && graphics.pipeline_cache_control_enabled) {
		auto lookup = info;
		lookup.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
		if (PipelineAttemptTrace::Invoke(PipelineAttemptTrace::Operation::BinaryCaptureProbe,
		                                 PipelineAttemptTrace::Cache::Static,
		                                 graphics.static_pipeline_cache, lookup,
		                                 create) == vk::Result::eSuccess) {
			bool captured;
			{
				PipelineAttemptTrace::Scope trace(
				    PipelineAttemptTrace::Operation::BinaryCaptureStore);
				captured = writer->Capture(key, nullptr, &info);
				trace.BooleanResult(captured);
			}
			if (captured) return vk::Result::eSuccess;
			graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
		}
		pipeline.pipeline = nullptr;
	}
	vk::PipelineCreateFlags2CreateInfoKHR flags {};
	flags.flags = vk::PipelineCreateFlags2KHR(static_cast<VkPipelineCreateFlags>(info.flags)) |
	              vk::PipelineCreateFlagBits2KHR::eCaptureDataKHR;
	flags.pNext = info.pNext;
	info.pNext  = &flags;
	// Vulkan binary capture requires a null application pipeline cache (VUID09617).
	const auto result =
	    PipelineAttemptTrace::Invoke(PipelineAttemptTrace::Operation::BinaryCaptureCreate,
	                                 PipelineAttemptTrace::Cache::None, nullptr, info, create);
	if (result == vk::Result::eSuccess) {
		PipelineAttemptTrace::Scope trace(PipelineAttemptTrace::Operation::BinaryCaptureStore);
		const bool                  captured = writer->Capture(key, pipeline.pipeline);
		trace.BooleanResult(captured);
		if (!captured) std::printf("Pipeline binaries: the driver gave none for a pipeline\n");
	}
	return result;
}
