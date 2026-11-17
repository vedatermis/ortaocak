// Base class of the full-screen UI scenes (login, character select/create, world placeholder).
//
// Owns the scene's UiContext and does the per-frame plumbing every screen needs: input routing (UI
// first, then onUnhandledKey), draining GameClient events into onNetEvent, the background, UI
// scale fitting (UiScale.h), message/confirm boxes, scene changes and automation state tokens
// (scene name + busy | modal | idle + extraStates()).
//
// After changeScene() the scene stops processing input and network events, so the events still
// queued go to the next scene.
#pragma once

#include "client/net/NetText.h"
#include "client/platform/InputEvent.h"
#include "client/scenes/Scene.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vo::client::net {
struct NetEvent;
}
namespace vo::client::ui {
class UiContext;
}

namespace vo::client::scenes {

class UiScene : public Scene
{
public:
    UiScene();
    ~UiScene() override;

    void onEnter(SceneContext& ctx) final;
    void onExit() final;
    void update(double dt) final;
    void render() override;
    void onResize(float width, float height, float uiScale) final;

    void automationStates(std::vector<std::string>& out) const final;
    [[nodiscard]] ui::UiContext* automationUi() noexcept final { return m_ui.get(); }

protected:
    // Build the widget tree (ui() and ctx() are valid).
    virtual void build() = 0;
    virtual void teardown() {}
    // KeyDown the UI did not consume (and no modal is open).
    virtual void onUnhandledKey(const platform::InputEvent&) {}
    // Any input event the UI did not consume (and no modal is open), before onUnhandledKey. The
    // world scene routes these to the camera and the character.
    virtual void onUnhandledEvent(const platform::InputEvent&) {}
    // Every input event before the UI sees it; true consumes it (M9: the world takes Tab for
    // targeting, which the UI would otherwise use to move the keyboard focus).
    virtual bool onPreviewEvent(const platform::InputEvent&) { return false; }
    // Default: a Disconnected event returns to the login screen with the reason.
    virtual void onNetEvent(const net::NetEvent& e);
    virtual void onFrame(double) {}
    // True: leave the queued network events where they are this frame (M13 automation: the world
    // scene holds the zone change screen for a screenshot; the events then go on in order).
    [[nodiscard]] virtual bool holdNetEvents() const { return false; }
    // Scene-specific "busy" (a request is in flight etc.).
    [[nodiscard]] virtual bool isBusy() const { return false; }
    virtual void extraStates(std::vector<std::string>&) const {}

    [[nodiscard]] SceneContext& ctx() const noexcept { return *m_ctx; }
    [[nodiscard]] ui::UiContext& ui() const noexcept { return *m_ui; }
    [[nodiscard]] const ui::Theme& theme() const noexcept;
    [[nodiscard]] bool leaving() const noexcept { return m_leaving; }

    void showMessage(const net::UserMessage& message, std::function<void()> onClose = {});
    // Two buttons; onResult(true) for `yes`. Esc = no.
    void confirm(std::string title, std::string text, std::string yes, std::string no,
                 std::function<void(bool)> onResult);
    void changeScene(std::unique_ptr<Scene> next);
    // Drops the connection (if any) and shows the login screen, optionally with a message box.
    void returnToLogin(std::optional<net::UserMessage> notice);

private:
    SceneContext* m_ctx = nullptr;
    std::unique_ptr<ui::UiContext> m_ui;
    bool m_leaving = false;
};

} // namespace vo::client::scenes
