#include "client/scenes/WorldPlaceholderScene.h"

#include "client/net/GameClient.h"
#include "client/scenes/CharSelectScene.h"
#include "client/scenes/CharacterPreview.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <cmath>
#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

WorldPlaceholderScene::WorldPlaceholderScene() = default;
WorldPlaceholderScene::~WorldPlaceholderScene() = default;

void WorldPlaceholderScene::build()
{
    const ui::Theme& t = theme();
    ui::Widget& root = ui().root();
    const auto& world = ctx().net.account().world;
    const vn::CharSummary character = world ? world->character : vn::CharSummary{};
    const vn::EnterWorldRes enter = world ? world->enter : vn::EnterWorldRes{};

    auto& panel = root.add<ui::Panel>(ui::Panel::Style::Window, "Dünya");
    panel.setAnchors(anchor::kCenter).setSize(520.0f, 360.0f);
    panel.setPadding({24.0f, 16.0f, 24.0f, 20.0f});
    auto& stack = panel.add<ui::StackPanel>(ui::Orientation::Vertical, 6.0f);
    stack.setAnchors(anchor::kFill);

    auto& name = stack.add<ui::Label>(character.name, ui::TextRole::Heading);
    name.setFontSize(34.0f).setWeight(render::FontWeight::Bold).setColor(t.textTitle).setShadow(true);
    name.setAlign(render::HAlign::Center);
    name.setSize(0.0f, 46.0f);
    name.setName("world.name");
    auto& info = stack.add<ui::Label>(describeCharacter(character.race, character.charClass, character.level));
    info.setColor(t.textDim).setAlign(render::HAlign::Center);
    info.setSize(0.0f, 22.0f);

    auto addRow = [&](std::string text, const char* widgetName)
    {
        auto& l = stack.add<ui::Label>(std::move(text));
        l.setAlign(render::HAlign::Center);
        l.setSize(0.0f, 22.0f);
        l.setName(widgetName);
        return &l;
    };
    addRow(std::format("Varlık No: {}", enter.entityId), "world.entity");
    addRow(std::format("Konum: Bölge {} · X {:.1f} · Y {:.1f} · Z {:.1f}", enter.zoneId, enter.x, enter.y, enter.z),
           "world.position");
    addRow(std::format("Hız: yürüme {:.1f} m/sn · koşu {:.1f} m/sn", enter.walkSpeed, enter.runSpeed), "world.speed");
    m_rtt = addRow("Gecikme (RTT): ölçülüyor…", "world.rtt");

    auto& soon = stack.add<ui::Label>("Dünya yakında — (M6)", ui::TextRole::Large);
    soon.setWeight(render::FontWeight::Bold).setColor(t.accent).setAlign(render::HAlign::Center);
    soon.setSize(0.0f, 0.0f); // flexible
    soon.setName("world.soon");

    m_back = &stack.add<ui::Button>("Karakter Seçimine Dön", [this]() { leaveWorld(); });
    m_back->setAnchors(anchor::kCenterX).setSize(260.0f, 40.0f);
    m_back->setName("world.back");
    ui().setFocus(m_back);
    onFrame(0.0);
}

void WorldPlaceholderScene::onFrame(double)
{
    const auto rtt = ctx().net.rttMs();
    const long long shown = rtt ? std::llround(*rtt) : -1;
    if (shown == m_rttShown)
        return;
    m_rttShown = shown;
    m_rtt->setText(rtt ? std::format("Gecikme (RTT): {} ms", shown) : std::string("Gecikme (RTT): ölçülüyor…"));
}

void WorldPlaceholderScene::leaveWorld()
{
    if (m_leavingWorld || leaving())
        return;
    VO_LOG_INFO("world: leaving the world");
    if (!ctx().net.request(vn::LeaveWorldReq{}, vn::Opcode::LeaveWorldRes))
    {
        returnToLogin(net::disconnectMessage({}));
        return;
    }
    m_leavingWorld = true;
    m_back->setEnabled(false);
    m_back->setText("Dönülüyor…");
}

void WorldPlaceholderScene::onUnhandledKey(const platform::InputEvent& e)
{
    if (e.key == platform::keys::kEscape && !e.repeat)
        leaveWorld();
}

void WorldPlaceholderScene::onNetEvent(const net::NetEvent& e)
{
    if (e.type != net::NetEventType::Message)
    {
        UiScene::onNetEvent(e);
        return;
    }
    if (!e.is(vn::Opcode::LeaveWorldRes) || !m_leavingWorld)
        return; // ENTITY_* and other world packets: nothing to show yet
    vn::LeaveWorldRes res;
    if (!vn::decodeMessage(e.payload, res))
    {
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return;
    }
    m_leavingWorld = false;
    if (res.result != vn::kLeaveWorldOk)
    {
        m_back->setEnabled(true);
        m_back->setText("Karakter Seçimine Dön");
        showMessage({"Dünya", net::leaveWorldResultMessage(res.result)});
        return;
    }
    auto& account = ctx().net.account();
    CharSelectScene::Options opts;
    if (account.world)
        opts.selectCharId = account.world->character.charId;
    account.world.reset();
    changeScene(std::make_unique<CharSelectScene>(opts));
}

} // namespace vo::client::scenes
