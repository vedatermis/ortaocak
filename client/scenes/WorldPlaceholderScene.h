// Stand-in for the world (replaced in M6): shows the character name, entity id, spawn position and
// the live RTT, plus "Karakter Seçimine Dön" (LEAVE_WORLD_REQ). World packets that arrive are
// ignored. A dropped connection returns to the login screen with the reason.
#pragma once

#include "client/scenes/UiScene.h"

namespace vo::client::ui {
class Button;
class Label;
} // namespace vo::client::ui

namespace vo::client::scenes {

class WorldPlaceholderScene final : public UiScene
{
public:
    WorldPlaceholderScene();
    ~WorldPlaceholderScene() override;

    [[nodiscard]] const char* name() const noexcept override { return "world"; }

protected:
    void build() override;
    void onUnhandledKey(const platform::InputEvent& e) override;
    void onNetEvent(const net::NetEvent& e) override;
    void onFrame(double dt) override;
    [[nodiscard]] bool isBusy() const override { return m_leavingWorld; }

private:
    void leaveWorld();

    bool m_leavingWorld = false;
    long long m_rttShown = -2;
    ui::Label* m_rtt = nullptr;
    ui::Button* m_back = nullptr;
};

} // namespace vo::client::scenes
