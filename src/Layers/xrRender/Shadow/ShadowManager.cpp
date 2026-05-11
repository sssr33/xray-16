#include "stdafx.h"
#include "ShadowManager.h"
#include "Layers/xrRender/Geometry/GeometryBatch.h"
#include "xrEngine/device.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"

namespace xray::render::shadow {

static Fvector TransformPoint(const Fmatrix& m, const Fvector& v)
{
    Fvector4 r;
    r.x = v.x * m._11 + v.y * m._21 + v.z * m._31 + m._41;
    r.y = v.x * m._12 + v.y * m._22 + v.z * m._32 + m._42;
    r.z = v.x * m._13 + v.y * m._23 + v.z * m._33 + m._43;
    r.w = v.x * m._14 + v.y * m._24 + v.z * m._34 + m._44;
    const float invW = 1.0f / r.w;
    return { r.x * invW, r.y * invW, r.z * invW };
}

static bool IsShadowCaster(const xray::render::GeometryBatch& batch)
{
    if (!batch.isVisible)
        return false;

    if (batch.IsStrictB2F())
        return false;

    if (!batch.visual)
        return false;

    return true;
}

void ShadowManager::Initialize(fg::RenderDevice* device)
{
    m_device = device;
    BuildExponentialCascadeBounds(m_config, m_cascadeFarBounds);
}

void ShadowManager::Shutdown()
{
    m_views.clear();
    m_cascadeFarBounds.clear();
    m_device = nullptr;
    m_active = false;
}

void ShadowManager::BeginFrame()
{
    m_views.clear();
    m_hudView = {};
    m_active = false;
}

void ShadowManager::BuildSunCascades()
{
    Fvector sunDir;
    if (g_pGamePersistent) {
        sunDir = g_pGamePersistent->Environment().CurrentEnv.sun_dir;
    } else {
        sunDir.set(0.5f, -0.7f, 0.5f);
    }
    sunDir.normalize();

    float sunLuminance = std::abs(sunDir.y);
    if (sunLuminance < 0.01f) {
        m_active = false;
        return;
    }

    Fvector lightRight;
    lightRight.set(1, 0, 0);
    if (_abs(lightRight.dotproduct(sunDir)) > 0.99f)
        lightRight.set(0, 0, 1);

    Fvector lightUp;
    lightUp.crossproduct(sunDir, lightRight).normalize();
    lightRight.crossproduct(lightUp, sunDir).normalize();

    const float camNear = VIEWPORT_NEAR;
    float camFar = 500.0f;
    if (g_pGamePersistent)
        camFar = g_pGamePersistent->Environment().CurrentEnv.far_plane;

    static constexpr Fvector ndcCorners[8] = {
        { -1, -1, 0 }, { +1, -1, 0 }, { +1, +1, 0 }, { -1, +1, 0 },
        { -1, -1, 1 }, { +1, -1, 1 }, { +1, +1, 1 }, { -1, +1, 1 },
    };

    Fmatrix invVP = Device.mInvFullTransform;

    Fvector frustumCorners[8];
    for (u32 i = 0; i < 8; ++i)
        frustumCorners[i] = TransformPoint(invVP, ndcCorners[i]);

    float splits[MAX_SUN_CASCADES + 1];
    splits[0] = camNear;
    for (u32 i = 0; i < m_config.cascadeCount; ++i)
        splits[i + 1] = m_cascadeFarBounds[i];

    m_views.resize(m_config.cascadeCount);

    for (u32 c = 0; c < m_config.cascadeCount; ++c)
    {
        float tNear = (splits[c] - camNear) / (camFar - camNear);
        float tFar = (splits[c + 1] - camNear) / (camFar - camNear);

        Fvector sliceCorners[8];
        for (u32 i = 0; i < 4; ++i)
        {
            Fvector edge;
            edge.sub(frustumCorners[i + 4], frustumCorners[i]);
            sliceCorners[i].mad(frustumCorners[i], edge, tNear);
            sliceCorners[i + 4].mad(frustumCorners[i], edge, tFar);
        }

        auto fit = FitStableCascade(
            sliceCorners,
            sunDir,
            lightUp,
            lightRight,
            float(m_config.shadowMapSize));

        ShadowView& view = m_views[c];
        view.kind = ShadowViewKind::DirectionalCascade;
        view.cascadeIndex = c;
        view.view = fit.lightView;
        view.projection = fit.lightProj;
        view.viewProjection = fit.lightViewProj;
        view.worldToShadowTex = fit.worldToShadowTex;
        view.texelSize = fit.texelSize;
        view.nearDistance = splits[c];
        view.farDistance = splits[c + 1];

        for (u32 i = 0; i < 8; ++i)
            view.frustumCornersWS[i] = sliceCorners[i];

        view.frustumBoundsWS.invalidate();
        for (u32 i = 0; i < 8; ++i)
            view.frustumBoundsWS.modify(sliceCorners[i]);

        xr_sprintf(view.debugName, "SunCascade_%u", c);
    }

    m_gpuData = {};
    m_gpuData.lightDirWS.set(sunDir.x, sunDir.y, sunDir.z, 0.0f);
    m_gpuData.shadowParams.set(
        float(m_config.cascadeCount),
        m_config.overlapProportion,
        0.0005f,
        2.0f);
    m_gpuData.textureParams.set(
        float(m_config.shadowMapSize),
        1.0f / float(m_config.shadowMapSize),
        0.0f,
        1.0f);

    for (u32 c = 0; c < m_config.cascadeCount; ++c)
    {
        const ShadowView& view = m_views[c];
        m_gpuData.cascades[c].worldToShadowClip = view.viewProjection;
        m_gpuData.cascades[c].worldToShadowTex = view.worldToShadowTex;
        m_gpuData.cascades[c].farBound = view.farDistance;
        m_gpuData.cascades[c].texelSize = view.texelSize;
    }

    {
        static constexpr float hudMapSize = 2.0f;
        static constexpr u32 hudSmapSize = 2048;

        Fvector sunPos;
        sunPos.mad(Device.vCameraPosition, sunDir, -50.0f);

        Fmatrix hudView;
        hudView.build_camera_dir(sunPos, sunDir, lightUp);

        Fplane lightPlane;
        lightPlane.build_unit_normal(sunPos, sunDir);
        float dist = lightPlane.classify(Device.vCameraPosition);

        Fmatrix hudProj;
        hudProj.build_projection_ortho(hudMapSize, hudMapSize, 0.1f, dist + sqrtf(2.0f) * hudMapSize);

        m_hudView.kind = ShadowViewKind::HudDirectional;
        m_hudView.view = hudView;
        m_hudView.projection = hudProj;
        m_hudView.viewProjection.mul(hudProj, hudView);

        static const Fmatrix texelAdjust = {
            0.5f,  0.0f,  0.0f,  0.0f,
            0.0f, -0.5f,  0.0f,  0.0f,
            0.0f,  0.0f,  1.0f,  0.0f,
            0.5f,  0.5f,  0.0f,  1.0f
        };
        m_hudView.worldToShadowTex.mul(texelAdjust, m_hudView.viewProjection);
        m_hudView.texelSize = hudMapSize / float(hudSmapSize);
        xr_sprintf(m_hudView.debugName, "HudShadow");

        m_gpuData.hudWorldToShadowTex = m_hudView.worldToShadowTex;
    }

    m_active = true;

    if (!m_loggedOnce)
    {
        Msg("* CSM: %u cascades, splits=[%.1f, %.1f, %.1f, %.1f], mapSize=%u",
            m_config.cascadeCount,
            m_config.cascadeCount > 0 ? m_cascadeFarBounds[0] : 0.f,
            m_config.cascadeCount > 1 ? m_cascadeFarBounds[1] : 0.f,
            m_config.cascadeCount > 2 ? m_cascadeFarBounds[2] : 0.f,
            m_config.cascadeCount > 3 ? m_cascadeFarBounds[3] : 0.f,
            m_config.shadowMapSize);
        m_loggedOnce = true;
    }
}

ShadowManager::CascadeFitResult ShadowManager::FitStableCascade(
    const Fvector sliceCorners[8],
    const Fvector& lightDir,
    const Fvector& lightUp,
    const Fvector& lightRight,
    float shadowMapSize)
{
    Fvector center = { 0, 0, 0 };
    for (u32 i = 0; i < 8; ++i)
        center.add(sliceCorners[i]);
    center.mul(1.0f / 8.0f);

    Fmatrix refLightView;
    refLightView.build_camera_dir(center, lightDir, lightUp);

    Fvector minLS = { FLT_MAX, FLT_MAX, FLT_MAX };
    Fvector maxLS = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    Fvector cornersLS[8];
    for (u32 i = 0; i < 8; ++i)
    {
        cornersLS[i] = TransformPoint(refLightView, sliceCorners[i]);
        minLS.x = std::min(minLS.x, cornersLS[i].x);
        minLS.y = std::min(minLS.y, cornersLS[i].y);
        minLS.z = std::min(minLS.z, cornersLS[i].z);
        maxLS.x = std::max(maxLS.x, cornersLS[i].x);
        maxLS.y = std::max(maxLS.y, cornersLS[i].y);
        maxLS.z = std::max(maxLS.z, cornersLS[i].z);
    }

    const float bodyDiagonal = sliceCorners[0].distance_to(sliceCorners[6]);
    const float farDiagonal = sliceCorners[4].distance_to(sliceCorners[6]);
    const float cascadeDiameter = ceilf(std::max(bodyDiagonal, farDiagonal));
    const float texelSize = cascadeDiameter / shadowMapSize;

    float centerX = 0.5f * (minLS.x + maxLS.x);
    float centerY = 0.5f * (minLS.y + maxLS.y);
    centerX = floorf(centerX / texelSize) * texelSize;
    centerY = floorf(centerY / texelSize) * texelSize;

    float snapDX = centerX - 0.5f * (minLS.x + maxLS.x);
    float snapDY = centerY - 0.5f * (minLS.y + maxLS.y);

    Fvector snappedCenter = center;
    snappedCenter.mad(lightRight, snapDX);
    snappedCenter.mad(lightUp, snapDY);

    float zBackExtend = std::max(500.0f, (maxLS.z - minLS.z) * 2.0f);
    float zNear = minLS.z - zBackExtend;
    float zFar = maxLS.z;

    Fmatrix lightView;
    lightView.build_camera_dir(snappedCenter, lightDir, lightUp);

    Fmatrix lightProj;
    lightProj.build_projection_ortho(cascadeDiameter, cascadeDiameter, zNear, zFar);

    Fmatrix lightViewProj;
    lightViewProj.mul(lightProj, lightView);

    static const Fmatrix texelAdjust = {
        0.5f,  0.0f,  0.0f,  0.0f,
        0.0f, -0.5f,  0.0f,  0.0f,
        0.0f,  0.0f,  1.0f,  0.0f,
        0.5f,  0.5f,  0.0f,  1.0f
    };

    Fmatrix worldToShadowTex;
    worldToShadowTex.mul(texelAdjust, lightViewProj);

    CascadeFitResult result;
    result.lightView = lightView;
    result.lightProj = lightProj;
    result.lightViewProj = lightViewProj;
    result.worldToShadowTex = worldToShadowTex;
    result.texelSize = texelSize;
    return result;
}

void ShadowManager::CullCasters(const GeometryCollector& collector)
{
    for (ShadowView& view : m_views)
    {
        view.visibleCasters.clear();

        CFrustum frustum;
        frustum.CreateFromMatrix(view.viewProjection, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

        for (const xray::render::GeometryBatch& batch : collector.GetBatches())
        {
            if (!IsShadowCaster(batch))
                continue;

            if (!frustum.testSphere_dirty(batch.worldBoundsCenter, batch.worldBoundsRadius))
                continue;

            view.visibleCasters.push_back(const_cast<xray::render::GeometryBatch*>(&batch));
        }
    }
}

}
