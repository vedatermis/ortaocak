#include "client/scenes/SceneManager.h"

#include "shared/core/Log.h"

namespace vo::client::scenes {

SceneManager::~SceneManager()
{
    shutdown();
}

void SceneManager::requestChange(std::unique_ptr<Scene> next)
{
    m_pending = std::move(next);
}

void SceneManager::applyPending(SceneContext& ctx)
{
    if (!m_pending)
        return;
    if (m_current)
    {
        VO_LOG_INFO("scene: leaving '{}'", m_current->name());
        m_current->onExit();
        m_current.reset();
    }
    m_current = std::move(m_pending);
    VO_LOG_INFO("scene: entering '{}'", m_current->name());
    m_current->onEnter(ctx);
    m_current->onResize(ctx.viewportWidth, ctx.viewportHeight, ctx.uiScale);
}

void SceneManager::update(double dt)
{
    if (m_current)
        m_current->update(dt);
}

void SceneManager::render()
{
    if (m_current)
        m_current->render();
}

void SceneManager::resize(float width, float height, float uiScale)
{
    if (m_current)
        m_current->onResize(width, height, uiScale);
}

void SceneManager::shutdown()
{
    m_pending.reset();
    if (m_current)
    {
        m_current->onExit();
        m_current.reset();
    }
}

} // namespace vo::client::scenes
