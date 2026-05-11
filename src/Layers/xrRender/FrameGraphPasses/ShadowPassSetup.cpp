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
#include "Layers/xrRender/FGDetailManager.h"
#include "xrCDB/Frustum.h"

namespace xray::render::fg { extern int ps_r__detail_gpu; extern int ps_r__detail_shadows; }
extern ENGINE_API float ps_r3_grass_lod_close;
extern ENGINE_API float ps_r3_grass_lod_mid;
extern ENGINE_API float ps_r3_grass_wind_displacement;
extern ENGINE_API float ps_r3_grass_interaction_displacement;
extern ENGINE_API float ps_r3_grass_blade_height;

namespace xray::render::fg::passes {

static constexpr u32 NUM_SHADOW_CASCADES = 4;

struct alignas(16) ShadowViewCB
{
    Fmatrix lightVP[NUM_SHADOW_CASCADES];
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

struct alignas(16) GrassShadowCullParams {
    Fvector4 lightFrustumPlanes[6];
    Fvector3 cameraPos;
    float fadeDistanceSqr;
    float lodDistanceCloseSqr;
    float lodDistanceMidSqr;
    u32 totalSlotCount;
    u32 visibleCapacity;
    u32 grassMode;
    u32 visibleDecalCapacity;
    u32 pad0, pad1;
};

static void InitializeGrassShadowResources(
    fg::RenderDevice* device,
    const nvrhi::FramebufferInfoEx& fbInfo,
    ShadowPassState& state)
{
    if (state.grassInitialized)
        return;

    nvrhi::IDevice* nvDevice = device->GetNVRHIDevice();
    auto* shaderLoader = GEnv.Render->GetShaderLoader();
    auto& cache = framegraph::GetPassResourceCache();
    auto* backend = device->GetBackend();
    nvrhi::IBindingLayout* bindlessLayout = backend ? backend->GetBindlessLayout() : nullptr;

    auto csResult = shaderLoader->LoadComputeShader("detail_shadow_cull", "main");
    if (!csResult.handle) {
        Msg("! [ShadowPass] Failed to load detail_shadow_cull.cs");
        state.grassInitialized = true;
        return;
    }

    state.grassShadowCullLayout = cache.GetOrCreateBindingLayoutFromReflection(
        "ShadowGrassCull", *csResult.reflection, nvDevice);

    nvrhi::ComputePipelineDesc cpDesc;
    cpDesc.CS = csResult.handle;
    cpDesc.bindingLayouts = { state.grassShadowCullLayout };
    state.grassShadowCullPipeline = nvDevice->createComputePipeline(cpDesc);

    for (u32 c = 0; c < ShadowPassState::NUM_SHADOW_CASCADES; c++)
    {
        auto& cb = state.grassShadowCascades[c];
        for (u32 lod = 0; lod < ShadowPassState::NUM_GRASS_LODS; lod++)
        {
            nvrhi::BufferDesc visDesc;
            visDesc.byteSize = ShadowPassState::MAX_SHADOW_GRASS_INSTANCES * sizeof(u32);
            visDesc.structStride = sizeof(u32);
            visDesc.debugName = "GrassShadowVisible";
            visDesc.canHaveUAVs = true;
            visDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            visDesc.keepInitialState = true;
            cb.visibleBuffer[lod] = nvDevice->createBuffer(visDesc);

            nvrhi::BufferDesc argsDesc;
            argsDesc.byteSize = 20;
            argsDesc.debugName = "GrassShadowDrawArgs";
            argsDesc.canHaveUAVs = true;
            argsDesc.canHaveRawViews = true;
            argsDesc.isDrawIndirectArgs = true;
            argsDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            argsDesc.keepInitialState = true;
            cb.drawArgsBuffer[lod] = nvDevice->createBuffer(argsDesc);
        }

        nvrhi::BufferDesc visDecalDesc;
        visDecalDesc.byteSize = ShadowPassState::MAX_SHADOW_GRASS_INSTANCES * sizeof(u32);
        visDecalDesc.structStride = sizeof(u32);
        visDecalDesc.debugName = "GrassShadowVisibleDecal";
        visDecalDesc.canHaveUAVs = true;
        visDecalDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        visDecalDesc.keepInitialState = true;
        cb.visibleDecalBuffer = nvDevice->createBuffer(visDecalDesc);

        nvrhi::BufferDesc decalArgsDesc;
        decalArgsDesc.byteSize = 20;
        decalArgsDesc.debugName = "GrassShadowDecalDrawArgs";
        decalArgsDesc.canHaveUAVs = true;
        decalArgsDesc.canHaveRawViews = true;
        decalArgsDesc.isDrawIndirectArgs = true;
        decalArgsDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        decalArgsDesc.keepInitialState = true;
        cb.decalDrawArgsBuffer = nvDevice->createBuffer(decalArgsDesc);
    }

    auto grassVsResult = shaderLoader->LoadVertexShader("detail_shadow_gpu", "main");
    auto grassPsResult = shaderLoader->LoadPixelShader("depth_prepass", "main");
    if (grassVsResult.handle && grassPsResult.handle) {
        state.grassShadowVS = grassVsResult.handle;
        state.grassShadowBindingLayout = cache.GetOrCreateBindingLayoutFromReflection(
            "ShadowGrass", *grassVsResult.reflection, *grassPsResult.reflection, nvDevice);

        nvrhi::VertexAttributeDesc grassAttribs[] = {
            nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setOffset(0).setElementStride(28),
            nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(12).setElementStride(28),
            nvrhi::VertexAttributeDesc().setName("COLOR0").setFormat(nvrhi::Format::R32_FLOAT).setOffset(20).setElementStride(28),
            nvrhi::VertexAttributeDesc().setName("COLOR1").setFormat(nvrhi::Format::R32_FLOAT).setOffset(24).setElementStride(28),
        };
        state.grassShadowInputLayout = nvDevice->createInputLayout(grassAttribs, 4, state.grassShadowVS);

        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = state.grassShadowVS;
        pd.PS = grassPsResult.handle;
        pd.inputLayout = state.grassShadowInputLayout;
        if (bindlessLayout)
            pd.bindingLayouts = { state.grassShadowBindingLayout, bindlessLayout };
        else
            pd.bindingLayouts = { state.grassShadowBindingLayout };
        pd.primType = nvrhi::PrimitiveType::TriangleList;
        pd.renderState.depthStencilState.depthTestEnable = true;
        pd.renderState.depthStencilState.depthWriteEnable = true;
        pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pd.renderState.rasterState.depthBias = 50;
        pd.renderState.rasterState.slopeScaledDepthBias = 1.0f;
        state.grassShadowPipeline = cache.GetOrCreatePipeline("ShadowGrassGPU", pd, fbInfo, nvDevice);
    }

    auto bbVsResult = shaderLoader->LoadVertexShader("detail_shadow_billboard", "main");
    auto bbPsResult = shaderLoader->LoadPixelShader("detail_shadow_billboard", "main");
    if (bbVsResult.handle && bbPsResult.handle) {
        state.billboardShadowVS = bbVsResult.handle;
        state.billboardShadowPS = bbPsResult.handle;
        state.billboardShadowBindingLayout = cache.GetOrCreateBindingLayoutFromReflection(
            "ShadowBillboard", *bbVsResult.reflection, *bbPsResult.reflection, nvDevice);

        nvrhi::GraphicsPipelineDesc pd;
        pd.VS = state.billboardShadowVS;
        pd.PS = state.billboardShadowPS;
        if (bindlessLayout)
            pd.bindingLayouts = { state.billboardShadowBindingLayout, bindlessLayout };
        else
            pd.bindingLayouts = { state.billboardShadowBindingLayout };
        pd.primType = nvrhi::PrimitiveType::TriangleList;
        pd.renderState.depthStencilState.depthTestEnable = true;
        pd.renderState.depthStencilState.depthWriteEnable = true;
        pd.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::LessOrEqual;
        pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
        pd.renderState.rasterState.depthBias = 50;
        pd.renderState.rasterState.slopeScaledDepthBias = 1.0f;
        state.billboardShadowPipeline = cache.GetOrCreatePipeline("ShadowBillboard", pd, fbInfo, nvDevice);
    }

    state.grassInitialized = true;
    Msg("* [ShadowPass] Grass shadow pipelines initialized");
}

static void CullGrassForShadow(
    nvrhi::ICommandList* cmdList,
    fg::RenderDevice* renderDevice,
    const ShadowPassState& passState,
    fg::FGDetailManager* dm,
    u32 cascadeIndex,
    const Fmatrix& lightVP)
{
    if (!passState.grassShadowCullPipeline || cascadeIndex >= ShadowPassState::NUM_SHADOW_CASCADES)
        return;

    auto& cascade = passState.grassShadowCascades[cascadeIndex];
    nvrhi::IDevice* nvDevice = renderDevice->GetNVRHIDevice();
    auto& cache = framegraph::GetPassResourceCache();

    Fmatrix lightVPCopy = lightVP;
    CFrustum lightFrustum;
    lightFrustum.CreateFromMatrix(lightVPCopy, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

    float fadeDistance = g_pGamePersistent ? g_pGamePersistent->Environment().CurrentEnv.far_plane : 500.0f;

    GrassShadowCullParams params = {};
    u32 planeCount = std::min<u32>((u32)lightFrustum.p_count, 6);
    for (u32 i = 0; i < 6; i++) {
        if (i < planeCount)
            params.lightFrustumPlanes[i].set(lightFrustum.planes[i].n.x, lightFrustum.planes[i].n.y, lightFrustum.planes[i].n.z, lightFrustum.planes[i].d);
        else
            params.lightFrustumPlanes[i].set(0, 0, 0, 1000000.0f);
    }
    params.cameraPos = Device.vCameraPosition;
    params.fadeDistanceSqr = fadeDistance * fadeDistance;
    params.lodDistanceCloseSqr = ps_r3_grass_lod_close * ps_r3_grass_lod_close;
    params.lodDistanceMidSqr = ps_r3_grass_lod_mid * ps_r3_grass_lod_mid;
    params.totalSlotCount = dm->slot_count;
    params.visibleCapacity = ShadowPassState::MAX_SHADOW_GRASS_INSTANCES;
    params.grassMode = ps_r__detail_gpu ? 1u : 0u;
    params.visibleDecalCapacity = ShadowPassState::MAX_SHADOW_GRASS_INSTANCES;

    auto cullParamsCB = cache.GetOrCreateVolatileCB(
        "GrassShadowCull", "Params", sizeof(GrassShadowCullParams), renderDevice, 16);
    cmdList->writeBuffer(cullParamsCB, &params, sizeof(params));

    bool billboardMode = !ps_r__detail_gpu;
    for (u32 lod = 0; lod < ShadowPassState::NUM_GRASS_LODS; lod++) {
        u32 indexCount = billboardMode ? dm->maxPulledIndexCount : dm->bladeIndexCount[lod];
        u32 drawArgs[5] = { indexCount, 0, 0, 0, 0 };
        cmdList->writeBuffer(cascade.drawArgsBuffer[lod], drawArgs, sizeof(drawArgs));
    }
    {
        u32 decalArgs[5] = { dm->maxPulledIndexCount, 0, 0, 0, 0 };
        cmdList->writeBuffer(cascade.decalDrawArgsBuffer, decalArgs, sizeof(decalArgs));
    }

    auto* csRefl = GEnv.Render->GetShaderLoader()->GetCachedReflection("detail_shadow_cull", ".cs");
    if (!csRefl) return;

    framegraph::BindingSetBuilder bsb(*csRefl, nvDevice, "GrassShadowCull");
    bsb.ConstantBuffer("GrassShadowCullParams", cullParamsCB);
    bsb.BufferSRV("g_slot_aabbs", dm->slotAABBBuffer);
    bsb.BufferSRV("g_all_instances", dm->generatedInstancesBuffer);
    bsb.BufferSRV("g_detail_models", dm->detailModelsBuffer);
    bsb.BufferUAV("g_visible_lod0", cascade.visibleBuffer[0]);
    bsb.BufferUAV("g_indirect_args_lod0", cascade.drawArgsBuffer[0]);
    bsb.BufferUAV("g_visible_lod1", cascade.visibleBuffer[1]);
    bsb.BufferUAV("g_indirect_args_lod1", cascade.drawArgsBuffer[1]);
    bsb.BufferUAV("g_visible_lod2", cascade.visibleBuffer[2]);
    bsb.BufferUAV("g_indirect_args_lod2", cascade.drawArgsBuffer[2]);
    bsb.BufferUAV("g_visible_decals", cascade.visibleDecalBuffer);
    bsb.BufferUAV("g_indirect_args_decal", cascade.decalDrawArgsBuffer);

    auto bindingSet = cache.GetOrCreateBindingSet(
        bsb.Build(), passState.grassShadowCullLayout, nvDevice);

    nvrhi::ComputeState cs;
    cs.pipeline = passState.grassShadowCullPipeline;
    cs.bindings = { bindingSet };
    cmdList->setComputeState(cs);
    cmdList->dispatch((dm->slot_count + 255) / 256, 1, 1);
}

static void RenderGrassShadows(
    nvrhi::ICommandList* cmdList,
    fg::RenderDevice* renderDevice,
    nvrhi::IFramebuffer* framebuffer,
    nvrhi::IBuffer* shadowCBBuffer,
    nvrhi::IBuffer* detailGlobalsCB,
    const ShadowPassState& passState,
    fg::FGDetailManager* dm,
    u32 cascadeIndex,
    u32 smapSize)
{
    if (cascadeIndex >= ShadowPassState::NUM_SHADOW_CASCADES)
        return;

    auto& cascade = passState.grassShadowCascades[cascadeIndex];
    nvrhi::IDevice* nvDevice = renderDevice->GetNVRHIDevice();
    auto& cache = framegraph::GetPassResourceCache();
    auto* backend = renderDevice->GetBackend();
    nvrhi::IBindingSet* bindlessTable = backend ? backend->GetBindlessDescriptorTable() : nullptr;

    float smapSizeF = static_cast<float>(smapSize);
    nvrhi::Viewport smapViewport(0.0f, smapSizeF, 0.0f, smapSizeF, 0.0f, 1.0f);

    bool billboardMode = !ps_r__detail_gpu;

    if (!billboardMode && passState.grassShadowPipeline && dm->bladeVertexBuffer[0] && dm->bladeIndexBuffer[0])
    {
        auto* vsRefl = GEnv.Render->GetShaderLoader()->GetCachedReflection("detail_shadow_gpu", ".vs");
        auto* psRefl = GEnv.Render->GetShaderLoader()->GetCachedReflection("depth_prepass", ".ps");
        if (vsRefl && psRefl)
        {
            for (u32 lod = 0; lod < ShadowPassState::NUM_GRASS_LODS; lod++)
            {
                if (!dm->bladeVertexBuffer[lod] || !dm->bladeIndexBuffer[lod])
                    continue;

                framegraph::BindingSetBuilder bsb(*vsRefl, *psRefl, nvDevice, "ShadowGrass");
                bsb.ConstantBuffer("ShadowViewCB", shadowCBBuffer);
                bsb.ConstantBuffer("DetailGlobals", detailGlobalsCB);
                bsb.Texture("g_Perlin4D", dm->perlin4dTexture);
                bsb.BufferSRV("visible_indices", cascade.visibleBuffer[lod]);
                bsb.BufferSRV("all_instances", dm->generatedInstancesBuffer);

                auto bindingSet = cache.GetOrCreateBindingSet(
                    bsb.Build(), passState.grassShadowBindingLayout, nvDevice);

                nvrhi::GraphicsState gfxState;
                gfxState.pipeline = passState.grassShadowPipeline;
                gfxState.framebuffer = framebuffer;
                gfxState.vertexBuffers = {{ dm->bladeVertexBuffer[lod], 0, 0 }};
                gfxState.indexBuffer = { dm->bladeIndexBuffer[lod], nvrhi::Format::R32_UINT, 0 };
                gfxState.viewport.addViewport(smapViewport);
                gfxState.viewport.addScissorRect(nvrhi::Rect(smapSize, smapSize));
                gfxState.bindings = { bindingSet };
                if (bindlessTable) gfxState.addBindingSet(bindlessTable);
                gfxState.indirectParams = cascade.drawArgsBuffer[lod];

                cmdList->setGraphicsState(gfxState);
                cmdList->drawIndexedIndirect(0);
            }
        }
    }

    if (passState.billboardShadowPipeline && dm->pulledVertexBuffer)
    {
        auto* vsRefl = GEnv.Render->GetShaderLoader()->GetCachedReflection("detail_shadow_billboard", ".vs");
        auto* psRefl = GEnv.Render->GetShaderLoader()->GetCachedReflection("detail_shadow_billboard", ".ps");
        if (vsRefl && psRefl)
        {
            auto drawBillboardSet = [&](nvrhi::IBuffer* visBuffer, nvrhi::IBuffer* argsBuffer) {
                framegraph::BindingSetBuilder bsb(*vsRefl, *psRefl, nvDevice, "ShadowBillboard");
                bsb.ConstantBuffer("ShadowViewCB", shadowCBBuffer);
                bsb.ConstantBuffer("DetailGlobals", detailGlobalsCB);
                bsb.Texture("g_Perlin4D", dm->perlin4dTexture);
                bsb.BufferSRV("visible_indices", visBuffer);
                bsb.BufferSRV("detail_models", dm->detailModelsBuffer);
                bsb.BufferSRV("pulled_vertices", dm->pulledVertexBuffer);
                bsb.BufferSRV("all_instances", dm->generatedInstancesBuffer);

                auto bindingSet = cache.GetOrCreateBindingSet(
                    bsb.Build(), passState.billboardShadowBindingLayout, nvDevice);

                nvrhi::GraphicsState gfxState;
                gfxState.pipeline = passState.billboardShadowPipeline;
                gfxState.framebuffer = framebuffer;
                gfxState.viewport.addViewport(smapViewport);
                gfxState.viewport.addScissorRect(nvrhi::Rect(smapSize, smapSize));
                gfxState.bindings = { bindingSet };
                if (bindlessTable) gfxState.addBindingSet(bindlessTable);
                gfxState.indirectParams = argsBuffer;

                cmdList->setGraphicsState(gfxState);
                cmdList->drawIndirect(0);
            };

            drawBillboardSet(cascade.visibleDecalBuffer, cascade.decalDrawArgsBuffer);

            if (billboardMode) {
                for (u32 lod = 0; lod < ShadowPassState::NUM_GRASS_LODS; lod++)
                    drawBillboardSet(cascade.visibleBuffer[lod], cascade.drawArgsBuffer[lod]);
            }
        }
    }
}

struct alignas(16) ShadowObjectCullParams {
    Fvector4 cascadeFrustumPlanes[6 * NUM_SHADOW_CASCADES];
    u32 objectCount;
    u32 numCascades;
    u32 pad[2];
};

static void InitializeObjectShadowCullResources(
    fg::RenderDevice* device,
    ShadowPassState& state)
{
    if (state.objectCullInitialized)
        return;

    nvrhi::IDevice* nvDevice = device->GetNVRHIDevice();
    auto* shaderLoader = GEnv.Render->GetShaderLoader();
    auto& cache = framegraph::GetPassResourceCache();

    auto csResult = shaderLoader->LoadComputeShader("shadow_object_cull", "main");
    if (!csResult.handle) {
        Msg("! [ShadowPass] Failed to load shadow_object_cull.cs");
        return;
    }

    state.objectShadowCullLayout = cache.GetOrCreateBindingLayoutFromReflection(
        "ShadowObjectCull", *csResult.reflection, nvDevice);

    nvrhi::ComputePipelineDesc cpDesc;
    cpDesc.CS = csResult.handle;
    cpDesc.bindingLayouts = { state.objectShadowCullLayout };
    state.objectShadowCullPipeline = nvDevice->createComputePipeline(cpDesc);

    state.objectCullInitialized = true;
    Msg("* [ShadowPass] Object shadow cull pipeline initialized");
}

static void CullObjectsForShadows(
    nvrhi::ICommandList* cmdList,
    fg::RenderDevice* renderDevice,
    const ShadowPassState& passState,
    nvrhi::IBuffer* objectBuffer,
    nvrhi::IBuffer* shadowDrawArgsBuffer,
    u32 objectCount,
    u32 cascadeCount,
    const Fmatrix cascadeLightVPs[NUM_SHADOW_CASCADES],
    const char* debugName)
{
    if (!passState.objectShadowCullPipeline || !objectBuffer || !shadowDrawArgsBuffer || objectCount == 0)
        return;

    nvrhi::IDevice* nvDevice = renderDevice->GetNVRHIDevice();
    auto& cache = framegraph::GetPassResourceCache();

    ShadowObjectCullParams params = {};
    for (u32 c = 0; c < cascadeCount; c++)
    {
        Fmatrix vp = cascadeLightVPs[c];
        CFrustum frustum;
        frustum.CreateFromMatrix(vp, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
        u32 planeCount = std::min<u32>(static_cast<u32>(frustum.p_count), 6);
        for (u32 i = 0; i < 6; i++)
        {
            u32 idx = c * 6 + i;
            if (i < planeCount)
                params.cascadeFrustumPlanes[idx].set(
                    frustum.planes[i].n.x, frustum.planes[i].n.y,
                    frustum.planes[i].n.z, frustum.planes[i].d);
            else
                params.cascadeFrustumPlanes[idx].set(0, 0, 0, -1000000.0f);
        }
    }
    params.objectCount = objectCount;
    params.numCascades = cascadeCount;

    auto cullParamsCB = cache.GetOrCreateVolatileCB(
        "ShadowObjectCull", debugName, sizeof(ShadowObjectCullParams), renderDevice, 16);
    cmdList->writeBuffer(cullParamsCB, &params, sizeof(params));

    auto* csRefl = GEnv.Render->GetShaderLoader()->GetCachedReflection("shadow_object_cull", ".cs");
    if (!csRefl) return;

    framegraph::BindingSetBuilder bsb(*csRefl, nvDevice, debugName);
    bsb.ConstantBuffer("ShadowCullParams", cullParamsCB);
    bsb.BufferSRV("g_Objects", objectBuffer);
    bsb.BufferUAV("g_ShadowDrawArgs", shadowDrawArgsBuffer);

    auto bindingSet = cache.GetOrCreateBindingSet(
        bsb.Build(), passState.objectShadowCullLayout, nvDevice);

    nvrhi::ComputeState cs;
    cs.pipeline = passState.objectShadowCullPipeline;
    cs.bindings = { bindingSet };
    cmdList->setComputeState(cs);
    cmdList->dispatch((objectCount + 255) / 256, 1, 1);
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
    fg::FGDetailManager* detailManager;
};

ShadowPassResources setupShadowPasses(
    framegraph::FrameGraph& fg,
    shadow::ShadowManager& shadows,
    RenderDevice* device,
    const GeometryCollector* geometry,
    ShadowPassState* state,
    const xr_vector<GeometryBatch>* hudBatches,
    FGDetailManager* detailManager)
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
    InitializeObjectShadowCullResources(device, *state);
    if (ps_r__detail_shadows && detailManager)
        InitializeGrassShadowResources(device, fbInfo, *state);
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
            data.detailManager = detailManager;
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

            ShadowViewCB shadowCB = {};
            for (u32 c = 0; c < data.cascadeCount; ++c)
                shadowCB.lightVP[c] = data.shadows->GetView(c).viewProjection;
            shadowCB.smapSize = static_cast<float>(data.smapSize);

            float smapSizeF = static_cast<float>(data.smapSize);
            nvrhi::Viewport smapViewport(0.0f, smapSizeF, 0.0f, smapSizeF, 0.0f, 1.0f);

            u32 staticCount = gpuCulling->GetStaticObjectCount();

            framegraph::BindingSetBuilder bsb(*vsRefl, *psRefl, nvDevice, "ShadowPass");
            bsb.ConstantBuffer("ShadowViewCB", shadowCBBuffer);
            bsb.BufferSRV("g_Materials", matBuffer.GetBuffer());
            bsb.BufferSRV("g_InstanceData", gpuCulling->GetStaticInstanceBuffer());
            bsb.BufferSRV("g_CompactBatchIndices",
                gpuCulling->GetStaticIdentityBatchIndicesBuffer()
                    ? gpuCulling->GetStaticIdentityBatchIndicesBuffer()
                    : gpuCulling->GetStaticCompactBatchIndicesBuffer());
            bsb.BufferSRV("g_CompactMaterialIDs", gpuCulling->GetStaticMaterialIDBuffer());

            auto staticBindingSet = cache.GetOrCreateBindingSet(
                bsb.Build(), data.passState->shadowBindingLayout, nvDevice);

            if (data.passState->objectCullInitialized)
            {
                Fmatrix cascadeVPs[NUM_SHADOW_CASCADES];
                for (u32 c = 0; c < data.cascadeCount; ++c)
                    cascadeVPs[c] = shadowCB.lightVP[c];

                if (staticCount > 0 && gpuCulling->GetStaticShadowDrawArgsBuffer())
                    CullObjectsForShadows(cmdList, data.device, *data.passState,
                        gpuCulling->GetStaticObjectBuffer(),
                        gpuCulling->GetStaticShadowDrawArgsBuffer(),
                        staticCount, data.cascadeCount, cascadeVPs, "StaticShadowCull");

                u32 terrainCount = gpuCulling->GetTerrainObjectCount();
                if (terrainCount > 0 && gpuCulling->GetTerrainShadowDrawArgsBuffer())
                    CullObjectsForShadows(cmdList, data.device, *data.passState,
                        gpuCulling->GetTerrainObjectBuffer(),
                        gpuCulling->GetTerrainShadowDrawArgsBuffer(),
                        terrainCount, data.cascadeCount, cascadeVPs, "TerrainShadowCull");
            }

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

                shadowCB.cascadeIndex = c;
                cmdList->writeBuffer(shadowCBBuffer, &shadowCB, sizeof(shadowCB));

                if (staticCount > 0 && gpuCulling->GetStaticShadowDrawArgsBuffer())
                {
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
                    gfxState.bindings = { staticBindingSet };
                    if (bindlessTable)
                        gfxState.addBindingSet(bindlessTable);
                    gfxState.indirectParams = gpuCulling->GetStaticShadowDrawArgsBuffer();

                    cmdList->setGraphicsState(gfxState);

                    u32 argsOffset = c * staticCount * sizeof(IndirectDrawArgs);
                    cmdList->drawIndexedIndirect(argsOffset, staticCount);
                }

                u32 terrainCount = gpuCulling->GetTerrainObjectCount();
                if (terrainCount > 0 && gpuCulling->GetTerrainShadowDrawArgsBuffer())
                {
                    framegraph::BindingSetBuilder terrBsb(*vsRefl, *psRefl, nvDevice, "ShadowPass.Terrain");
                    terrBsb.ConstantBuffer("ShadowViewCB", shadowCBBuffer);
                    terrBsb.BufferSRV("g_Materials", matBuffer.GetBuffer());
                    terrBsb.BufferSRV("g_InstanceData", gpuCulling->GetTerrainInstanceBuffer());
                    terrBsb.BufferSRV("g_CompactBatchIndices", gpuCulling->GetTerrainBatchIndicesBuffer());
                    terrBsb.BufferSRV("g_CompactMaterialIDs", gpuCulling->GetTerrainMaterialIDBuffer());

                    auto terrBindingSet = cache.GetOrCreateBindingSet(
                        terrBsb.Build(), data.passState->shadowBindingLayout, nvDevice);

                    nvrhi::GraphicsState terrState;
                    terrState.pipeline = data.passState->shadowPipeline;
                    terrState.framebuffer = framebuffer;
                    terrState.vertexBuffers = {
                        { gpuCulling->GetMegaVertexBuffer(), 0, 0 },
                        { drawIndexBuffer, 1, 0 }
                    };
                    terrState.indexBuffer = { gpuCulling->GetMegaIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
                    terrState.viewport.addViewport(smapViewport);
                    terrState.viewport.addScissorRect(nvrhi::Rect(data.smapSize, data.smapSize));
                    terrState.bindings = { terrBindingSet };
                    if (bindlessTable)
                        terrState.addBindingSet(bindlessTable);
                    terrState.indirectParams = gpuCulling->GetTerrainShadowDrawArgsBuffer();

                    cmdList->setGraphicsState(terrState);

                    u32 terrArgsOffset = c * terrainCount * sizeof(IndirectDrawArgs);
                    cmdList->drawIndexedIndirect(terrArgsOffset, terrainCount);
                }

                if (ps_r__detail_shadows && data.passState->grassInitialized && data.detailManager
                    && data.detailManager->generatedInstancesBuffer && data.detailManager->slot_count > 0)
                {
                    CullGrassForShadow(cmdList, data.device, *data.passState,
                        data.detailManager, c, shadowCB.lightVP[c]);

                    auto* dm = data.detailManager;
                    auto detailGlobalsCB = cache.GetOrCreateVolatileCB(
                        "ShadowPass", "DetailGlobals",
                        sizeof(fg::FGDetailManager::DetailFrameConstants), data.device, 16);

                    float windAngleDeg = 0.0f;
                    if (g_pGamePersistent)
                        windAngleDeg = g_pGamePersistent->Environment().CurrentEnv.wind_direction;

                    fg::FGDetailManager::DetailFrameConstants fc = {};
                    fc.wave.set(1.0f / 5.0f, 1.0f / 7.0f, 1.0f / 3.0f, Device.fTimeGlobal);
                    fc.g_wind_direction.set(windAngleDeg, dm->windSpeed, 0.0f, 0.0f);
                    fc.grass_wind_displacement = ps_r3_grass_wind_displacement;
                    fc.grass_interaction_displacement = ps_r3_grass_interaction_displacement;
                    fc.grass_blade_height = ps_r3_grass_blade_height;
                    fc.buildDetailsIndex = dm->buildDetailsBindlessIndex;
                    fc.buildDetailsPbrIndex = dm->buildDetailsPbrBindlessIndex;
                    cmdList->writeBuffer(detailGlobalsCB, &fc, sizeof(fc));

                    RenderGrassShadows(cmdList, data.device, framebuffer, shadowCBBuffer,
                        detailGlobalsCB, *data.passState, dm, c, data.smapSize);
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
                shadowCB.lightVP[0] = data.lightVP;
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
