#include "shared/assets/Pose.h"

#include <algorithm>
#include <cmath>

namespace vo::assets {

ClipBinding bindClip(const AnimationClip& clip, const Skeleton& skeleton)
{
    ClipBinding binding;
    binding.reserve(clip.tracks.size());
    for (const AnimationTrack& t : clip.tracks)
        binding.push_back(skeleton.findBone(t.bone));
    return binding;
}

void bindPose(const Skeleton& skeleton, std::vector<Transform>& localPose)
{
    localPose.resize(skeleton.bones.size());
    for (std::size_t b = 0; b < skeleton.bones.size(); ++b)
        localPose[b] = skeleton.bones[b].bindLocal;
}

void samplePose(const AnimationClip& clip, const ClipBinding& binding, const Skeleton& skeleton, float time,
                bool loop, std::vector<Transform>& localPose)
{
    bindPose(skeleton, localPose);
    if (clip.frameCount == 0 || clip.tracks.empty())
        return;

    if (!std::isfinite(time))
        time = 0.0f;
    if (clip.duration > 0.0f)
    {
        if (loop)
        {
            time = std::fmod(time, clip.duration);
            if (time < 0.0f)
                time += clip.duration;
        }
        else
        {
            time = std::clamp(time, 0.0f, clip.duration);
        }
    }
    else
    {
        time = 0.0f;
    }

    const float frame = time * clip.sampleRate;
    const auto lastFrame = static_cast<std::uint32_t>(clip.frameCount - 1);
    auto k0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(frame)));
    k0 = std::min(k0, lastFrame);
    const std::uint32_t k1 = std::min(k0 + 1, lastFrame);
    const float a = std::clamp(frame - static_cast<float>(k0), 0.0f, 1.0f);

    const std::size_t n = std::min(binding.size(), clip.tracks.size());
    for (std::size_t t = 0; t < n; ++t)
    {
        const int bone = binding[t];
        if (bone < 0 || static_cast<std::size_t>(bone) >= localPose.size())
            continue;
        const AnimationTrack& track = clip.tracks[t];
        if (track.keys.size() != clip.frameCount)
            continue;
        const Transform& x0 = track.keys[k0];
        const Transform& x1 = track.keys[k1];
        Transform& dst = localPose[static_cast<std::size_t>(bone)];
        dst.translation = lerp(x0.translation, x1.translation, a);
        dst.rotation = nlerp(x0.rotation, x1.rotation, a);
        dst.scale = lerp(x0.scale, x1.scale, a);
    }
}

void localToModel(const Skeleton& skeleton, std::span<const Transform> localPose, std::vector<Mat4>& model)
{
    model.resize(skeleton.bones.size());
    for (std::size_t b = 0; b < skeleton.bones.size(); ++b)
    {
        const Transform& local = b < localPose.size() ? localPose[b] : skeleton.bones[b].bindLocal;
        const Mat4 m = local.toMatrix();
        const std::int32_t parent = skeleton.bones[b].parent;
        if (parent >= 0 && static_cast<std::size_t>(parent) < b)
            model[b] = model[static_cast<std::size_t>(parent)] * m;
        else
            model[b] = m;
    }
}

void computeSkinningMatrices(const Skeleton& skeleton, std::span<const Mat4> model, std::vector<Mat4>& skin)
{
    skin.resize(skeleton.bones.size());
    for (std::size_t b = 0; b < skeleton.bones.size(); ++b)
        skin[b] = (b < model.size() ? model[b] : Mat4::identity()) * skeleton.bones[b].inverseBind;
}

Vec3 skinPosition(const MeshData& mesh, std::uint32_t vertex, std::span<const Mat4> skin) noexcept
{
    const Vec3 p = mesh.position(vertex);
    if (!mesh.isSkinned())
        return p;
    const auto j = mesh.joints(vertex);
    const auto w = mesh.weights(vertex);
    Vec3 result{};
    for (std::size_t k = 0; k < 4; ++k)
    {
        if (w[k] == 0 || j[k] >= skin.size())
            continue;
        result = result + transformPoint(skin[j[k]], p) * (static_cast<float>(w[k]) / 255.0f);
    }
    return result;
}

Vec3 skinNormal(const MeshData& mesh, std::uint32_t vertex, std::span<const Mat4> skin) noexcept
{
    const Vec3 n = mesh.normal(vertex);
    if (!mesh.isSkinned())
        return n;
    const auto j = mesh.joints(vertex);
    const auto w = mesh.weights(vertex);
    Vec3 result{};
    for (std::size_t k = 0; k < 4; ++k)
    {
        if (w[k] == 0 || j[k] >= skin.size())
            continue;
        result = result + transformVector(skin[j[k]], n) * (static_cast<float>(w[k]) / 255.0f);
    }
    return normalize(result);
}

} // namespace vo::assets
