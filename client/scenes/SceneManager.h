// Holds the active scene. Transitions requested during a frame (e.g. from a button callback) are
// applied at the start of the next frame, so a scene is never destroyed while its code runs.
#pragma once

#include "client/scenes/Scene.h"

#include <memory>

namespace vo::client::scenes {

class SceneManager
{
public:
    SceneManager() = default;
    ~SceneManager();
    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    // Replaces the current scene at the next applyPending().
    void requestChange(std::unique_ptr<Scene> next);
    [[nodiscard]] bool hasPending() const noexcept { return m_pending != nullptr; }

    // Runs onExit/onEnter/onResize for a pending transition. Call at the start of a frame.
    void applyPending(SceneContext& ctx);

    void update(double dt);
    void render();
    void resize(float width, float height, float uiScale);
    // Exits and destroys the current (and any pending) scene.
    void shutdown();

    [[nodiscard]] Scene* current() const noexcept { return m_current.get(); }

private:
    std::unique_ptr<Scene> m_current;
    std::unique_ptr<Scene> m_pending;
};

} // namespace vo::client::scenes
