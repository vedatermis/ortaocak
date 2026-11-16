#include "shared/game/Attachments.h"

#include "shared/core/Paths.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

namespace vo::game {

namespace {

constexpr Grip kGrips[] = {Grip::Sword, Grip::Dagger, Grip::Mace, Grip::Staff, Grip::Axe2h, Grip::Shield};

std::optional<Grip> parseGripName(std::string_view s)
{
    for (const Grip g : kGrips)
        if (gripName(g) == s)
            return g;
    return std::nullopt;
}

std::optional<WeaponHand> parseHand(std::string_view s)
{
    if (s == "main")
        return WeaponHand::Main;
    if (s == "off")
        return WeaponHand::Off;
    return std::nullopt;
}

class Problems
{
public:
    explicit Problems(std::string_view source) : m_source(source) {}
    void add(const toml::node* at, const std::string& what)
    {
        if (!m_error.empty())
            return;
        if (at != nullptr && at->source().begin.line > 0)
            m_error = std::format("{}:{}: {}", m_source, at->source().begin.line, what);
        else
            m_error = std::format("{}: {}", m_source, what);
    }
    [[nodiscard]] bool any() const noexcept { return !m_error.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

private:
    std::string m_source;
    std::string m_error;
};

std::optional<double> number(const toml::node& n)
{
    if (const auto* f = n.as_floating_point())
        return f->get();
    if (const auto* i = n.as_integer())
        return static_cast<double>(i->get());
    return std::nullopt;
}

// A list of exactly `count` finite numbers.
bool numbers(const toml::table& t, std::string_view key, std::size_t count, double* out, std::string_view what,
             Problems& p)
{
    const toml::node* n = t.get(key);
    if (n == nullptr)
    {
        p.add(&t, std::format("{}: missing '{}'", what, key));
        return false;
    }
    const toml::array* a = n->as_array();
    if (a == nullptr || a->size() != count)
    {
        p.add(n, std::format("{}: '{}' must be a list of {} numbers", what, key, count));
        return false;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto v = number(*a->get(i));
        if (!v || !std::isfinite(*v))
        {
            p.add(n, std::format("{}: '{}' must be a list of {} finite numbers", what, key, count));
            return false;
        }
        out[i] = *v;
    }
    return true;
}

std::optional<std::string> string(const toml::table& t, std::string_view key, std::string_view what, Problems& p)
{
    const toml::node* n = t.get(key);
    if (n == nullptr)
    {
        p.add(&t, std::format("{}: missing '{}'", what, key));
        return std::nullopt;
    }
    const auto* s = n->as_string();
    if (s == nullptr || s->get().empty())
    {
        p.add(n, std::format("{}: '{}' must be a non-empty string", what, key));
        return std::nullopt;
    }
    return s->get();
}

bool parseRow(const toml::table& t, WeaponAttachment& out, Problems& p)
{
    const std::string what = "[[attach]]";
    for (auto&& [key, value] : t)
    {
        static constexpr std::string_view kKeys[] = {"grip", "look", "hand", "bone", "translation", "rotation", "scale"};
        bool known = false;
        for (const std::string_view k : kKeys)
            known = known || k == key.str();
        if (!known)
        {
            p.add(&value, std::format("{}: unknown key '{}'", what, key.str()));
            return false;
        }
    }
    const auto grip = string(t, "grip", what, p);
    const auto look = string(t, "look", what, p);
    const auto hand = string(t, "hand", what, p);
    const auto bone = string(t, "bone", what, p);
    if (p.any())
        return false;
    const auto g = parseGripName(*grip);
    if (!g)
    {
        p.add(t.get("grip"), std::format("{}: unknown grip '{}'", what, *grip));
        return false;
    }
    const auto h = parseHand(*hand);
    if (!h)
    {
        p.add(t.get("hand"), std::format("{}: hand must be \"main\" or \"off\", not '{}'", what, *hand));
        return false;
    }
    double tr[3]{};
    double q[4]{};
    if (!numbers(t, "translation", 3, tr, what, p) || !numbers(t, "rotation", 4, q, what, p))
        return false;
    if (std::sqrt(tr[0] * tr[0] + tr[1] * tr[1] + tr[2] * tr[2]) >= 10.0)
    {
        p.add(t.get("translation"), std::format("{}: translation is 10 m or more from the bone", what));
        return false;
    }
    const double qlen = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(std::fabs(qlen - 1.0) <= 1e-3))
    {
        p.add(t.get("rotation"), std::format("{}: rotation is not a unit quaternion (length {:.5f})", what, qlen));
        return false;
    }
    const toml::node* sn = t.get("scale");
    const auto s = sn != nullptr ? number(*sn) : std::nullopt;
    if (!s || !(*s > 0.0 && *s <= 10.0))
    {
        p.add(sn != nullptr ? sn : &t, std::format("{}: scale must be a number in (0, 10]", what));
        return false;
    }
    out.grip = *g;
    out.look = *look;
    out.hand = *h;
    out.bone = *bone;
    out.translation = {static_cast<float>(tr[0]), static_cast<float>(tr[1]), static_cast<float>(tr[2])};
    out.rotation = {static_cast<float>(q[0] / qlen), static_cast<float>(q[1] / qlen), static_cast<float>(q[2] / qlen),
                    static_cast<float>(q[3] / qlen)};
    out.scale = static_cast<float>(*s);
    return true;
}

} // namespace

assets::Mat4 WeaponAttachment::matrix() const noexcept
{
    return assets::composeTrs(translation, rotation, {scale, scale, scale});
}

const WeaponAttachment* AttachmentTable::find(Grip grip, std::string_view look, WeaponHand hand) const noexcept
{
    const WeaponAttachment* fallback = nullptr;
    for (const WeaponAttachment& a : m_rows)
    {
        if (a.grip != grip || a.hand != hand)
            continue;
        if (a.look == look)
            return &a;
        if (a.look == kAnyLook)
            fallback = &a;
    }
    return fallback;
}

bool parseAttachmentTable(std::string_view tomlText, std::string_view sourceName, AttachmentTable& out,
                          std::string& error)
{
    toml::table root;
    try
    {
        root = toml::parse(tomlText, sourceName);
    }
    catch (const toml::parse_error& e)
    {
        const auto& b = e.source().begin;
        error = std::format("{}:{}:{}: {}", sourceName, b.line, b.column, e.description());
        return false;
    }
    Problems p(sourceName);
    for (auto&& [key, value] : root)
    {
        if (key.str() != "version" && key.str() != "attach")
        {
            p.add(&value, std::format("top level: unknown key '{}'", key.str()));
            error = p.error();
            return false;
        }
    }
    const toml::node* version = root.get("version");
    if (version == nullptr || version->as_integer() == nullptr || version->as_integer()->get() != 1)
    {
        p.add(version, "'version = 1' expected");
        error = p.error();
        return false;
    }
    const toml::node* rows = root.get("attach");
    const toml::array* list = rows != nullptr ? rows->as_array() : nullptr;
    if (list == nullptr || list->empty())
    {
        p.add(rows, "no [[attach]] entries");
        error = p.error();
        return false;
    }
    AttachmentTable table;
    std::set<std::tuple<int, std::string, int>> seen;
    std::set<std::pair<int, int>> used;
    std::set<std::pair<int, int>> fallbacks;
    for (const auto& node : *list)
    {
        const auto* t = node.as_table();
        if (t == nullptr)
        {
            p.add(&node, "'attach' must be an array of tables ([[attach]])");
            break;
        }
        WeaponAttachment a;
        if (!parseRow(*t, a, p))
            break;
        const int g = static_cast<int>(a.grip);
        const int h = static_cast<int>(a.hand);
        if (!seen.emplace(g, a.look, h).second)
        {
            p.add(t, std::format("duplicate [[attach]] for grip '{}', look '{}', hand '{}'", gripName(a.grip), a.look,
                                 weaponHandName(a.hand)));
            break;
        }
        used.emplace(g, h);
        if (a.look == kAnyLook)
            fallbacks.emplace(g, h);
        table.m_rows.push_back(std::move(a));
    }
    for (const auto& [g, h] : used)
    {
        if (!p.any() && fallbacks.count({g, h}) == 0)
            p.add(nullptr, std::format("grip '{}', hand '{}' has no look = \"*\" fallback row",
                                       gripName(static_cast<Grip>(g)), weaponHandName(static_cast<WeaponHand>(h))));
    }
    if (p.any())
    {
        error = p.error();
        return false;
    }
    out = std::move(table);
    return true;
}

bool loadAttachmentTable(const std::filesystem::path& file, AttachmentTable& out, std::string& error)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        error = std::format("cannot open {}", core::pathToUtf8(file));
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad())
    {
        error = std::format("cannot read {}", core::pathToUtf8(file));
        return false;
    }
    return parseAttachmentTable(ss.str(), core::pathToUtf8(file), out, error);
}

std::optional<std::filesystem::path> locateAttachmentsFile()
{
    return core::locateDataFile(std::filesystem::path(kAttachmentsFile));
}

std::string_view weaponHandName(WeaponHand h) noexcept
{
    return h == WeaponHand::Main ? "main" : "off";
}

assets::Mat4 weaponWorldMatrix(const assets::Mat4& characterWorld, const assets::Mat4& boneModel,
                               const WeaponAttachment& attachment) noexcept
{
    return characterWorld * (boneModel * attachment.matrix());
}

} // namespace vo::game
