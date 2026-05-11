#include "stdafx.h"
#include "ShadowPassSetup.h"
#include "PassCommon.h"
#include "Layers/xrRender/Shadow/ShadowManager.h"
#include "Layers/xrRender/Shadow/ShadowTypes.h"
#include "Layers/xrRender/FrameGraph/FrameGraph.h"
#include "Layers/xrRender/FrameGraph/RenderPassBuilder.h"
#include "Layers/xrRender/FrameGraph/ShaderLoader.h"
#include "Layers/xrRender/FrameGraph/PassResourceCache.h"
#include "Layers/xrRender/FrameGraph/BindingSetBuilder.h"
#include "Layers/xrRender/Geometry/GeometryBatch.h"
#include "Layers/xrRender/RenderContext/RenderContext.h"
#include "Layers/xrRender/RenderContext/RenderDevice.h"
#include "Layers/xrRender/Backend/D3D12Backend.h"
#include "Layers/xrRender/Bindless/MaterialBuffer.h"
#include "Layers/xrRender/GPUCullingManager.h"

namespace xray::render::fg::passes {

struct alignas(16) ShadowViewCB
{
    Fmatrix lightVP;
    u32 cascadeIndex;
    float smapSize;
    float padding[2];
};

static void InitializeShadowResources(
    fg::RenderDevice* device,
    const nvrhi::FramebufferInfoEx& fbInfo,
    ShadowPassState& state)
{
    if (state.initialized)
        return;

    nvrhi::IDevice* nvDevice = device->GetNVRHIDevice();
    auto* shaderLoader = GEnv.Render->GetShaderLoader();

    auto vsResult = shaderLoader->LoadVertexShader("bindless_shadow", "main");
    auto psResult = shaderLoader->LoadPixelShader("bindless_shadow", "main");

    if (!vsResult.handle || !psResult.handle) {
        Msg("! [ShadowPass] Failed to load shadow shaders");
        return;
    }

    state.shadowVS = vsResult.handle;
    state.shadowPS = psResult.handle;

    auto& cache = framegraph::GetPassResourceCache();

    state.shadowBindingLayout = cache.GetOrCreateBindingLayoutFromReflection(
        "ShadowPass", *vsResult.reflection, *psResult.reflection, nvDevice);

    u32 attrCount = 0;
    auto* attrs = GetUnifiedVertexAttributes(attrCount);
    state.shadowInputLayout = nvDevice->createInputLayout(attrs, attrCount, state.shadowVS);

    nvrhi::GraphicsPipelineDesc pipeDesc;
    pipeDesc.VS = state.shadowVS;
    pipeDesc.PS = state.shadowPS;
    pipeDesc.inputLayout = state.shadowInputLayout;
    pipeDesc.primType = nvrhi::PrimitiveType::TriangleList;

    auto* backend = device->GetBackend();
    nvrhi::IBindingLayout* bindlessLayout = backend ? backend->GetBindlessLayout() : nullptr;

    if (bindlessLayout)
        pipeDesc.bindingLayouts = { state.shadowBindingLayout, bindlessLayout };
    else
        pipeDesc.bindingLayouts = { state.shadowBindingLayout };

    pipeDesc.renderState.depthStencilState.depthTestEnable = true;
    pipeDesc.renderState.depthStencilState.depthWriteEnable = true;
    pipeDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
    pipeDesc.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
    pipeDesc.renderState.rasterState.frontCounterClockwise = false;
    pipeDesc.renderState.rasterState.depthBias = 100;
    pipeDesc.renderState.rasterState.slopeScaledDepthBias = 2.0f;
    pipeDesc.renderState.rasterState.depthBiasClamp = 0.01f;

    state.shadowPipeline = cache.GetOrCreatePipeline("ShadowPass", pipeDesc, fbInfo, nvDevice);
    if (!state.shadowPipeline) {
        Msg("! [ShadowPass] Failed to create shadow pipeline");
        return;
    }

    state.initialized = true;
    Msg("* [ShadowPass] Pipeline initialized");
}

struct ShadowPassData
{
    framegraph::VirtualResourceHandle shadowArray;
    u32 cascadeCount;
    u32 smapSize;
    fg::RenderDevice* device;
    shadow::ShadowManager* shadows;
    ShadowPassState* passState;
};

ShadowPassResources setupShadowPasses(
    framegraph::FrameGraph& fg,
    shadow::ShadowManager& shadows,
    RenderDevice* device,
    const GeometryCollector* geometry,
    ShadowPassState* state)
{
    ShadowPassResources resources;

    if (!shadows.IsActive())
        return resources;

    const auto& config = shadows.GetConfig();
    resources.sunCascadeCount = config.cascadeCount;

    nvrhi::FramebufferInfoEx fbInfo;
    fbInfo.depthFormat = nvrhi::Format::D32;
    InitializeShadowResources(device, fbInfo, *state);
    if (!state->initialized)
        return resources;

    framegraph::ResourceDesc shadowDesc;
    shadowDesc.type = framegraph::ResourceDesc::Type::Texture2DArray;
    shadowDesc.width = config.shadowMapSize;
    shadowDesc.height = config.shadowMapSize;
    shadowDesc.arraySize = config.cascadeCount;
    shadowDesc.mipLevels = 1;
    shadowDesc.format = nvrhi::Format::D32;
    shadowDesc.isDepthStencil = true;
    shadowDesc.isRenderTarget = false;
    shadowDesc.isTransient = true;
    shadowDesc.debugName = "rt_SunShadowArray";

    auto sunShadowArray = fg.CreateTexture("rt_SunShadowArray", shadowDesc);
    resources.sunShadowArray = sunShadowArray;

    fg.addCallbackPass<ShadowPassData>(
        "SunShadowPass",
        [&, sunShadowArray, state](
            framegraph::FrameGraph& builder,
            framegraph::PassHandle passHandle,
            ShadowPassData& data)
        {
            framegraph::RenderPassBuilder passBuilder(builder, passHandle);

            data.shadowArray = passBuilder.write(sunShadowArray,
                framegraph::ResourceState::DepthStencilWrite);
            data.cascadeCount = config.cascadeCount;
            data.smapSize = config.shadowMapSize;
            data.device = device;
            data.shadows = &shadows;
            data.passState = state;
        },
        [](const ShadowPassData& data, const framegraph::FrameGraph& fg,
           fg::RenderContext* ctx)
        {
            nvrhi::IDevice* nvDevice = data.device->GetNVRHIDevice();
            nvrhi::ICommandList* cmdList = ctx->GetCommandList();

            nvrhi::ITexture* shadowArrayTex = fg.GetPhysicalTexture(data.shadowArray);

            auto* gpuCulling = xray::render::fg::RImplementation.GetGPUCullingManager();
            if (!gpuCulling || !gpuCulling->AreMegaBuffersReady())
                return;

            auto& cache = framegraph::GetPassResourceCache();
            auto& matBuffer = fg::bindless::MaterialBuffer::Instance();

            auto* shaderLoader = GEnv.Render->GetShaderLoader();
            auto* vsRefl = shaderLoader->GetCachedReflection("bindless_shadow", ".vs");
            auto* psRefl = shaderLoader->GetCachedReflection("bindless_shadow", ".ps");

            auto* backend = data.device->GetBackend();
            nvrhi::IBindingSet* bindlessTable = backend ? backend->GetBindlessDescriptorTable() : nullptr;
            auto drawIndexBuffer = GetOrCreateDrawIndexBuffer("ShadowPass", nvDevice);

            auto shadowCBBuffer = cache.GetOrCreateVolatileCB(
                "ShadowPass", "ShadowViewCB", sizeof(ShadowViewCB), data.device, 16);

            float smapSizeF = static_cast<float>(data.smapSize);
            nvrhi::Viewport smapViewport(0.0f, smapSizeF, 0.0f, smapSizeF, 0.0f, 1.0f);

            for (u32 c = 0; c < data.cascadeCount; ++c)
            {
                nvrhi::TextureSubresourceSet sliceSubresource;
                sliceSubresource.baseMipLevel = 0;
                sliceSubresource.numMipLevels = 1;
                sliceSubresource.baseArraySlice = c;
                sliceSubresource.numArraySlices = 1;

                nvrhi::FramebufferDesc fbDesc;
                fbDesc.setDepthAttachment(nvrhi::FramebufferAttachment()
                    .setTexture(shadowArrayTex)
                    .setSubresources(sliceSubresource));

                string128 fbName;
                xr_sprintf(fbName, "SunCascade_%u", c);
                auto framebuffer = cache.GetOrCreateFramebuffer(fbName, fbDesc, nvDevice);

                cmdList->clearDepthStencilTexture(shadowArrayTex, sliceSubresource, true, 1.0f, false, 0);

                const auto& view = data.shadows->GetView(c);

                ShadowViewCB shadowCB;
                shadowCB.lightVP = view.viewProjection;
                shadowCB.cascadeIndex = c;
                shadowCB.smapSize = smapSizeF;
                shadowCB.padding[0] = 0;
                shadowCB.padding[1] = 0;
                cmdList->writeBuffer(shadowCBBuffer, &shadowCB, sizeof(shadowCB));

                framegraph::BindingSetBuilder bsb(*vsRefl, *psRefl, nvDevice, "ShadowPass");
                bsb.ConstantBuffer("ShadowViewCB", shadowCBBuffer);
                bsb.BufferSRV("g_Materials", matBuffer.GetBuffer());
                bsb.BufferSRV("g_InstanceData", gpuCulling->GetStaticInstanceBuffer());
                bsb.BufferSRV("g_CompactBatchIndices", gpuCulling->GetStaticCompactBatchIndicesBuffer());
                bsb.BufferSRV("g_CompactMaterialIDs", gpuCulling->GetStaticCompactMaterialIDBuffer());

                auto bindingSet = cache.GetOrCreateBindingSet(
                    bsb.Build(), data.passState->shadowBindingLayout, nvDevice);

                nvrhi::GraphicsState gfxState;
                gfxState.pipeline = data.passState->shadowPipeline;
                gfxState.framebuffer = framebuffer;
                gfxState.vertexBuffers = {
                    { gpuCulling->GetMegaVertexBuffer(), 0, 0 },
                    { drawIndexBuffer, 1, 0 }
                };
                gfxState.indexBuffer = { gpuCulling->GetMegaIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
                gfxState.viewport.addViewport(smapViewport);
                gfxState.viewport.addScissorRect(nvrhi::Rect(data.smapSize, data.smapSize));
                gfxState.bindings = { bindingSet };
                if (bindlessTable)
                    gfxState.addBindingSet(bindlessTable);
                gfxState.indirectParams = gpuCulling->GetStaticCompactDrawArgsBuffer();

                cmdList->setGraphicsState(gfxState);

                DrawIndexedIndirectCountOrFallback(
                    cmdList, 0, 0, gpuCulling->GetStaticObjectCount());
            }
        }
    );

    return resources;
}

}
