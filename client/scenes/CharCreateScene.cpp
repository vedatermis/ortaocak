#include "client/scenes/CharCreateScene.h"

#include "client/net/GameClient.h"
#include "client/scenes/CharSelectScene.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/SceneWidgets.h"
#include "client/scenes/Validation.h"
#include "client/ui/Ui.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Names.h"
#include "shared/net/Messages.h"

#include <format>

namespace vo::client::scenes {

namespace anchor = ui::anchor;
namespace vn = vo::net;

namespace {

constexpr float kSideMargin = 32.0f;
constexpr float kTop = 84.0f;
constexpr float kBottom = 80.0f;
constexpr float kPanelWidth = 400.0f;
const Color kErrorText = Color::hex(0xE58A72);

constexpr std::uint8_t kAydor = static_cast<std::uint8_t>(game::Race::Aydor);
constexpr std::uint8_t kKargan = static_cast<std::uint8_t>(game::Race::Kargan);

struct RaceText
{
    std::uint8_t race;
    const char* key; // widget name suffix
    const char* tagline;
    const char* shortLine;
    const char* lore;
};

constexpr RaceText kRaces[2] = {
    {kAydor, "aydor", "İnsan krallığı · ovaların ve beyaz kalelerin halkı", "ovaların insan krallığı",
     "Aydor, verimli ovalar ve beyaz taştan kaleler üzerine kurulmuş köklü bir insan krallığıdır. Halkı "
     "yemine, yazılı yasaya ve ortak sofraya bağlıdır. Sancaklarındaki soluk güneş, uzun kışların ardından "
     "doğan ilk sabahı anlatır."},
    {kKargan, "kargan", "Dağ halkı · ocak-kentlerin iri yapılı ustaları", "iri yapılı dağ halkı",
     "Karganlar, sarp dağların içine oyulmuş ocak-kentlerde yaşayan iri yapılı bir halktır. Taşı ve demiri "
     "herkesten iyi işler, verdikleri sözü kayaya kazınmış gibi tutarlar. Bir Kargan için en büyük şeref, "
     "klanının ocağını sönmeden yeni kuşağa devretmektir."},
};

struct ClassText
{
    game::CharClass cls;
    const char* description;
};

constexpr ClassText kClasses[4] = {
    {game::CharClass::Warrior, "Ağır zırh ve kalkanla ön safta durur, düşmanın darbelerini üstüne çeker."},
    {game::CharClass::Rogue, "Gölgelerde hızla hareket eder, rakibin zayıf noktasını bulup ani vurur."},
    {game::CharClass::Mage, "Ateşi ve buzu uzaktan yönlendirir; kırılgandır ama gücü yıkıcıdır."},
    {game::CharClass::Priest, "Dualar ve şifalı otlarla yaraları kapatır, dostlarını kötülükten korur."},
};

const RaceText& raceText(std::uint8_t race)
{
    return race == kKargan ? kRaces[1] : kRaces[0];
}

} // namespace

CharCreateScene::CharCreateScene(Options options) : m_options(options) {}
CharCreateScene::~CharCreateScene() = default;

void CharCreateScene::build()
{
    const ui::Theme& t = theme();
    ui::Widget& root = ui().root();

    m_header = &root.add<ui::Label>("", ui::TextRole::Heading);
    m_header->setFontSize(28.0f).setWeight(render::FontWeight::Bold).setColor(t.textTitle).setShadow(true);
    m_header->setAlign(render::HAlign::Center, render::VAlign::Middle);
    m_header->setAnchors(anchor::kFillX | anchor::kTop).setMargins({160.0f, 6.0f, 160.0f, 0.0f}).setSize(0.0f, 38.0f);
    m_header->setName("create.header");
    m_subheader = &root.add<ui::Label>("", ui::TextRole::Small);
    m_subheader->setColor(t.textDim).setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Top);
    m_subheader->setAnchors(anchor::kFillX | anchor::kTop).setMargins({160.0f, 43.0f, 160.0f, 0.0f}).setSize(0.0f, 18.0f);

    auto& account = root.add<ui::Label>("Hesap: " + ctx().net.account().username, ui::TextRole::Small);
    account.setColor(t.textDim).setAlign(render::HAlign::Right);
    account.setAnchors(anchor::kTopRight).setMargins({0.0f, 26.0f, kSideMargin, 0.0f}).setSize(150.0f, 20.0f);

    buildRaceStep(root);
    buildDetailsStep(root);
    showStep(Step::Race);
}

void CharCreateScene::buildRaceStep(ui::Widget& root)
{
    m_raceStep = &root.add<ui::Panel>(ui::Panel::Style::None);
    m_raceStep->setAnchors(anchor::kFill);

    auto& row = m_raceStep->add<CenteredRow>(420.0f, 520.0f, 24.0f);
    row.setAnchors(anchor::kFill).setMargins({kSideMargin, kTop, kSideMargin, kBottom});
    for (std::size_t i = 0; i < 2; ++i)
    {
        const RaceText& rt = kRaces[i];
        auto& card = row.add<RaceCard>(ctx().resources, rt.race, rt.tagline, rt.lore);
        card.setName(std::string("create.race.") + rt.key);
        const std::uint8_t race = rt.race;
        card.setOnSelect([this, race]() { selectRace(race); });
        card.setOnActivate(
            [this, race]()
            {
                selectRace(race);
                showStep(Step::Details);
            });
        m_raceCards[i] = &card;
    }

    auto& back = m_raceStep->add<ui::Button>("Geri", [this]() { this->back(); });
    back.setAnchors(anchor::kBottomLeft).setMargins({kSideMargin, 0.0f, 0.0f, 20.0f}).setSize(160.0f, 40.0f);
    back.setName("create.race.back");
    m_next = &m_raceStep->add<ui::Button>("İleri", [this]() { showStep(Step::Details); });
    m_next->setAnchors(anchor::kBottomRight).setMargins({0.0f, 0.0f, kSideMargin, 16.0f}).setSize(220.0f, 46.0f);
    m_next->setName("create.next");
    m_next->setEnabled(false);
}

void CharCreateScene::buildDetailsStep(ui::Widget& root)
{
    const ui::Theme& t = theme();
    m_detailsStep = &root.add<ui::Panel>(ui::Panel::Style::None);
    m_detailsStep->setAnchors(anchor::kFill);

    m_preview = &m_detailsStep->add<CharacterPreview>(ctx().resources);
    m_preview->setAnchors(anchor::kFill).setMargins({kSideMargin, kTop, kSideMargin + kPanelWidth + 20.0f, kBottom});
    m_preview->setName("create.preview");

    auto& panel = m_detailsStep->add<ui::Panel>(ui::Panel::Style::Window, "Karakter Bilgileri");
    panel.setAnchors(anchor::kRight | anchor::kFillY).setMargins({0.0f, kTop, kSideMargin, kBottom}).setSize(kPanelWidth, 0.0f);
    panel.setPadding({16.0f, 12.0f, 16.0f, 12.0f});
    auto& form = panel.add<ui::StackPanel>(ui::Orientation::Vertical, 6.0f);
    form.setAnchors(anchor::kFill);

    m_raceLine = &form.add<ui::Label>("", ui::TextRole::Normal);
    m_raceLine->setSize(0.0f, 22.0f);
    m_raceLine->setName("create.raceline");

    form.add<ui::Label>("Sınıf", ui::TextRole::Small).setColor(t.textDim).setSize(0.0f, 16.0f);
    for (int rowIndex = 0; rowIndex < 2; ++rowIndex)
    {
        auto& row = form.add<ui::StackPanel>(ui::Orientation::Horizontal, 8.0f);
        row.setSize(0.0f, t.controlHeight);
        for (int col = 0; col < 2; ++col)
        {
            const std::size_t i = static_cast<std::size_t>(rowIndex * 2 + col);
            const auto cls = static_cast<std::uint8_t>(kClasses[i].cls);
            auto& b = row.add<ToggleButton>(std::string(game::className(kClasses[i].cls)),
                                            [this, cls]() { selectClass(cls); });
            b.setName(std::format("create.class.{}", cls));
            m_classButtons[i] = &b;
        }
    }
    m_classDesc = &form.add<ui::Label>("", ui::TextRole::Small);
    m_classDesc->setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top);
    m_classDesc->setSize(0.0f, 34.0f);
    m_classDesc->setName("create.classdesc");

    form.add<ui::Label>("Görünüm", ui::TextRole::Small).setColor(t.textDim).setSize(0.0f, 16.0f);
    auto& looks = form.add<ui::StackPanel>(ui::Orientation::Horizontal, 4.0f);
    looks.setSize(0.0f, 32.0f);
    auto addPicker = [&](const char* key, ui::Label*& label, auto onStep)
    {
        auto& prev = looks.add<ChevronButton>(ChevronButton::Direction::Left, [onStep]() { onStep(-1); });
        prev.setSize(32.0f, 0.0f);
        prev.setName(std::format("create.{}.prev", key));
        label = &looks.add<ui::Label>("", ui::TextRole::Normal);
        label->setAlign(render::HAlign::Center);
        label->setName(std::format("create.{}", key));
        auto& next = looks.add<ChevronButton>(ChevronButton::Direction::Right, [onStep]() { onStep(1); });
        next.setSize(32.0f, 0.0f);
        next.setName(std::format("create.{}.next", key));
    };
    addPicker("face", m_faceLabel, [this](int d) { stepFace(d); });
    looks.add<ui::Widget>().setSize(14.0f, 0.0f); // gap between the pickers
    addPicker("hair", m_hairLabel, [this](int d) { stepHair(d); });

    form.add<ui::Label>("İsim", ui::TextRole::Small).setColor(t.textDim).setSize(0.0f, 16.0f);
    m_name = &form.add<ui::TextBox>();
    m_name->setPlaceholder("karakterinin adı")
        .setMaxLength(game::kCharacterNameMaxLength)
        .setOnChange([this](const std::string&) { onNameChanged(); })
        .setOnSubmit([this](const std::string&) { submit(); });
    m_name->setSize(0.0f, t.controlHeight);
    m_name->setName("create.name");
    m_hints = &form.add<NameHints>();
    m_hints->setSize(0.0f, 20.0f);
    m_hints->setName("create.hints");

    auto& statusRow = form.add<ui::Widget>();
    statusRow.setSize(0.0f, 0.0f); // flexible
    m_spinner = &statusRow.add<Spinner>();
    m_spinner->setAnchors(anchor::kTopLeft).setMargins({0.0f, 4.0f, 0.0f, 0.0f}).setSize(20.0f, 20.0f);
    m_spinner->setVisible(false);
    m_status = &statusRow.add<ui::Label>("", ui::TextRole::Small);
    m_status->setWrap(true).setAlign(render::HAlign::Left, render::VAlign::Top).setColor(t.textDim);
    m_status->setAnchors(anchor::kFill).setMargins({0.0f, 6.0f, 0.0f, 0.0f});
    m_status->setName("create.status");

    auto& back = m_detailsStep->add<ui::Button>("Geri", [this]() { this->back(); });
    back.setAnchors(anchor::kBottomLeft).setMargins({kSideMargin, 0.0f, 0.0f, 20.0f}).setSize(160.0f, 40.0f);
    back.setName("create.back");
    m_create = &m_detailsStep->add<ui::Button>("Yarat", [this]() { submit(); });
    m_create->setAnchors(anchor::kBottomRight).setMargins({0.0f, 0.0f, kSideMargin, 16.0f}).setSize(kPanelWidth, 46.0f);
    m_create->setName("create.submit");

    selectClass(m_class);
    stepFace(0);
    stepHair(0);
    onNameChanged();
}

void CharCreateScene::extraStates(std::vector<std::string>& out) const
{
    out.emplace_back(m_step == Step::Race ? "race" : "details");
    if (m_race != 0)
        out.emplace_back(std::string("race.") + raceText(m_race).key);
}

void CharCreateScene::showStep(Step step)
{
    if (step == Step::Details && m_race == 0)
        return;
    m_step = step;
    m_raceStep->setVisible(step == Step::Race);
    m_detailsStep->setVisible(step == Step::Details);
    if (step == Step::Race)
    {
        m_header->setText(m_options.firstCharacter ? "İlk Karakterini Yarat · Irk Seçimi" : "Irk Seçimi");
        m_subheader->setText("Karakterinin halkını seç. Irk, karakter yaratıldıktan sonra değiştirilemez.");
        if (m_race != 0)
            ui().setFocus(m_raceCards[m_race == kKargan ? 1 : 0]);
    }
    else
    {
        m_header->setText("Karakterini Oluştur");
        m_subheader->setText("Sınıfını ve görünümünü seç, karakterine bir isim ver.");
        const RaceText& rt = raceText(m_race);
        m_raceLine->setText(std::format("Irk: {} — {}", game::raceName(static_cast<game::Race>(m_race)), rt.shortLine));
        updatePreview();
        ui().setFocus(m_name);
    }
}

void CharCreateScene::selectRace(std::uint8_t race)
{
    m_race = race;
    for (RaceCard* card : m_raceCards)
        card->setSelected(card->race() == race);
    m_next->setEnabled(true);
    updatePreview();
}

void CharCreateScene::selectClass(std::uint8_t cls)
{
    m_class = cls;
    for (std::size_t i = 0; i < m_classButtons.size(); ++i)
    {
        const bool on = static_cast<std::uint8_t>(kClasses[i].cls) == cls;
        m_classButtons[i]->setSelected(on);
        if (on)
            m_classDesc->setText(kClasses[i].description);
    }
    updatePreview();
}

void CharCreateScene::stepFace(int delta)
{
    const int count = game::kMaxFace + 1;
    m_face = static_cast<std::uint8_t>((m_face + delta + count) % count);
    m_faceLabel->setText(std::format("Yüz {}/{}", m_face + 1, count));
    updatePreview();
}

void CharCreateScene::stepHair(int delta)
{
    const int count = game::kMaxHair + 1;
    m_hair = static_cast<std::uint8_t>((m_hair + delta + count) % count);
    m_hairLabel->setText(std::format("Saç {}/{}", m_hair + 1, count));
    updatePreview();
}

void CharCreateScene::onNameChanged()
{
    m_hints->setCheck(checkCharacterName(m_name->text()));
    if (!m_creating && m_status != nullptr)
        setStatus(""); // an earlier error refers to the old name
    updatePreview();
}

void CharCreateScene::updatePreview()
{
    if (m_preview == nullptr || m_name == nullptr)
        return;
    if (m_race == 0)
    {
        m_preview->setLook(std::nullopt);
        return;
    }
    CharacterLook look;
    look.race = m_race;
    look.charClass = m_class;
    look.face = m_face;
    look.hair = m_hair;
    look.name = m_name->text();
    look.level = game::kStartLevel;
    m_preview->setLook(look);
}

void CharCreateScene::setStatus(std::string_view text, bool error)
{
    m_status->setText(text);
    m_status->setColor(error ? kErrorText : theme().textDim);
}

void CharCreateScene::submit()
{
    if (m_creating || leaving() || m_step != Step::Details)
        return;
    const std::string name = m_name->text();
    const CharacterNameCheck check = checkCharacterName(name);
    if (!check.valid)
    {
        setStatus(check.message, true);
        ui().setFocus(m_name);
        showMessage({"İsim Geçersiz", check.message});
        return;
    }
    vn::CharCreateReq req;
    req.name = name;
    req.race = m_race;
    req.charClass = m_class;
    req.face = m_face;
    req.hair = m_hair;
    VO_LOG_INFO("charcreate: creating '{}' (race {}, class {}, face {}, hair {})", name, m_race, m_class, m_face,
                m_hair);
    if (!ctx().net.request(req, vn::Opcode::CharCreateRes))
    {
        returnToLogin(net::disconnectMessage({}));
        return;
    }
    m_creating = true;
    m_spinner->setVisible(true);
    m_status->setMargins({28.0f, 6.0f, 0.0f, 0.0f});
    setStatus("Karakter yaratılıyor…");
    m_create->setEnabled(false);
    m_name->setEnabled(false);
}

void CharCreateScene::back()
{
    if (m_creating || leaving())
        return;
    if (m_step == Step::Details)
    {
        showStep(Step::Race);
        return;
    }
    CharSelectScene::Options opts; // no auto-jump back here, even if the account is still empty
    changeScene(std::make_unique<CharSelectScene>(opts));
}

void CharCreateScene::onUnhandledKey(const platform::InputEvent& e)
{
    if (e.repeat)
        return;
    if (e.key == platform::keys::kEscape)
    {
        back();
        return;
    }
    if (m_step != Step::Race)
        return;
    if (e.key == platform::keys::kLeft || e.key == platform::keys::kRight)
    {
        selectRace(e.key == platform::keys::kLeft ? kAydor : kKargan);
        ui().setFocus(m_raceCards[m_race == kKargan ? 1 : 0]);
    }
    else if (e.key == platform::keys::kEnter && m_race != 0)
    {
        showStep(Step::Details);
    }
}

void CharCreateScene::onNetEvent(const net::NetEvent& e)
{
    if (e.type != net::NetEventType::Message)
    {
        UiScene::onNetEvent(e);
        return;
    }
    if (!e.is(vn::Opcode::CharCreateRes) || !m_creating)
        return;
    vn::CharCreateRes res;
    if (!vn::decodeMessage(e.payload, res))
    {
        returnToLogin(net::connectFailureMessage(vn::ConnectResult::ProtocolError));
        return;
    }
    m_creating = false;
    m_spinner->setVisible(false);
    m_status->setMargins({0.0f, 6.0f, 0.0f, 0.0f});
    m_create->setEnabled(true);
    m_name->setEnabled(true);
    if (res.result == vn::CharCreateResult::Ok)
    {
        VO_LOG_INFO("charcreate: created {} '{}'", res.character.charId, res.character.name);
        net::AccountState& account = ctx().net.account();
        account.characters.push_back(res.character);
        CharSelectScene::Options opts;
        opts.selectCharId = res.character.charId;
        opts.status = std::format("«{}» yaratıldı.", res.character.name);
        changeScene(std::make_unique<CharSelectScene>(opts));
        return;
    }
    VO_LOG_INFO("charcreate: rejected (result {})", static_cast<int>(res.result));
    const std::string msg = net::charCreateResultMessage(res.result);
    setStatus(msg, true);
    ui().setFocus(m_name);
    showMessage({"Karakter Yaratılamadı", msg});
}

} // namespace vo::client::scenes
