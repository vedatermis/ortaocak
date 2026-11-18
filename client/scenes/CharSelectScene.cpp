#include "client/scenes/CharSelectScene.h"

#include "client/net/GameClient.h"
#include "client/scenes/CharCreateScene.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/LoginScene.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/SceneWidgets.h"
#include "client/scenes/TypedConfirmDialog.h"
#include "client/scenes/WorldScene.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/net/Messages.h"

#include <format>
#include <sstream>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

namespace {
constexpr float kSideMargin = 32.0f;
constexpr float kTop = 84.0f;
constexpr float kBottom = 80.0f;
constexpr float kListWidth = 380.0f;
constexpr float kCardHeight = 80.0f;
constexpr float kCardGap = 10.0f;
const Color kErrorText = Color::hex(0xE58A72);
constexpr std::string_view kListLoadingText = "Karakterler yükleniyor…";
} // namespace

CharSelectScene::CharSelectScene(Options options) : m_options(std::move(options)) {}
CharSelectScene::~CharSelectScene() = default;

void CharSelectScene::build()
{
    const ui::Theme& t = theme();
    ui::Widget& root = ui().root();

    auto& header = root.add<ui::Label>("Karakter Seçimi", ui::TextRole::Heading);
    header.setFontSize(28.0f).setWeight(render::FontWeight::Bold).setColor(t.textTitle).setShadow(true);
    header.setAlign(render::HAlign::Center, render::VAlign::Middle);
    header.setAnchors(anchor::kFillX | anchor::kTop).setMargins({240.0f, 14.0f, 240.0f, 0.0f}).setSize(0.0f, 42.0f);

    auto& account = root.add<ui::Label>("Hesap: " + ctx().net.account().username, ui::TextRole::Small);
    account.setColor(t.textDim).setAlign(render::HAlign::Right);
    account.setAnchors(anchor::kTopRight).setMargins({0.0f, 26.0f, kSideMargin, 0.0f}).setSize(220.0f, 20.0f);
    account.setName("select.account");

    // ---- Preview (left, takes the remaining width) ----
    m_preview = &root.add<CharacterPreview>(ctx().resources);
    m_preview->setAnchors(anchor::kFill).setMargins({kSideMargin, kTop, kSideMargin + kListWidth + 20.0f, kBottom});
    m_preview->setEmptyText("Boş yuva — yeni bir karakter yaratın");
    m_preview->setName("select.preview");

    // ---- Character list (right) ----
    m_listPanel = &root.add<ui::Panel>(ui::Panel::Style::Window, "Karakterler");
    m_listPanel->setAnchors(anchor::kRight | anchor::kFillY)
        .setMargins({0.0f, kTop, kSideMargin, kBottom})
        .setSize(kListWidth, 0.0f);
    m_listPanel->setPadding(Insets::all(14.0f));
    for (int i = 0; i < CharacterSlots::kSlotCount; ++i)
    {
        auto& card = m_listPanel->add<CharacterSlotCard>();
        card.setAnchors(anchor::kFillX | anchor::kTop)
            .setMargins({0.0f, static_cast<float>(i) * (kCardHeight + kCardGap), 0.0f, 0.0f})
            .setSize(0.0f, kCardHeight);
        card.setName("select.slot" + std::to_string(i));
        card.setResources(&ctx().resources);
        card.setOnSelect([this, i]() { selectSlot(i); });
        card.setOnActivate([this, i]() { activateSlot(i); });
        m_cards[static_cast<std::size_t>(i)] = &card;
    }
    const float statusTop = static_cast<float>(CharacterSlots::kSlotCount) * (kCardHeight + kCardGap);
    m_spinner = &m_listPanel->add<Spinner>();
    m_spinner->setAnchors(anchor::kTopLeft).setMargins({2.0f, statusTop + 2.0f, 0.0f, 0.0f}).setSize(20.0f, 20.0f);
    m_spinner->setVisible(false);
    m_status = &m_listPanel->add<ui::Label>("", ui::TextRole::Small);
    m_status->setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top).setColor(t.textDim);
    m_status->setAnchors(anchor::kFill).setMargins({0.0f, statusTop + 4.0f, 0.0f, t.controlHeight + 8.0f});
    m_status->setName("select.status");

    auto& row = m_listPanel->add<ui::StackPanel>(ui::Orientation::Horizontal, 10.0f);
    row.setAnchors(anchor::kFillX | anchor::kBottom).setSize(0.0f, t.controlHeight);
    m_create = &row.add<ui::Button>("Karakter Yarat", [this]() { openCreate(); });
    m_create->setName("select.create");
    m_delete = &row.add<ui::Button>("Sil", [this]() { askDelete(); });
    m_delete->setName("select.delete");

    // ---- Bottom bar ----
    m_logout = &root.add<ui::Button>("Çıkış Yap", [this]() { logout(); });
    m_logout->setAnchors(anchor::kBottomLeft).setMargins({kSideMargin, 0.0f, 0.0f, 20.0f}).setSize(160.0f, 40.0f);
    m_logout->setName("select.logout");
    m_enter = &root.add<ui::Button>("Oyuna Gir", [this]() { enterWorld(); });
    m_enter->setAnchors(anchor::kBottomRight).setMargins({0.0f, 0.0f, kSideMargin, 16.0f}).setSize(kListWidth, 46.0f);
    m_enter->setName("select.enter");

    auto& hint = root.add<ui::Label>("↑↓ seç · Enter gir · Del sil · Esc çıkış", ui::TextRole::Small);
    hint.setColor(t.textDim).setAlign(render::HAlign::Center);
    hint.setAnchors(anchor::kFillX | anchor::kBottom)
        .setMargins({kSideMargin + 180.0f, 0.0f, kSideMargin + kListWidth + 20.0f, 30.0f})
        .setSize(0.0f, 20.0f);

    // Show what we already know while the fresh list is on its way.
    if (ctx().net.account().charactersLoaded)
        m_slots.setCharacters(ctx().net.account().characters, m_options.selectCharId);
    refresh();
    requestList();
    if (!m_options.status.empty())
        setStatus(m_options.status);
}

void CharSelectScene::extraStates(std::vector<std::string>& out) const
{
    out.emplace_back(std::format("chars{}", m_slots.count()));
    if (m_listLoaded)
        out.emplace_back("loaded");
    out.emplace_back(std::format("selected{}", m_slots.selectedIndex()));
    // fix1: whether the "Karakterler yükleniyor…" line is shown.
    out.emplace_back(m_status != nullptr && m_status->text() == kListLoadingText ? "listloading" : "nolistloading");
}

bool CharSelectScene::automationCommand(std::string_view command, std::string& error)
{
    if (command == "expectpreviewmesh")
    {
        const vn::CharSummary* c = m_slots.selected();
        if (c == nullptr)
        {
            error = "no character selected";
            return false;
        }
        return expectPreviewMesh(*m_preview, ctx().assets, c->race, c->face, error);
    }
    if (command.starts_with("expectpreviewequip"))
    {
        // expectpreviewequip <main> <off>: the selected character's CharSummary and the figure.
        int main = -1;
        int off = -1;
        std::istringstream in{std::string(command.substr(std::string_view("expectpreviewequip").size()))};
        if (!(in >> main >> off))
        {
            error = "usage: expectpreviewequip <mainTemplate> <offTemplate>";
            return false;
        }
        const vn::CharSummary* c = m_slots.selected();
        if (c == nullptr || c->mainHandTemplate != main || c->offHandTemplate != off)
        {
            error = c == nullptr ? "no character selected"
                                 : std::format("CharSummary lists {} / {}, expected {} / {}", c->mainHandTemplate,
                                               c->offHandTemplate, main, off);
            return false;
        }
        return expectPreviewEquip(*m_preview, main, off, error);
    }
    return UiScene::automationCommand(command, error);
}

void CharSelectScene::setStatus(std::string_view text, bool error)
{
    m_status->setText(text);
    m_status->setColor(error ? kErrorText : theme().textDim);
}

void CharSelectScene::setPending(Pending pending, std::string_view status)
{
    m_pending = pending;
    m_spinner->setVisible(pending != Pending::None);
    m_status->setMargins({pending != Pending::None ? 28.0f : 0.0f,
                          static_cast<float>(CharacterSlots::kSlotCount) * (kCardHeight + kCardGap) + 4.0f, 0.0f,
                          theme().controlHeight + 8.0f});
    if (!status.empty() || pending != Pending::None)
        setStatus(status);
    refresh();
}

void CharSelectScene::requestList()
{
    if (!ctx().net.request(vn::CharListReq{}, vn::Opcode::CharListRes))
    {
        returnToLogin(net::disconnectMessage({}));
        return;
    }
    setPending(Pending::List, kListLoadingText);
}

void CharSelectScene::syncAccount()
{
    net::AccountState& account = ctx().net.account();
    account.characters = m_slots.characters();
    account.charactersLoaded = true;
}

void CharSelectScene::refresh()
{
    const bool busy = m_pending != Pending::None;
    for (int i = 0; i < CharacterSlots::kSlotCount; ++i)
    {
        CharacterSlotCard* card = m_cards[static_cast<std::size_t>(i)];
        card->setCharacter(m_slots.slot(i));
        card->setSelected(i == m_slots.selectedIndex());
        card->setEnabled(!busy || m_pending == Pending::List);
    }
    m_listPanel->setTitle(std::format("Karakterler ({}/{})", m_slots.count(), CharacterSlots::kSlotCount));

    if (const vn::CharSummary* c = m_slots.selected())
    {
        CharacterLook look;
        look.race = c->race;
        look.charClass = c->charClass;
        look.look = c->face; // the wire `face` carries the look index
        look.name = c->name;
        look.level = c->level;
        look.mainHand = c->mainHandTemplate; // CharSummary v2: the weapons it holds
        look.offHand = c->offHandTemplate;
        m_preview->setLook(look);
    }
    else
    {
        m_preview->setLook(std::nullopt);
    }

    const bool hasChar = m_slots.selected() != nullptr;
    m_enter->setEnabled(!busy && m_listLoaded && hasChar);
    m_delete->setEnabled(!busy && m_listLoaded && hasChar);
    m_create->setEnabled(!busy && m_listLoaded && m_slots.hasFreeSlot());
    m_logout->setEnabled(m_pending != Pending::Enter && m_pending != Pending::Delete);
}

void CharSelectScene::selectSlot(int index)
{
    m_slots.select(index);
    refresh();
}

void CharSelectScene::activateSlot(int index)
{
    selectSlot(index);
    if (m_pending != Pending::None || !m_listLoaded)
        return;
    if (m_slots.selected() != nullptr)
        enterWorld();
    else
        openCreate();
}

void CharSelectScene::enterWorld()
{
    const vn::CharSummary* c = m_slots.selected();
    if (c == nullptr || m_pending != Pending::None || leaving())
        return;
    VO_LOG_INFO("charselect: entering the world with {} '{}'", c->charId, c->name);
    if (!ctx().net.request(vn::EnterWorldReq{c->charId}, vn::Opcode::EnterWorldRes))
    {
        returnToLogin(net::disconnectMessage({}));
        return;
    }
    setPending(Pending::Enter, std::format("{} dünyaya giriyor…", c->name));
}

void CharSelectScene::openCreate()
{
    if (m_pending != Pending::None || leaving())
        return;
    if (!m_slots.hasFreeSlot())
    {
        showMessage({"Karakter Yarat", net::charCreateResultMessage(vn::CharCreateResult::NoFreeSlot)});
        return;
    }
    CharCreateScene::Options opts;
    changeScene(std::make_unique<CharCreateScene>(opts));
}

void CharSelectScene::askDelete()
{
    const vn::CharSummary* c = m_slots.selected();
    if (c == nullptr || m_pending != Pending::None)
        return;
    const std::uint32_t id = c->charId;
    const std::string name = c->name;
    TypedConfirmDesc desc;
    desc.title = "Karakteri Sil";
    desc.text = std::format("«{}» ({}) kalıcı olarak silinecek; bu işlem geri alınamaz ve isim bir daha "
                            "kullanılamaz. Onaylamak için karakterin adını yazın:",
                            name, describeCharacter(c->race, c->charClass, c->level));
    desc.expected = name;
    desc.onClose = [this, id, name](bool confirmed)
    {
        if (confirmed)
            sendDelete(id, name);
    };
    TypedConfirmDialog::show(ui(), std::move(desc));
}

void CharSelectScene::sendDelete(std::uint32_t charId, std::string name)
{
    if (m_pending != Pending::None || leaving())
        return;
    VO_LOG_INFO("charselect: deleting character {} '{}'", charId, name);
    if (!ctx().net.request(vn::CharDeleteReq{charId}, vn::Opcode::CharDeleteRes))
    {
        returnToLogin(net::disconnectMessage({}));
        return;
    }
    m_deletingName = std::move(name);
    setPending(Pending::Delete, std::format("«{}» siliniyor…", m_deletingName));
}

void CharSelectScene::logout()
{
    if (leaving())
        return;
    VO_LOG_INFO("charselect: logout");
    ctx().net.logout();
    changeScene(std::make_unique<LoginScene>());
}

void CharSelectScene::onUnhandledKey(const platform::InputEvent& e)
{
    if (e.repeat && e.key != platform::keys::kUp && e.key != platform::keys::kDown)
        return;
    switch (e.key)
    {
    case platform::keys::kUp:
        if (m_slots.moveSelection(-1))
            refresh();
        ui().setFocus(m_cards[static_cast<std::size_t>(m_slots.selectedIndex())]);
        break;
    case platform::keys::kDown:
        if (m_slots.moveSelection(1))
            refresh();
        ui().setFocus(m_cards[static_cast<std::size_t>(m_slots.selectedIndex())]);
        break;
    case platform::keys::kEnter: activateSlot(m_slots.selectedIndex()); break;
    case platform::keys::kDelete: askDelete(); break;
    case platform::keys::kEscape:
        confirm("Çıkış Yap", "Hesabınızdan çıkış yapıp giriş ekranına dönmek istiyor musunuz?", "Evet", "Hayır",
                [this](bool yes)
                {
                    if (yes)
                        logout();
                });
        break;
    default: break;
    }
}

void CharSelectScene::onNetEvent(const net::NetEvent& e)
{
    if (e.type != net::NetEventType::Message)
    {
        UiScene::onNetEvent(e); // Disconnected -> login screen with the reason
        return;
    }

    if (e.is(vn::Opcode::CharListRes))
    {
        vn::CharListRes res;
        if (!vn::decodeMessage(e.payload, res))
        {
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
            return;
        }
        VO_LOG_INFO("charselect: {} character(s)", res.characters.size());
        m_listLoaded = true;
        m_slots.setCharacters(std::move(res.characters), m_options.selectCharId);
        m_options.selectCharId.reset();
        syncAccount();
        // fix1: the "Karakterler yükleniyor…" line goes with the answer (setPending keeps a status it is
        // given no new text for; it lingered under a loaded list, also on the way back from the world).
        const bool loadingShown = m_status->text() == kListLoadingText;
        if (m_pending == Pending::List)
            setPending(Pending::None, m_slots.count() == 0 ? "Bu hesapta henüz karakter yok." : "");
        else
            refresh();
        if (loadingShown && m_slots.count() > 0)
            setStatus("");
        if (m_slots.count() > 0)
            ui().setFocus(m_cards[static_cast<std::size_t>(m_slots.selectedIndex())]);
        if (m_slots.count() == 0 && m_options.autoCreateWhenEmpty)
        {
            m_options.autoCreateWhenEmpty = false;
            CharCreateScene::Options opts;
            opts.firstCharacter = true;
            changeScene(std::make_unique<CharCreateScene>(opts));
        }
        return;
    }

    if (e.is(vn::Opcode::EnterWorldRes) && m_pending == Pending::Enter)
    {
        vn::EnterWorldRes res;
        if (!vn::decodeMessage(e.payload, res))
        {
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
            return;
        }
        if (res.result == vn::EnterWorldResult::Ok && m_slots.selected() != nullptr)
        {
            ctx().net.account().world = net::WorldEntry{*m_slots.selected(), res};
            m_pending = Pending::None;
            changeScene(std::make_unique<WorldScene>());
            return;
        }
        const std::string msg = net::enterWorldResultMessage(res.result);
        setPending(Pending::None);
        setStatus(msg, true);
        showMessage({"Dünyaya Girilemedi", msg});
        if (res.result == vn::EnterWorldResult::NotFound)
            requestList();
        return;
    }

    if (e.is(vn::Opcode::CharDeleteRes) && m_pending == Pending::Delete)
    {
        vn::CharDeleteRes res;
        if (!vn::decodeMessage(e.payload, res))
        {
            returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
            return;
        }
        if (res.result == vn::CharDeleteResult::Ok)
        {
            m_slots.remove(res.charId);
            syncAccount();
            setPending(Pending::None, std::format("«{}» silindi.", m_deletingName));
            return;
        }
        const std::string msg = net::charDeleteResultMessage(res.result);
        setPending(Pending::None);
        setStatus(msg, true);
        showMessage({"Karakter Silinemedi", msg});
        if (res.result == vn::CharDeleteResult::NotFound)
            requestList();
        return;
    }
    // Anything else (e.g. world packets that are still in flight) is not for this screen.
}

} // namespace vo::client::scenes
