#include "stdafx.h"
#include "ShadowPassSetup.h"
#include "ShaderConstants.h"
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
#include "Layers/xrRender/SkeletonCustom.h"
#include "Layers/xrRender/FSkinned.h"
#include "Layers/xrRender/SkeletonX.h"

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

static void InitializeSkinnedShadowResources(
    fg::RenderDevice* device,
    const nvrhi::FramebufferInfoEx& fbInfo,
    ShadowPassState& state)
{
    if (state.skinnedInitialized)
        return;

    nvrhi::IDevice* nvDevice = device->GetNVRHIDevice();
    auto* shaderLoader = GEnv.Render->GetShaderLoader();

    auto psResult = shaderLoader->LoadPixelShader("shadow_skinned", "main");
    if (!psResult.handle) {
        Msg("! [ShadowPass] Failed to load shadow_skinned.ps");
        return;
    }
    state.skinnedPS = psResult.handle;

    auto& cache = framegraph::GetPassResourceCache();

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(3),
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(4),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),
        nvrhi::BindingLayoutItem::Sampler(0),
    };
    state.skinnedBindingLayout = nvDevice->createBindingLayout(layoutDesc);

    auto* backend = device->GetBackend();
    nvrhi::IBindingLayout* bindlessLayout = backend ? backend->GetBindlessLayout() : nullptr;

    auto initVariant = [&](ShadowPassState::SkinnedVariant& var,
        const char* shaderName, const char* pipeName,
        nvrhi::VertexAttributeDesc* attribs, u32 attribCount)
    {
        auto vsResult = shaderLoader->LoadVertexShader(shaderName, "main");
        if (!vsResult.handle) return;
        var.vs = vsResult.handle;
        var.inputLayout = nvDevice->createInputLayout(attribs, attribCount, var.vs);

        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = var.vs;
        pd.PS = state.skinnedPS;
        pd.inputLayout = var.inputLayout;
        if (bindlessLayout)
            pd.bindingLayouts = { state.skinnedBindingLayout, bindlessLayout };
        else
            pd.bindingLayouts = { state.skinnedBindingLayout };
        pd.primType = nvrhi::PrimitiveType::TriangleList;
        pd.renderState.depthStencilState.depthTestEnable = true;
        pd.renderState.depthStencilState.depthWriteEnable = true;
        pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        pd.renderState.rasterState.frontCounterClockwise = false;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::Back;
        pd.renderState.rasterState.depthBias = 100;
        pd.renderState.rasterState.slopeScaledDepthBias = 2.0f;
        pd.renderState.rasterState.depthBiasClamp = 0.01f;
        var.pipeline = cache.GetOrCreatePipeline(pipeName, pd, fbInfo, nvDevice);
    };

    {
        constexpr u32 s = 24;
        nvrhi::VertexAttributeDesc a[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGBA16_SNORM).setOffset(0).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(8).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(12).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("BINORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(16).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG16_SNORM).setOffset(20).setElementStride(s),
        };
        initVariant(state.skNonHQ, "shadow_skinned", "ShadowSk_NonHQ", a, 5);
    }
    {
        constexpr u32 s = 36;
        nvrhi::VertexAttributeDesc a[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(0).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(16).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(20).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("BINORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(24).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(28).setElementStride(s),
        };
        initVariant(state.skHQ1W, "shadow_skinned_hq", "ShadowSk_HQ1W", a, 5);
    }
    {
        constexpr u32 s = 44;
        nvrhi::VertexAttributeDesc a[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(0).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(16).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(20).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("BINORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(24).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(28).setElementStride(s),
        };
        initVariant(state.skHQ2W, "shadow_skinned_2w", "ShadowSk_HQ2W", a, 5);
    }
    {
        constexpr u32 s = 44;
        nvrhi::VertexAttributeDesc a[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(0).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(16).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(20).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("BINORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(24).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(28).setElementStride(s),
        };
        initVariant(state.skHQ3W, "shadow_skinned_3w", "ShadowSk_HQ3W", a, 5);
    }
    {
        constexpr u32 s = 40;
        nvrhi::VertexAttributeDesc a[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(0).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(16).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(20).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("BINORMAL").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(24).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(28).setElementStride(s),
            nvrhi::VertexAttributeDesc().setName("BLENDINDICES").setFormat(nvrhi::Format::BGRA8_UNORM).setOffset(36).setElementStride(s),
        };
        initVariant(state.skHQ4W, "shadow_skinned_4w", "ShadowSk_HQ4W", a, 6);
    }

    state.skinnedInitialized = true;
    Msg("* [ShadowPass] Skinned pipelines initialized");
}

enum {
    SK_RM_SKINNING_2B = 5, SK_RM_SKINNING_2B_HQ = 6,
    SK_RM_SKINNING_3B = 7, SK_RM_SKINNING_3B_HQ = 8,
};

static nvrhi::IGraphicsPipeline* SelectSkinnedShadowPipeline(
    const ShadowPassState& state, u32 stride, u16 renderMode)
{
    if (stride == 24) return state.skNonHQ.pipeline.Get();
    if (stride == 36) return state.skHQ1W.pipeline.Get();
    if (stride == 40) return state.skHQ4W.pipeline.Get();
    if (stride == 44) {
        if (renderMode == SK_RM_SKINNING_3B || renderMode == SK_RM_SKINNING_3B_HQ)
            return state.skHQ3W.pipeline.Get();
        return state.skHQ2W.pipeline.Get();
    }
    return nullptr;
}

static u32 GetShadowSkeletonBoneOffset(
    nvrhi::ICommandList* cmdList,
    GPUCullingManager& gpuCullMgr,
    const GeometryBatch& batch)
{
    CKinematics* parent = nullptr;
    u32 visualType = batch.visual ? batch.visual->getType() : 0;

    if (visualType == MT_SKELETON_GEOMDEF_ST)
        parent = static_cast<CSkeletonX_ST*>(batch.visual)->GetParent();
    else if (visualType == MT_SKELETON_GEOMDEF_PM)
        parent = static_cast<CSkeletonX_PM*>(batch.visual)->GetParent();

    if (!parent)
        return 0;

    return gpuCullMgr.GetOrUploadSkeleton(cmdList, parent);
}

struct ShadowPassData
{
    framegraph::VirtualResourceHandle shadowArray;
    u32 cascadeCount;
    u32 smapSize;
    fg::RenderDevice* device;
    shadow::ShadowManager* shadows;
    const GeometryCollector* geometry;
    ShadowPassState* passState;
};

ShadowPassResources setupShadowPasses(
    framegraph::FrameGraph& fg,
    shadow::ShadowManager& shadows,
    RenderDevice* device,
    const GeometryCollector* geometry,
    ShadowPassState* state,
    const xr_vector<GeometryBatch>* hudBatches)
{
    ShadowPassResources resources;

    if (!shadows.IsActive())
        return resources;

    const auto& config = shadows.GetConfig();
    resources.sunCascadeCount = config.cascadeCount;

    nvrhi::FramebufferInfoEx fbInfo;
    fbInfo.depthFormat = nvrhi::Format::D32;
    InitializeShadowResources(device, fbInfo, *state);
    InitializeSkinnedShadowResources(device, fbInfo, *state);
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
            data.geometry = geometry;
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

                u32 terrainCount = gpuCulling->GetTerrainObjectCount();
                if (terrainCount > 0 && gpuCulling->GetTerrainCompactDrawArgsBuffer())
                {
                    framegraph::BindingSetBuilder terrBsb(*vsRefl, *psRefl, nvDevice, "ShadowPass.Terrain");
                    terrBsb.ConstantBuffer("ShadowViewCB", shadowCBBuffer);
                    terrBsb.BufferSRV("g_Materials", matBuffer.GetBuffer());
                    terrBsb.BufferSRV("g_InstanceData", gpuCulling->GetTerrainInstanceBuffer());
                    terrBsb.BufferSRV("g_CompactBatchIndices", gpuCulling->GetTerrainCompactBatchIndicesBuffer());
                    terrBsb.BufferSRV("g_CompactMaterialIDs", gpuCulling->GetTerrainCompactMaterialIDBuffer());

                    auto terrBindingSet = cache.GetOrCreateBindingSet(
                        terrBsb.Build(), data.passState->shadowBindingLayout, nvDevice);

                    gfxState.bindings = { terrBindingSet };
                    if (bindlessTable)
                        gfxState.addBindingSet(bindlessTable);
                    gfxState.indirectParams = gpuCulling->GetTerrainCompactDrawArgsBuffer();

                    cmdList->setGraphicsState(gfxState);
                    DrawIndexedIndirectCountOrFallback(
                        cmdList, 0, 0, terrainCount);
                }

                if (data.passState->skinnedInitialized && data.geometry)
                {
                    auto* globalBoneBuffer = gpuCulling->GetGlobalBoneBuffer();
                    if (globalBoneBuffer)
                    {
                        auto dynTransCB = cache.GetOrCreateVolatileCB(
                            "ShadowPass", "SkinnedDynTrans", sizeof(Fmatrix), data.device, 8192);
                        auto skinnedMatCB = cache.GetOrCreateVolatileCB(
                            "ShadowPass", "SkinnedMat", sizeof(SkinnedMaterialCB), data.device, 8192);

                        for (const auto& batch : data.geometry->GetBatches())
                        {
                            if (!batch.isSkinned || !batch.vertexBuffer || !batch.indexBuffer)
                                continue;

                            auto* skPipeline = SelectSkinnedShadowPipeline(
                                *data.passState, batch.vertexStride, batch.skinningRenderMode);
                            if (!skPipeline)
                                continue;

                            u32 boneOffset = GetShadowSkeletonBoneOffset(cmdList, *gpuCulling, batch);

                            Fmatrix worldMat = batch.worldMatrix;
                            cmdList->writeBuffer(dynTransCB, &worldMat, sizeof(worldMat));

                            SkinnedMaterialCB matCB = {};
                            matCB.materialID = batch.bindlessMaterialID;
                            matCB.skeletonBoneOffset = boneOffset;
                            cmdList->writeBuffer(skinnedMatCB, &matCB, sizeof(matCB));

                            nvrhi::BindingSetDesc skBindDesc;
                            skBindDesc.bindings = {
                                nvrhi::BindingSetItem::ConstantBuffer(0, dynTransCB),
                                nvrhi::BindingSetItem::ConstantBuffer(3, shadowCBBuffer),
                                nvrhi::BindingSetItem::ConstantBuffer(4, skinnedMatCB),
                                nvrhi::BindingSetItem::StructuredBuffer_SRV(3, globalBoneBuffer),
                                nvrhi::BindingSetItem::StructuredBuffer_SRV(8, matBuffer.GetBuffer()),
                                nvrhi::BindingSetItem::Sampler(0, cache.GetAnisoWrapSampler(nvDevice)),
                            };
                            auto skBindingSet = cache.GetOrCreateBindingSet(
                                skBindDesc, data.passState->skinnedBindingLayout, nvDevice);

                            nvrhi::GraphicsState skState;
                            skState.pipeline = skPipeline;
                            skState.framebuffer = framebuffer;
                            skState.vertexBuffers = {{ batch.vertexBuffer, 0, 0 }};
                            skState.indexBuffer = { batch.indexBuffer, nvrhi::Format::R16_UINT, 0 };
                            skState.viewport.addViewport(smapViewport);
                            skState.viewport.addScissorRect(nvrhi::Rect(data.smapSize, data.smapSize));
                            skState.bindings = { skBindingSet };
                            if (bindlessTable)
                                skState.addBindingSet(bindlessTable);
                            skState.indirectParams = nullptr;

                            cmdList->setGraphicsState(skState);
                            cmdList->drawIndexed(nvrhi::DrawArguments()
                                .setVertexCount(batch.indexCount)
                                .setStartIndexLocation(batch.startIndex)
                                .setStartVertexLocation(batch.baseVertex));
                        }
                    }
                }
            }
        }
    );

    if (hudBatches && !hudBatches->empty() && shadows.HasHudShadow() && state->skinnedInitialized)
    {
        static constexpr u32 hudSmapSize = 2048;

        framegraph::ResourceDesc hudDesc;
        hudDesc.type = framegraph::ResourceDesc::Type::Texture2D;
        hudDesc.width = hudSmapSize;
        hudDesc.height = hudSmapSize;
        hudDesc.format = nvrhi::Format::D32;
        hudDesc.isDepthStencil = true;
        hudDesc.isTransient = true;
        hudDesc.debugName = "rt_HUDShadowMap";

        auto hudShadowHandle = fg.CreateTexture("rt_HUDShadowMap", hudDesc);

        struct HudShadowPassData
        {
            framegraph::VirtualResourceHandle hudShadowMap;
            u32 smapSize;
            Fmatrix lightVP;
            fg::RenderDevice* device;
            const xr_vector<GeometryBatch>* hudBatches;
            ShadowPassState* passState;
        };

        auto& hudData = fg.addCallbackPass<HudShadowPassData>(
            "HUDShadowPass",
            [&, hudShadowHandle](
                framegraph::FrameGraph& builder,
                framegraph::PassHandle passHandle,
                HudShadowPassData& data)
            {
                framegraph::RenderPassBuilder passBuilder(builder, passHandle);
                data.hudShadowMap = passBuilder.write(hudShadowHandle,
                    framegraph::ResourceState::DepthStencilWrite);
                data.smapSize = hudSmapSize;
                data.lightVP = shadows.GetHudView().viewProjection;
                data.device = device;
                data.hudBatches = hudBatches;
                data.passState = state;
            },
            [](const HudShadowPassData& data, const framegraph::FrameGraph& fg,
               fg::RenderContext* ctx)
            {
                if (!data.hudBatches || data.hudBatches->empty())
                    return;

                nvrhi::IDevice* nvDevice = data.device->GetNVRHIDevice();
                nvrhi::ICommandList* cmdList = ctx->GetCommandList();

                nvrhi::ITexture* hudSmapTex = fg.GetPhysicalTexture(data.hudShadowMap);

                auto& cache = framegraph::GetPassResourceCache();

                nvrhi::FramebufferDesc fbDesc;
                fbDesc.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(hudSmapTex));
                auto framebuffer = cache.GetOrCreateFramebuffer("HUDShadow", fbDesc, nvDevice);

                cmdList->clearDepthStencilTexture(hudSmapTex, nvrhi::AllSubresources, true, 1.0f, false, 0);

                auto* gpuCulling = xray::render::fg::RImplementation.GetGPUCullingManager();
                auto* globalBoneBuffer = gpuCulling ? gpuCulling->GetGlobalBoneBuffer() : nullptr;
                if (!globalBoneBuffer)
                    return;

                auto& matBuffer = fg::bindless::MaterialBuffer::Instance();

                ShadowViewCB shadowCB;
                shadowCB.lightVP = data.lightVP;
                shadowCB.cascadeIndex = 0;
                shadowCB.smapSize = static_cast<float>(data.smapSize);
                shadowCB.padding[0] = 0;
                shadowCB.padding[1] = 0;

                auto shadowCBBuffer = cache.GetOrCreateVolatileCB(
                    "HUDShadow", "ShadowViewCB", sizeof(ShadowViewCB), data.device, 8);
                cmdList->writeBuffer(shadowCBBuffer, &shadowCB, sizeof(shadowCB));

                auto dynTransCB = cache.GetOrCreateVolatileCB(
                    "HUDShadow", "DynTrans", sizeof(Fmatrix), data.device, 512);
                auto skinnedMatCB = cache.GetOrCreateVolatileCB(
                    "HUDShadow", "SkinnedMat", sizeof(SkinnedMaterialCB), data.device, 512);

                float smapSizeF = static_cast<float>(data.smapSize);
                nvrhi::Viewport smapViewport(0.0f, smapSizeF, 0.0f, smapSizeF, 0.0f, 1.0f);

                auto* backend = data.device->GetBackend();
                nvrhi::IBindingSet* bindlessTable = backend ? backend->GetBindlessDescriptorTable() : nullptr;

                for (const auto& batch : *data.hudBatches)
                {
                    if (!batch.isSkinned || !batch.vertexBuffer || !batch.indexBuffer)
                        continue;

                    auto* skPipeline = SelectSkinnedShadowPipeline(
                        *data.passState, batch.vertexStride, batch.skinningRenderMode);
                    if (!skPipeline)
                        continue;

                    u32 boneOffset = GetShadowSkeletonBoneOffset(cmdList, *gpuCulling, batch);

                    Fmatrix worldMat = batch.worldMatrix;
                    cmdList->writeBuffer(dynTransCB, &worldMat, sizeof(worldMat));

                    SkinnedMaterialCB matCB = {};
                    matCB.materialID = batch.bindlessMaterialID;
                    matCB.skeletonBoneOffset = boneOffset;
                    cmdList->writeBuffer(skinnedMatCB, &matCB, sizeof(matCB));

                    nvrhi::BindingSetDesc skBindDesc;
                    skBindDesc.bindings = {
                        nvrhi::BindingSetItem::ConstantBuffer(0, dynTransCB),
                        nvrhi::BindingSetItem::ConstantBuffer(3, shadowCBBuffer),
                        nvrhi::BindingSetItem::ConstantBuffer(4, skinnedMatCB),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, globalBoneBuffer),
                        nvrhi::BindingSetItem::StructuredBuffer_SRV(8, matBuffer.GetBuffer()),
                        nvrhi::BindingSetItem::Sampler(0, cache.GetAnisoWrapSampler(nvDevice)),
                    };
                    auto skBindingSet = cache.GetOrCreateBindingSet(
                        skBindDesc, data.passState->skinnedBindingLayout, nvDevice);

                    nvrhi::GraphicsState skState;
                    skState.pipeline = skPipeline;
                    skState.framebuffer = framebuffer;
                    skState.vertexBuffers = {{ batch.vertexBuffer, 0, 0 }};
                    skState.indexBuffer = { batch.indexBuffer, nvrhi::Format::R16_UINT, 0 };
                    skState.viewport.addViewport(smapViewport);
                    skState.viewport.addScissorRect(nvrhi::Rect(data.smapSize, data.smapSize));
                    skState.bindings = { skBindingSet };
                    if (bindlessTable)
                        skState.addBindingSet(bindlessTable);
                    skState.indirectParams = nullptr;

                    cmdList->setGraphicsState(skState);
                    cmdList->drawIndexed(nvrhi::DrawArguments()
                        .setVertexCount(batch.indexCount)
                        .setStartIndexLocation(batch.startIndex)
                        .setStartVertexLocation(batch.baseVertex));
                }
            }
        );
        resources.hudShadowMap = hudData.hudShadowMap;
    }

    return resources;
}

}
