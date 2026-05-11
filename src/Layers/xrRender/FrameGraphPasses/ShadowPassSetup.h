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
}

namespace xray::render::fg {
class RenderDevice;
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
};

struct ShadowPassResources
{
    framegraph::VirtualResourceHandle sunShadowArray;
    u32 sunCascadeCount = 0;
};

ShadowPassResources setupShadowPasses(
    framegraph::FrameGraph& fg,
    shadow::ShadowManager& shadows,
    RenderDevice* device,
    const GeometryCollector* geometry,
    ShadowPassState* state);

}
