#pragma once

#include "ShadowTypes.h"

namespace xray::render {
class GeometryCollector;
}

namespace xray::render::fg {
class RenderDevice;
}

namespace xray::render::shadow {

class ShadowManager
{
public:
    ShadowManager() = default;
    ~ShadowManager() = default;

    void Initialize(fg::RenderDevice* device);
    void Shutdown();
    void BeginFrame();
    void BuildSunCascades();
    void CullCasters(const GeometryCollector& collector);

    bool IsActive() const { return m_active; }
    bool HasHudShadow() const { return m_hudView.kind == ShadowViewKind::HudDirectional; }
    const CascadeShadowConfig& GetConfig() const { return m_config; }
    const xr_vector<ShadowView>& GetViews() const { return m_views; }
    const ShadowView& GetView(u32 index) const { return m_views[index]; }
    const ShadowView& GetHudView() const { return m_hudView; }
    const GpuSunShadowData& GetGpuData() const { return m_gpuData; }

private:
    struct CascadeFitResult
    {
        Fmatrix lightView;
        Fmatrix lightProj;
        Fmatrix lightViewProj;
        Fmatrix worldToShadowTex;
        float texelSize = 0.0f;
    };

    CascadeFitResult FitStableCascade(
        const Fvector sliceCorners[8],
        const Fvector& lightDir,
        const Fvector& lightUp,
        const Fvector& lightRight,
        float shadowMapSize);

    fg::RenderDevice* m_device = nullptr;
    CascadeShadowConfig m_config;
    xr_vector<ShadowView> m_views;
    xr_vector<float> m_cascadeFarBounds;
    GpuSunShadowData m_gpuData = {};
    ShadowView m_hudView = {};
    bool m_active = false;
    bool m_loggedOnce = false;
};

}
