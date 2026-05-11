#include "stdafx.h"
#include "ShadowPassSetup.h"
#include "Layers/xrRender/Shadow/ShadowManager.h"

namespace xray::render::fg::passes {

ShadowPassResources setupShadowPasses(
    framegraph::FrameGraph& fg,
    shadow::ShadowManager& shadows,
    RenderDevice* device)
{
    ShadowPassResources resources;
    resources.sunCascadeCount = shadows.GetConfig().cascadeCount;
    return resources;
}

}
