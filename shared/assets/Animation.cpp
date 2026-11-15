#include "shared/assets/Animation.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace vo::assets {

namespace {

constexpr std::size_t kInfoEnd = 32;
constexpr std::uint32_t kMaxClips = 256;
constexpr std::uint32_t kMaxTracks = 255;
constexpr std::uint32_t kMaxFrames = 65536;
constexpr std::uint32_t kMaxStringTable = 1u << 20;
constexpr std::uint32_t kClipSize = 36;
constexpr std::uint32_t kTrackSize = 12;
constexpr std::uint32_t kFlagLoop = 0x1;
constexpr std::uint32_t kFlagQuantized = 0x2;

bool finite(float f) noexcept { return std::isfinite(f); }

std::int16_t toSnorm16(float v) noexcept
{
    const float c = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(c * 32767.0f));
}

} // namespace

const AnimationTrack* AnimationClip::findTrack(std::string_view bone) const noexcept
{
    for (const AnimationTrack& t : tracks)
        if (t.bone == bone)
            return &t;
    return nullptr;
}

const AnimationClip* AnimationFile::findClip(std::string_view name) const noexcept
{
    for (const AnimationClip& c : clips)
        if (c.name == name)
            return &c;
    return nullptr;
}

bool parseAnimation(std::span<const std::byte> file, AnimationFile& out, std::string& error)
{
    if (!detail::checkHeader(file, kMagicAnim, error))
        return false;
    if (file.size() < kInfoEnd)
        return error = "file too small for AnimInfo", false;
    detail::ByteReader r(file);
    const auto clipCount = r.read<std::uint32_t>(16);
    const auto clipOffset = r.read<std::uint32_t>(20);
    const auto stringOffset = r.read<std::uint32_t>(24);
    const auto stringSize = r.read<std::uint32_t>(28);
    if (clipCount < 1 || clipCount > kMaxClips)
        return error = std::format("clipCount {} out of range", clipCount), false;
    if (stringSize > kMaxStringTable)
        return error = "string table too large", false;
    if (!r.hasRange(clipOffset, clipCount, kClipSize) || !r.hasRange(stringOffset, stringSize, 1))
        return error = "section outside the file", false;
    const auto strings = r.bytes(stringOffset, stringSize);

    out.clips.clear();
    out.clips.resize(clipCount);
    std::set<std::string, std::less<>> clipNames;
    for (std::uint32_t c = 0; c < clipCount; ++c)
    {
        const std::uint64_t base = clipOffset + static_cast<std::uint64_t>(c) * kClipSize;
        AnimationClip& clip = out.clips[c];
        if (!detail::readStringRef(r, base, strings, clip.name) || clip.name.empty())
            return error = std::format("clip {}: bad name", c), false;
        if (!clipNames.insert(clip.name).second)
            return error = std::format("duplicate clip '{}'", clip.name), false;
        clip.duration = r.read<float>(base + 8);
        clip.sampleRate = r.read<float>(base + 12);
        clip.frameCount = r.read<std::uint32_t>(base + 16);
        const auto flags = r.read<std::uint32_t>(base + 20);
        clip.moveSpeed = r.read<float>(base + 24);
        const auto trackCount = r.read<std::uint32_t>(base + 28);
        const auto trackOffset = r.read<std::uint32_t>(base + 32);
        if (!finite(clip.sampleRate) || clip.sampleRate < 1.0f || clip.sampleRate > 240.0f)
            return error = std::format("clip '{}': sampleRate out of range", clip.name), false;
        if (clip.frameCount < 1 || clip.frameCount > kMaxFrames)
            return error = std::format("clip '{}': frameCount {} out of range", clip.name, clip.frameCount), false;
        const float expected = static_cast<float>(clip.frameCount - 1) / clip.sampleRate;
        if (!finite(clip.duration) || std::abs(clip.duration - expected) > 1e-4f ||
            (clip.frameCount > 1 && !(clip.duration > 0.0f)))
            return error = std::format("clip '{}': duration inconsistent with frameCount", clip.name), false;
        if ((flags & ~(kFlagLoop | kFlagQuantized)) != 0)
            return error = std::format("clip '{}': unknown flags", clip.name), false;
        clip.loop = (flags & kFlagLoop) != 0;
        clip.quantizedRotation = (flags & kFlagQuantized) != 0;
        if (!finite(clip.moveSpeed) || clip.moveSpeed < 0.0f)
            return error = std::format("clip '{}': bad moveSpeed", clip.name), false;
        if (trackCount > kMaxTracks || !r.hasRange(trackOffset, trackCount, kTrackSize))
            return error = std::format("clip '{}': bad track table", clip.name), false;
        const std::uint64_t keySize = clip.quantizedRotation ? 32 : 40;

        clip.tracks.resize(trackCount);
        std::set<std::string, std::less<>> trackNames;
        for (std::uint32_t t = 0; t < trackCount; ++t)
        {
            const std::uint64_t tb = trackOffset + static_cast<std::uint64_t>(t) * kTrackSize;
            AnimationTrack& track = clip.tracks[t];
            if (!detail::readStringRef(r, tb, strings, track.bone) || track.bone.empty())
                return error = std::format("clip '{}': track {} bad bone name", clip.name, t), false;
            if (!trackNames.insert(track.bone).second)
                return error = std::format("clip '{}': duplicate track '{}'", clip.name, track.bone), false;
            const auto keyOffset = r.read<std::uint32_t>(tb + 8);
            if (!r.hasRange(keyOffset, clip.frameCount, keySize))
                return error = std::format("clip '{}': track '{}' keys outside the file", clip.name, track.bone),
                       false;
            track.keys.resize(clip.frameCount);
            for (std::uint32_t k = 0; k < clip.frameCount; ++k)
            {
                std::uint64_t o = keyOffset + k * keySize;
                Transform& x = track.keys[k];
                x.translation = {r.read<float>(o), r.read<float>(o + 4), r.read<float>(o + 8)};
                o += 12;
                if (clip.quantizedRotation)
                {
                    x.rotation = normalize(Quat{r.read<std::int16_t>(o) / 32767.0f, r.read<std::int16_t>(o + 2) / 32767.0f,
                                                r.read<std::int16_t>(o + 4) / 32767.0f,
                                                r.read<std::int16_t>(o + 6) / 32767.0f});
                    o += 8;
                }
                else
                {
                    x.rotation = {r.read<float>(o), r.read<float>(o + 4), r.read<float>(o + 8), r.read<float>(o + 12)};
                    o += 16;
                }
                x.scale = {r.read<float>(o), r.read<float>(o + 4), r.read<float>(o + 8)};
                const Quat& q = x.rotation;
                if (!finite(x.translation.x) || !finite(x.translation.y) || !finite(x.translation.z) ||
                    !finite(x.scale.x) || !finite(x.scale.y) || !finite(x.scale.z) || !finite(q.x) || !finite(q.y) ||
                    !finite(q.z) || !finite(q.w) || std::abs(dot(q, q) - 1.0f) > 1e-3f)
                    return error = std::format("clip '{}': track '{}' key {} invalid", clip.name, track.bone, k),
                           false;
            }
        }
    }
    if (!r.ok)
        return error = "truncated data", false;
    return true;
}

bool loadAnimation(const std::filesystem::path& path, AnimationFile& out, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!readFileBytes(path, bytes, error))
        return false;
    if (!parseAnimation(bytes, out, error))
    {
        error = std::format("{}: {}", core::pathToUtf8(path), error);
        return false;
    }
    return true;
}

bool serializeAnimation(const AnimationFile& anim, std::vector<std::byte>& out, std::string& error)
{
    if (anim.clips.empty() || anim.clips.size() > kMaxClips)
        return error = "clip count out of range", false;
    for (const AnimationClip& c : anim.clips)
    {
        if (c.tracks.size() > kMaxTracks)
            return error = std::format("clip '{}': too many tracks", c.name), false;
        for (const AnimationTrack& t : c.tracks)
            if (t.keys.size() != c.frameCount)
                return error = std::format("clip '{}': track '{}' has {} keys, expected {}", c.name, t.bone,
                                           t.keys.size(), c.frameCount),
                       false;
    }

    detail::ByteWriter w;
    detail::StringTableBuilder strings;
    w.beginFile(kMagicAnim);
    w.putZeros(kInfoEnd - w.size());
    w.patch<std::uint32_t>(16, static_cast<std::uint32_t>(anim.clips.size()));
    w.patch<std::uint32_t>(20, static_cast<std::uint32_t>(w.size()));

    // Clip table first, track tables and keys after; offsets patched as we go.
    const std::size_t clipTable = w.size();
    w.putZeros(anim.clips.size() * kClipSize);
    for (std::size_t c = 0; c < anim.clips.size(); ++c)
    {
        const AnimationClip& clip = anim.clips[c];
        const std::size_t cb = clipTable + c * kClipSize;
        const auto n = strings.add(clip.name);
        w.patch(cb, n.offset);
        w.patch(cb + 4, n.length);
        w.patch(cb + 8, clip.duration);
        w.patch(cb + 12, clip.sampleRate);
        w.patch(cb + 16, clip.frameCount);
        w.patch<std::uint32_t>(cb + 20, (clip.loop ? kFlagLoop : 0u) | (clip.quantizedRotation ? kFlagQuantized : 0u));
        w.patch(cb + 24, clip.moveSpeed);
        w.patch(cb + 28, static_cast<std::uint32_t>(clip.tracks.size()));
        w.patch(cb + 32, static_cast<std::uint32_t>(w.size()));
        const std::size_t trackTable = w.size();
        w.putZeros(clip.tracks.size() * kTrackSize);
        for (std::size_t t = 0; t < clip.tracks.size(); ++t)
        {
            const AnimationTrack& track = clip.tracks[t];
            const std::size_t tb = trackTable + t * kTrackSize;
            const auto bn = strings.add(track.bone);
            w.patch(tb, bn.offset);
            w.patch(tb + 4, bn.length);
            w.patch(tb + 8, static_cast<std::uint32_t>(w.size()));
            for (const Transform& k : track.keys)
            {
                w.put(k.translation);
                if (clip.quantizedRotation)
                {
                    const Quat q = normalize(k.rotation);
                    w.put(toSnorm16(q.x));
                    w.put(toSnorm16(q.y));
                    w.put(toSnorm16(q.z));
                    w.put(toSnorm16(q.w));
                }
                else
                {
                    w.put(k.rotation);
                }
                w.put(k.scale);
            }
        }
    }
    w.align4();
    w.patch<std::uint32_t>(24, static_cast<std::uint32_t>(w.size()));
    w.patch<std::uint32_t>(28, static_cast<std::uint32_t>(strings.bytes().size()));
    w.putBytes(strings.bytes());
    w.align4();
    if (w.size() > kMaxFileSize)
        return error = "animation too large", false;
    out = w.finishFile();

    AnimationFile check;
    std::string why;
    if (!parseAnimation(out, check, why))
    {
        error = "serialised animation is invalid: " + why;
        out.clear();
        return false;
    }
    return true;
}

} // namespace vo::assets
