#pragma once

#include "ShadowTypes.h"

namespace xray::render::fg {
class RenderDevice;
class GeometryCollector;
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

    const CascadeShadowConfig& GetConfig() const { return m_config; }
    const xr_vector<ShadowView>& GetViews() const { return m_views; }
    const ShadowView& GetView(u32 index) const { return m_views[index]; }

private:
    fg::RenderDevice* m_device = nullptr;
    CascadeShadowConfig m_config;
    xr_vector<ShadowView> m_views;
    xr_vector<float> m_cascadeFarBounds;
};

}
