#pragma once

#include "Layers/xrRender/FrameGraph/FGTypes.h"
#include "Layers/xrRender/FrameGraph/FGResource.h"

namespace xray::render::framegraph {
class FrameGraph;
}

namespace xray::render::shadow {
class ShadowManager;
}

namespace xray::render {
class GeometryCollector;
struct GeometryBatch;
}

namespace xray::render::fg {
class RenderDevice;
class FGDetailManager;
}

namespace xray::render::fg::passes {

struct ShadowPassState
{
    bool initialized = false;
    nvrhi::ShaderHandle shadowVS;
    nvrhi::ShaderHandle shadowPS;
    nvrhi::InputLayoutHandle shadowInputLayout;
    nvrhi::BindingLayoutHandle shadowBindingLayout;
    nvrhi::GraphicsPipelineHandle shadowPipeline;

    struct SkinnedVariant
    {
        nvrhi::ShaderHandle vs;
        nvrhi::InputLayoutHandle inputLayout;
        nvrhi::GraphicsPipelineHandle pipeline;
    };

    bool skinnedInitialized = false;
    nvrhi::BindingLayoutHandle skinnedBindingLayout;
    nvrhi::ShaderHandle skinnedPS;
    SkinnedVariant skNonHQ;
    SkinnedVariant skHQ1W;
    SkinnedVariant skHQ2W;
    SkinnedVariant skHQ3W;
    SkinnedVariant skHQ4W;

    bool grassInitialized = false;
    nvrhi::ShaderHandle grassShadowVS;
    nvrhi::ShaderHandle billboardShadowVS;
    nvrhi::ShaderHandle billboardShadowPS;
    nvrhi::InputLayoutHandle grassShadowInputLayout;
    nvrhi::BindingLayoutHandle grassShadowBindingLayout;
    nvrhi::BindingLayoutHandle billboardShadowBindingLayout;
    nvrhi::GraphicsPipelineHandle grassShadowPipeline;
    nvrhi::GraphicsPipelineHandle billboardShadowPipeline;

    nvrhi::ComputePipelineHandle grassShadowCullPipeline;
    nvrhi::BindingLayoutHandle grassShadowCullLayout;

    static constexpr u32 NUM_SHADOW_CASCADES = 4;
    static constexpr u32 NUM_GRASS_LODS = 3;
    static constexpr u32 MAX_SHADOW_GRASS_INSTANCES = 512 * 1024;
    struct GrassShadowCascadeBuffers {
        nvrhi::BufferHandle visibleBuffer[NUM_GRASS_LODS];
        nvrhi::BufferHandle drawArgsBuffer[NUM_GRASS_LODS];
        nvrhi::BufferHandle visibleDecalBuffer;
        nvrhi::BufferHandle decalDrawArgsBuffer;
    };
    GrassShadowCascadeBuffers grassShadowCascades[NUM_SHADOW_CASCADES];
};

struct ShadowPassResources
{
    framegraph::VirtualResourceHandle sunShadowArray;
    framegraph::VirtualResourceHandle hudShadowMap;
    u32 sunCascadeCount = 0;
};

ShadowPassResources setupShadowPasses(
    framegraph::FrameGraph& fg,
    shadow::ShadowManager& shadows,
    RenderDevice* device,
    const GeometryCollector* geometry,
    ShadowPassState* state,
    const xr_vector<GeometryBatch>* hudBatches = nullptr,
    FGDetailManager* detailManager = nullptr);

}
