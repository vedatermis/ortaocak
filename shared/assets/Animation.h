// .vanim: skeletal animation clips sampled at a fixed rate (docs/ASSET_FORMATS.md §3). Tracks
// refer to bones by name; see Pose.h for binding a clip to a skeleton and sampling it.
#pragma once

#include "shared/assets/AssetMath.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vo::assets {

struct AnimationTrack
{
    std::string bone;
    std::vector<Transform> keys; // frameCount keys, local transforms in engine space
};

struct AnimationClip
{
    std::string name;
    float duration = 0.0f;   // (frameCount - 1) / sampleRate
    float sampleRate = 30.0f;
    std::uint32_t frameCount = 1;
    bool loop = true;
    bool quantizedRotation = false; // storage only: rotations written as snorm16
    float moveSpeed = 0.0f;         // m/s the clip is authored for (0 = in place)
    std::vector<AnimationTrack> tracks;

    [[nodiscard]] const AnimationTrack* findTrack(std::string_view bone) const noexcept;
};

struct AnimationFile
{
    std::vector<AnimationClip> clips;

    [[nodiscard]] const AnimationClip* findClip(std::string_view name) const noexcept;
};

[[nodiscard]] bool parseAnimation(std::span<const std::byte> file, AnimationFile& out, std::string& error);
[[nodiscard]] bool loadAnimation(const std::filesystem::path& path, AnimationFile& out, std::string& error);
[[nodiscard]] bool serializeAnimation(const AnimationFile& anim, std::vector<std::byte>& out, std::string& error);

} // namespace vo::assets
