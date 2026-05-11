#include "stdafx.h"
#include "ShadowManager.h"

namespace xray::render::shadow {

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
}

void ShadowManager::BeginFrame()
{
    m_views.clear();
}

}
