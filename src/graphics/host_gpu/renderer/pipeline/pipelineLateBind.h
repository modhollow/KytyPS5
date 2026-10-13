#pragma once

#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

// Draws and dispatches that bind a pipeline a worker still builds (pipelineLateBuild.h).
namespace Libs::Graphics::PipelineLateBind {

// While one lives on this thread, PipelineCache::CreateGraphicsPipeline and CreateComputePipeline
// may return a pipeline whose build a worker has only started (with the Vulkan recording worker:
// its commands are recorded on another thread). Its layouts are there; its caller binds it with
// BindPipeline and reads no `pipeline` handle of it.
class Scope {
public:
	Scope();
	~Scope();
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	bool m_outer;
};
[[nodiscard]] bool Allowed();

// Its build is not done yet (no handle to record in a packet).
[[nodiscard]] inline bool Pending(const PipelineCache::Pipeline& pipeline) {
	return pipeline.pipeline == nullptr;
}

// Binds the pipeline: a pending one by a recorded wait for its build, in command order.
void BindPipeline(vk::CommandBuffer command, vk::PipelineBindPoint point,
                  const PipelineCache::Pipeline& pipeline);

// The descriptor set layout and pipeline layout of a pipeline (what its draws and dispatches bind
// before the pipeline exists); CreatePipelineInternal then builds the pipeline with them.
void CreatePipelineLayouts(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                           const ShaderVertexInputInfo& vs_input_info,
                           const ShaderProgram&         vertex_program,
                           const ShaderPixelInputInfo*  ps_input_info,
                           const ShaderProgram& pixel_program, bool native_bindings);
void CreatePipelineLayouts(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                           const ShaderComputeInputInfo& input_info, bool native_bindings);
// While one lives on this thread, CreatePipelineInternal builds a Full or Fast pipeline with the
// layouts CreatePipelineLayouts gave its Pipeline instead of making them (a Pipeline that has
// layouts is otherwise an error).
class LayoutsGiven {
public:
	LayoutsGiven();
	~LayoutsGiven();
	LayoutsGiven(const LayoutsGiven&)            = delete;
	LayoutsGiven& operator=(const LayoutsGiven&) = delete;

	[[nodiscard]] static bool Active();

private:
	bool m_outer;
};

} // namespace Libs::Graphics::PipelineLateBind
