#include "client/scenes/CharCreateScene.h"

#include "client/net/GameClient.h"
#include "client/render/AssetCache.h"
#include "client/scenes/CharSelectScene.h"
#include "client/scenes/CharacterPreview.h"
#include "client/scenes/SceneResources.h"
#include "client/scenes/SceneWidgets.h"
#include "client/scenes/Validation.h"
#include "client/ui/Ui.h"
#include "client/world/ItemData.h"
#include "shared/core/Log.h"
#include "shared/game/Character.h"
#include "shared/game/Names.h"
#include "shared/net/Messages.h"

#include <algorithm>
#include <format>
#include <sstream>

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

// Race and class texts follow docs/LORE.md (the story bible): two peoples, both right and flawed.
struct RaceText
{
    std::uint8_t race;
    const char* key; // widget name suffix
    const char* tagline;
    const char* shortLine;
    const char* lore;
};

constexpr RaceText kRaces[2] = {
    {kAydor, "aydor", "İnsan krallığı · yasanın ve yeminin halkı", "yazılı yasanın ova krallığı",
     "Güney ovalarının halkı: beyaz taştan kaleler, sulanan tarlalar, seçilmiş bir kral. Aydorlu için en "
     "büyük erdem, sözünü yazıya döküp ona uymaktır; yemin, mühür ve tanık olmadan iş yapılmaz. Sancaktaki "
     "soluk güneş, Uzun Kış'tan sonraki ilk sabahtır."},
    {kKargan, "kargan", "Ork soylu dağ halkı · ocak-kentlerin ustaları", "ork soylu dağ halkı",
     "Kuzey Dağları'na oyulmuş ocak-kentlerde yaşarlar: iri, gri-yeşil tenli, alt çenesinden iki kısa diş "
     "çıkan gururlu bir halk. Taşı ve demiri kimse onlar kadar iyi işleyemez. Krala değil ocaklarına "
     "bağlıdırlar; sözleri yazılmaz, taşa kazınır."},
};

struct ClassText
{
    game::CharClass cls;
    const char* aydor;  // the class as Aydor knows it
    const char* kargan; // ... and as the Kargan hearths know it
};

constexpr ClassText kClasses[4] = {
    {game::CharClass::Warrior, "Tanyeri kalkancısı · Kalkanla safı tutar, darbeleri üstüne çeker.",
     "Ocak bekçisi · Kapıyı tek başına tutar, ağır demirle darbeleri karşılar."},
    {game::CharClass::Rogue, "Sınır izcisi · Gölgede hızlı ilerler; yasayla arası pek iyi değildir.",
     "Geçit avcısı · Dağ yollarını ve pusuyu bilir, ani ve sessiz vurur."},
    {game::CharClass::Mage, "Közbilimci · Kırıkların gücünü ölçüp yönlendirir; kırılgan ama yıkıcıdır.",
     "Körükçü · Ateşi ve buzu nefesle, ilahiyle sürer; kırılgan ama yıkıcıdır."},
    {game::CharClass::Priest, "Soluk Güneş rahibi · Yemin tanığı ve hekim; dualarla yaraları kapatır.",
     "Kül anası, kül babası · Yarayı da ocağı da sarar, dostlarını korur."},
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
    // First character only: the Közaran oath (docs/LORE.md "Oyuncu kim?").
    m_oath = &root.add<ui::Label>("Közaran yemini: «Kırıkları bulup yurduma taşıyacağım.»", ui::TextRole::Small);
    m_oath->setColor(t.accent).setShadow(true).setAlign(render::HAlign::Center, render::VAlign::Top);
    m_oath->setAnchors(anchor::kFillX | anchor::kTop).setMargins({kSideMargin, 67.0f, kSideMargin, 0.0f}).setSize(0.0f, 17.0f);
    m_oath->setName("create.oath");
    m_oath->setVisible(false);

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

    // Complete character looks of the race (hidden while the race has only one).
    m_lookRow = &form.add<ui::StackPanel>(ui::Orientation::Horizontal, 4.0f);
    m_lookRow->setSize(0.0f, 32.0f);
    m_lookRow->setName("create.look.row");
    auto& prev = m_lookRow->add<ChevronButton>(ChevronButton::Direction::Left, [this]() { stepLook(-1); });
    prev.setSize(32.0f, 0.0f);
    prev.setName("create.look.prev");
    m_lookLabel = &m_lookRow->add<ui::Label>("", ui::TextRole::Normal);
    m_lookLabel->setAlign(render::HAlign::Center);
    m_lookLabel->setName("create.look");
    auto& next = m_lookRow->add<ChevronButton>(ChevronButton::Direction::Right, [this]() { stepLook(1); });
    next.setSize(32.0f, 0.0f);
    next.setName("create.look.next");

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
    stepLook(0);
    onNameChanged();
}

const world::CharacterLookTable* CharCreateScene::lookTable() const
{
    return ctx().assets != nullptr ? &ctx().assets->looks() : nullptr;
}

int CharCreateScene::lookCount() const
{
    const world::CharacterLookTable* looks = lookTable();
    return looks != nullptr && m_race != 0 ? looks->lookCount(m_race) : 0;
}

void CharCreateScene::stepLook(int delta)
{
    const world::CharacterLookTable* looks = lookTable();
    const int count = lookCount();
    if (looks == nullptr || count == 0)
    {
        m_look = 0;
    }
    else
    {
        const int index = looks->available(m_race, m_look) ? looks->indexOfLook(m_race, m_look) : 0;
        m_look = static_cast<std::uint8_t>(looks->nthAvailable(m_race, index + delta));
    }
    if (m_lookRow != nullptr)
    {
        m_lookRow->setVisible(count > 1);
        const int shown = looks != nullptr && count > 0 ? looks->indexOfLook(m_race, m_look) + 1 : 1;
        m_lookLabel->setText(std::format("Görünüm {}/{}", shown, std::max(count, 1)));
    }
    updatePreview();
}

void CharCreateScene::extraStates(std::vector<std::string>& out) const
{
    out.emplace_back(m_step == Step::Race ? "race" : "details");
    if (m_race != 0)
        out.emplace_back(std::string("race.") + raceText(m_race).key);
    out.emplace_back(std::format("looks{}", lookCount()));
    out.emplace_back(std::format("look{}", m_look));
    out.emplace_back(std::format("class{}", m_class));
}

bool CharCreateScene::automationCommand(std::string_view command, std::string& error)
{
    std::istringstream in{std::string(command)};
    std::string verb;
    in >> verb;
    if (verb == "setlook")
    {
        // setlook <n>: pick look n if the race has it (the selector is hidden with one look);
        // otherwise keep the current one and say so in the log.
        int look = -1;
        if (!(in >> look) || look < 0)
        {
            error = "usage: setlook <n>";
            return false;
        }
        const world::CharacterLookTable* looks = lookTable();
        if (m_race == 0 || looks == nullptr || !looks->available(m_race, look))
        {
            VO_LOG_INFO("charcreate: look {} not available for race {} ({} look(s)); keeping look {}", look, m_race,
                        lookCount(), m_look);
            return true;
        }
        m_look = static_cast<std::uint8_t>(look);
        stepLook(0);
        return true;
    }
    if (verb == "expectpreviewmesh")
    {
        // The 3D preview shows the mesh the look table resolves for the current race and look, and
        // that is look n's own `_n` file whenever it exists.
        return expectPreviewMesh(*m_preview, ctx().assets, m_race, m_look, error);
    }
    if (verb == "expectpreviewequip")
    {
        // expectpreviewequip <main> <off>: the figure holds these templates (the class's kit).
        int main = -1;
        int off = -1;
        if (!(in >> main >> off))
        {
            error = "usage: expectpreviewequip <mainTemplate> <offTemplate>";
            return false;
        }
        return expectPreviewEquip(*m_preview, main, off, error);
    }
    if (verb == "setclass")
    {
        int cls = 0;
        if (!(in >> cls) || !game::isValidClass(static_cast<std::uint8_t>(cls)))
        {
            error = "usage: setclass <1..4>";
            return false;
        }
        selectClass(static_cast<std::uint8_t>(cls));
        return true;
    }
    return UiScene::automationCommand(command, error);
}

void CharCreateScene::showStep(Step step)
{
    if (step == Step::Details && m_race == 0)
        return;
    m_step = step;
    m_raceStep->setVisible(step == Step::Race);
    m_detailsStep->setVisible(step == Step::Details);
    m_oath->setVisible(m_options.firstCharacter && step == Step::Race);
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
        selectClass(m_class); // the class line depends on the race
        stepLook(0);
        ui().setFocus(m_name);
    }
}

void CharCreateScene::selectRace(std::uint8_t race)
{
    m_race = race;
    for (RaceCard* card : m_raceCards)
        card->setSelected(card->race() == race);
    m_next->setEnabled(true);
    stepLook(0); // keeps the look if the race has it, else its first one
}

void CharCreateScene::selectClass(std::uint8_t cls)
{
    m_class = cls;
    for (std::size_t i = 0; i < m_classButtons.size(); ++i)
    {
        const bool on = static_cast<std::uint8_t>(kClasses[i].cls) == cls;
        m_classButtons[i]->setSelected(on);
        if (on)
            m_classDesc->setText(m_race == kKargan ? kClasses[i].kargan : kClasses[i].aydor);
    }
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
    look.look = m_look;
    look.name = m_name->text();
    look.level = game::kStartLevel;
    // The figure holds the class's starter kit (PROTOCOL_ITEMS.md §6): picking a class changes it.
    if (const world::ItemData* items = ctx().resources.itemData())
    {
        const game::VisibleEquipment kit = items->kitOf(m_class);
        look.mainHand = kit.mainHand;
        look.offHand = kit.offHand;
    }
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
    req.face = m_look; // the wire `face` carries the look index; `hair` is unused (always 0)
    req.hair = 0;
    VO_LOG_INFO("charcreate: creating '{}' (race {}, class {}, look {})", name, m_race, m_class, m_look);
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
