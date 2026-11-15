// CPU pose evaluation: bind a clip to a skeleton by bone name, sample it at time t, build model-space
// and skinning matrices, and skin single vertices (tests, picking, bounds). The client uploads the
// skinning matrices for GPU skinning as-is (layout in docs/ASSET_FORMATS.md §1).
//
//   ClipBinding binding = bindClip(clip, mesh.skeleton);       // once per (clip, skeleton)
//   samplePose(clip, binding, mesh.skeleton, t, clip.loop, local);
//   localToModel(mesh.skeleton, local, model);
//   computeSkinningMatrices(mesh.skeleton, model, skin);       // skin[b] = model[b] * inverseBind[b]
#pragma once

#include "shared/assets/Animation.h"
#include "shared/assets/AssetMath.h"
#include "shared/assets/Mesh.h"

#include <span>
#include <vector>

namespace vo::assets {

// For each track of the clip, the skeleton bone index it drives, or -1 if the skeleton has no such bone.
using ClipBinding = std::vector<int>;

[[nodiscard]] ClipBinding bindClip(const AnimationClip& clip, const Skeleton& skeleton);

// Bind-pose local transforms of every bone.
void bindPose(const Skeleton& skeleton, std::vector<Transform>& localPose);

// Local pose at `time` seconds. With `loop`, time wraps into [0, duration); otherwise it is clamped.
// Bones without a track keep their bind-pose local transform.
void samplePose(const AnimationClip& clip, const ClipBinding& binding, const Skeleton& skeleton, float time,
                bool loop, std::vector<Transform>& localPose);

// Model-space matrices: model[b] = model[parent] * local[b]. `local` must have one entry per bone.
void localToModel(const Skeleton& skeleton, std::span<const Transform> localPose, std::vector<Mat4>& model);

// skin[b] = model[b] * inverseBind[b].
void computeSkinningMatrices(const Skeleton& skeleton, std::span<const Mat4> model, std::vector<Mat4>& skin);

// Linear-blend skinned position / normal of one vertex (static meshes: returned unchanged).
[[nodiscard]] Vec3 skinPosition(const MeshData& mesh, std::uint32_t vertex, std::span<const Mat4> skin) noexcept;
[[nodiscard]] Vec3 skinNormal(const MeshData& mesh, std::uint32_t vertex, std::span<const Mat4> skin) noexcept;

} // namespace vo::assets
