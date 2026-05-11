#pragma once

#include "xrCore/xrCore.h"
#include "xrCore/_matrix.h"
#include "xrCore/_vector3d.h"
#include "xrCommon/xr_vector.h"
#include "Layers/xrRender/FrameGraph/FGTypes.h"

namespace xray::render {
struct GeometryBatch;
}

namespace xray::render::shadow {

enum class ShadowViewKind : u8
{
    DirectionalCascade,
    Spot,
    PointFace,
    HudDirectional,
};

struct ShadowViewId
{
    u32 index = UINT32_MAX;
};

struct ShadowTextureSlice
{
    framegraph::VirtualResourceHandle texture;
    u32 arraySlice = 0;
    u32 cubeFace = 0;
};

struct ShadowView
{
    ShadowViewKind kind = ShadowViewKind::DirectionalCascade;

    u32 lightIndex = 0;
    u32 cascadeIndex = 0;
    u32 faceIndex = 0;

    Fmatrix view;
    Fmatrix projection;
    Fmatrix viewProjection;

    Fmatrix worldToShadowTex;
    Fvector frustumCornersWS[8] = {};
    Fbox frustumBoundsWS;
    float texelSize = 0.0f;
    float nearDistance = 0.0f;
    float farDistance = 0.0f;

    ShadowTextureSlice target;
    xr_vector<xray::render::GeometryBatch*> visibleCasters;

    string64 debugName = {};
};

static constexpr u32 MAX_SUN_CASCADES = 4;

struct CascadeShadowConfig
{
    u32 cascadeCount = 4;
    float minimumDistance = 0.1f;
    float firstCascadeFar = 10.0f;
    float maximumDistance = 150.0f;
    float overlapProportion = 0.2f;
    u32 shadowMapSize = 2048;
};

inline void BuildExponentialCascadeBounds(
    const CascadeShadowConfig& config,
    xr_vector<float>& outFarBounds)
{
    outFarBounds.clear();
    outFarBounds.reserve(config.cascadeCount);

    if (config.cascadeCount == 1)
    {
        outFarBounds.push_back(config.maximumDistance);
        return;
    }

    const float base = powf(
        config.maximumDistance / config.firstCascadeFar,
        1.0f / float(config.cascadeCount - 1));

    for (u32 i = 0; i < config.cascadeCount; ++i)
        outFarBounds.push_back(config.firstCascadeFar * powf(base, float(i)));
}

struct GpuShadowCascade
{
    Fmatrix worldToShadowClip;
    Fmatrix worldToShadowTex;
    float farBound = 0.0f;
    float texelSize = 0.0f;
    float _pad0 = 0.0f;
    float _pad1 = 0.0f;
};

struct GpuSunShadowData
{
    GpuShadowCascade cascades[MAX_SUN_CASCADES];
    Fvector4 lightDirWS;
    Fvector4 shadowParams;
    Fvector4 textureParams;
    Fmatrix hudWorldToShadowTex;
};

}
