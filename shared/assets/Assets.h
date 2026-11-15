// shared/assets - runtime asset formats and loaders shared by client, server and tools. Umbrella header.
//   AssetMath.h  Vec3/Quat/Mat4/Transform (column vectors, glTF matrix layout)
//   AssetFile.h  common header, CRC-32, asset path rules, file IO
//   Mesh.h       .vmesh (static/skinned mesh, skeleton, appearance variants)
//   Animation.h  .vanim (sampled clips)
//   Terrain.h    .vterrain (heightmap, splat, walk mask, spawns, placements)
//   Texture.h    .vtex (pre-mipped, block-compressed textures)
//   NormalMapping.h  tangent-space normal decoding as the shaders do it
//   Pose.h       CPU pose evaluation and skinning
// Formats: docs/ASSET_FORMATS.md. Pipeline: docs/ASSET_PIPELINE.md.
#pragma once

#include "shared/assets/Animation.h"
#include "shared/assets/AssetFile.h"
#include "shared/assets/AssetMath.h"
#include "shared/assets/Mesh.h"
#include "shared/assets/NormalMapping.h"
#include "shared/assets/Pose.h"
#include "shared/assets/Terrain.h"
#include "shared/assets/Texture.h"
