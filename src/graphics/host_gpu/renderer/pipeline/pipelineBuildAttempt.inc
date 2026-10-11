// The static precompile's pipeline when it holds this one (no compilation), else built as `build`
// says.
template <typename Info, typename Create>
static vk::Result CreatePipelineHandle(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                                       Info info, vk::PipelineCache driver_cache,
                                       PipelineBuild build, Create create) {
	const auto cached = [&](vk::PipelineCache cache, PipelineAttemptTrace::Operation operation,
	                        PipelineAttemptTrace::Cache cache_class) {
		if (cache == nullptr || !graphics.pipeline_cache_control_enabled) return false;
		auto lookup = info;
		lookup.flags |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
		if (PipelineAttemptTrace::Invoke(operation, cache_class, cache, lookup, create) ==
		    vk::Result::eSuccess)
			return true;
		pipeline.pipeline = nullptr;
		return false;
	};
	// Its binaries (pipelineBinaries.h) when the store holds them; the precompile keeps every
	// pipeline's.
	const auto* store  = graphics.pipeline_binaries;
	auto*       writer = graphics.pipeline_binary_writer;
	if (build != PipelineBuild::Optimize && (store != nullptr || writer != nullptr)) {
		PipelineBinaries::Key key;
		bool                  have_key;
		{
			PipelineAttemptTrace::Scope trace(PipelineAttemptTrace::Operation::BinaryKey);
			have_key = PipelineBinaries::PipelineKey(graphics.device, &info, key);
			trace.BooleanResult(have_key);
		}
		if (have_key) {
			if (writer != nullptr && PipelineKeyLog()) LogPipelineKey("tool", key, info);
			// Another shard of this precompile holds it: no pipeline (see CreatePipelineInternal).
			if (writer != nullptr && writer->Claimed(key))
				return vk::Result::ePipelineCompileRequired;
			const auto binaries = [&] {
				if (store == nullptr) return PipelineBinaries::Handles {};
				PipelineAttemptTrace::Scope trace(PipelineAttemptTrace::Operation::BinaryLookup);
				return store->Load(key);
			}();
			if (!binaries.binaries.empty()) {
				vk::PipelineBinaryInfoKHR from {};
				from.binaryCount = static_cast<uint32_t>(binaries.binaries.size());
				from.pPipelineBinaries =
				    reinterpret_cast<const vk::PipelineBinaryKHR*>(binaries.binaries.data());
				auto with  = info;
				from.pNext = with.pNext;
				with.pNext = &from;
				if (PipelineAttemptTrace::Invoke(PipelineAttemptTrace::Operation::BinaryCreate,
				                                 PipelineAttemptTrace::Cache::None, nullptr, with,
				                                 create) == vk::Result::eSuccess) {
					if (writer != nullptr) {
						PipelineAttemptTrace::Scope trace(
						    PipelineAttemptTrace::Operation::BinaryCopy);
						writer->Copy(key, *store);
					}
					return vk::Result::eSuccess;
				}
				pipeline.pipeline = nullptr;
			}
			if (writer == nullptr && PipelineKeyLog()) LogPipelineKey("miss", key, info);
			if (writer != nullptr)
				return CapturePipelineBinaries(graphics, pipeline, info, driver_cache, key, create);
		}
	}
	if (build != PipelineBuild::Optimize && store == nullptr &&
	    cached(graphics.static_pipeline_cache, PipelineAttemptTrace::Operation::StaticProbe,
	           PipelineAttemptTrace::Cache::Static))
		return vk::Result::eSuccess;
	if (build != PipelineBuild::Fast)
		return PipelineAttemptTrace::Invoke(PipelineAttemptTrace::Operation::DriverCreate,
		                                    driver_cache != nullptr
		                                        ? PipelineAttemptTrace::Cache::Driver
		                                        : PipelineAttemptTrace::Cache::None,
		                                    driver_cache, info, create);
	if (cached(driver_cache, PipelineAttemptTrace::Operation::OptimizedProbe,
	           PipelineAttemptTrace::Cache::Driver))
		return vk::Result::eSuccess;
	// An unoptimized cache hit still needs the normal optimized replacement.
	info.flags |= vk::PipelineCreateFlagBits::eDisableOptimization;
	pipeline.unoptimized = true;
	if (cached(driver_cache, PipelineAttemptTrace::Operation::FastProbe,
	           PipelineAttemptTrace::Cache::Driver))
		return vk::Result::eSuccess;
	const auto cache = graphics.pipeline_cache_control_enabled ? driver_cache : nullptr;
	return PipelineAttemptTrace::Invoke(PipelineAttemptTrace::Operation::DriverCreate,
	                                    cache != nullptr ? PipelineAttemptTrace::Cache::Driver
	                                                     : PipelineAttemptTrace::Cache::None,
	                                    cache, info, create);
}
