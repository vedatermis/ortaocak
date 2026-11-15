#include "tools/assetc/GltfImport.h"

#include "shared/assets/AssetFile.h"
#include "shared/core/Paths.h"

#include <cgltf.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string_view>

namespace vo::tools::assetc {

using assets::Mat4;
using assets::Quat;
using assets::Transform;
using assets::Vec3;

Mat4 gltfToEngine(const Mat4& m) noexcept
{
    Mat4 r = m;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if ((row == 0) != (col == 0))
                r.at(row, col) = -m.at(row, col);
    return r;
}

namespace {

struct CgltfDeleter
{
    void operator()(cgltf_data* d) const noexcept { cgltf_free(d); }
};

struct GltfDocument
{
    std::vector<std::byte> bytes; // cgltf keeps pointers into this (GLB BIN chunk, JSON)
    std::unique_ptr<cgltf_data, CgltfDeleter> data;
    std::filesystem::path path;
};

const char* resultName(cgltf_result r) noexcept
{
    switch (r)
    {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF 1.0";
    default: return "error";
    }
}

bool loadDocument(const std::filesystem::path& path, GltfDocument& doc, std::string& error)
{
    doc.path = path;
    if (!assets::readFileBytes(path, doc.bytes, error))
        return false;
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    cgltf_result r = cgltf_parse(&options, doc.bytes.data(), doc.bytes.size(), &raw);
    if (r != cgltf_result_success)
    {
        error = std::format("{}: cannot parse glTF ({})", core::pathToUtf8(path), resultName(r));
        return false;
    }
    doc.data.reset(raw);
    const std::string utf8 = core::pathToUtf8(path);
    r = cgltf_load_buffers(&options, raw, utf8.c_str());
    if (r != cgltf_result_success)
    {
        error = std::format("{}: cannot load buffers ({})", utf8, resultName(r));
        return false;
    }
    r = cgltf_validate(raw);
    if (r != cgltf_result_success)
    {
        error = std::format("{}: glTF validation failed ({})", utf8, resultName(r));
        return false;
    }
    return true;
}

Mat4 toMat4(const cgltf_float* m) noexcept
{
    Mat4 r;
    for (std::size_t i = 0; i < 16; ++i)
        r.m[i] = m[i];
    return r;
}

Mat4 localMatrix(const cgltf_node* n) noexcept
{
    cgltf_float m[16];
    cgltf_node_transform_local(n, m);
    return toMat4(m);
}

Mat4 worldMatrix(const cgltf_node* n) noexcept
{
    cgltf_float m[16];
    cgltf_node_transform_world(n, m);
    return toMat4(m);
}

bool isIdentity(const Mat4& m) noexcept
{
    const Mat4 id = Mat4::identity();
    for (std::size_t i = 0; i < 16; ++i)
        if (std::abs(m.m[i] - id.m[i]) > 1e-6f)
            return false;
    return true;
}

std::string nodeName(const cgltf_node* n)
{
    if (n->name != nullptr && n->name[0] != '\0')
        return n->name;
    if (n->mesh != nullptr && n->mesh->name != nullptr)
        return n->mesh->name;
    return {};
}

// Mesh nodes of the default scene in depth-first order (scene order, then children order).
std::vector<const cgltf_node*> collectSceneNodes(const cgltf_data* data)
{
    std::vector<const cgltf_node*> roots;
    const cgltf_scene* scene = data->scene != nullptr ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene != nullptr)
    {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            roots.push_back(scene->nodes[i]);
    }
    else
    {
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
            if (data->nodes[i].parent == nullptr)
                roots.push_back(&data->nodes[i]);
    }
    std::vector<const cgltf_node*> out;
    std::vector<const cgltf_node*> stack(roots.rbegin(), roots.rend());
    std::set<const cgltf_node*> seen;
    while (!stack.empty())
    {
        const cgltf_node* n = stack.back();
        stack.pop_back();
        if (!seen.insert(n).second)
            continue;
        out.push_back(n);
        for (cgltf_size c = n->children_count; c > 0; --c)
            stack.push_back(n->children[c - 1]);
    }
    return out;
}

// ---- skeleton -------------------------------------------------------------------------------

struct SkeletonImport
{
    assets::Skeleton skeleton;
    std::vector<int> jointToBone;               // skin joint index -> bone index
    std::vector<const cgltf_node*> boneNodes;   // bone index -> node
    std::vector<Mat4> rootFold;                 // bone index -> glTF-space parent transform folded in
    std::map<const cgltf_node*, int> nodeToBone;
};

bool buildSkeleton(const cgltf_skin* skin, SkeletonImport& out, std::string& error)
{
    const cgltf_size jointCount = skin->joints_count;
    if (jointCount == 0 || jointCount > 255)
        return error = std::format("skin has {} joints (1..255 supported)", jointCount), false;

    std::map<const cgltf_node*, int> jointIndex;
    for (cgltf_size j = 0; j < jointCount; ++j)
        if (!jointIndex.emplace(skin->joints[j], static_cast<int>(j)).second)
            return error = "skin lists a joint twice", false;

    // Parent joint (skin index) per joint, and the non-joint ancestor transform for roots.
    std::vector<int> parentJoint(jointCount, -1);
    std::vector<Mat4> fold(jointCount, Mat4::identity());
    std::set<std::string> names;
    for (cgltf_size j = 0; j < jointCount; ++j)
    {
        const cgltf_node* n = skin->joints[j];
        const std::string name = nodeName(n);
        if (name.empty())
            return error = std::format("joint {} has no name", j), false;
        if (!names.insert(name).second)
            return error = std::format("duplicate joint name '{}'", name), false;
        const cgltf_node* p = n->parent;
        if (p != nullptr && jointIndex.contains(p))
        {
            parentJoint[j] = jointIndex[p];
            continue;
        }
        for (const cgltf_node* a = p; a != nullptr; a = a->parent)
            if (jointIndex.contains(a))
                return error = std::format("joint '{}' has a non-joint parent below another joint (unsupported)", name),
                       false;
        if (p != nullptr)
            fold[j] = worldMatrix(p);
    }

    // Parents-first order: keep the skin order if it already is, else depth-first emission.
    bool ordered = true;
    for (cgltf_size j = 0; j < jointCount; ++j)
        if (parentJoint[j] >= static_cast<int>(j))
            ordered = false;
    std::vector<int> order;
    if (ordered)
    {
        for (cgltf_size j = 0; j < jointCount; ++j)
            order.push_back(static_cast<int>(j));
    }
    else
    {
        std::vector<char> emitted(jointCount, 0);
        for (cgltf_size j = 0; j < jointCount; ++j)
        {
            std::vector<int> chain;
            for (int k = static_cast<int>(j); k >= 0 && !emitted[static_cast<std::size_t>(k)]; k = parentJoint[static_cast<std::size_t>(k)])
                chain.push_back(k);
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                emitted[static_cast<std::size_t>(*it)] = 1;
                order.push_back(*it);
            }
        }
    }

    std::vector<Mat4> inverseBind(jointCount, Mat4::identity());
    if (skin->inverse_bind_matrices != nullptr)
    {
        const cgltf_accessor* acc = skin->inverse_bind_matrices;
        if (acc->count < jointCount || acc->type != cgltf_type_mat4)
            return error = "inverseBindMatrices accessor too small or not MAT4", false;
        for (cgltf_size j = 0; j < jointCount; ++j)
        {
            cgltf_float m[16];
            if (!cgltf_accessor_read_float(acc, j, m, 16))
                return error = "cannot read inverseBindMatrices", false;
            inverseBind[j] = toMat4(m);
        }
    }

    out.jointToBone.assign(jointCount, -1);
    for (std::size_t b = 0; b < order.size(); ++b)
        out.jointToBone[static_cast<std::size_t>(order[b])] = static_cast<int>(b);

    out.skeleton.bones.resize(jointCount);
    out.boneNodes.resize(jointCount);
    out.rootFold.resize(jointCount);
    for (std::size_t b = 0; b < order.size(); ++b)
    {
        const auto j = static_cast<std::size_t>(order[b]);
        const cgltf_node* n = skin->joints[j];
        assets::Bone& bone = out.skeleton.bones[b];
        bone.name = nodeName(n);
        bone.parent = parentJoint[j] < 0 ? -1 : out.jointToBone[static_cast<std::size_t>(parentJoint[j])];
        bone.inverseBind = gltfToEngine(inverseBind[j]);
        const Mat4 local = fold[j] * localMatrix(n);
        Transform t;
        if (!assets::decomposeTrs(gltfToEngine(local), t.translation, t.rotation, t.scale))
            return error = std::format("joint '{}': local transform is not a TRS", bone.name), false;
        bone.bindLocal = t;
        out.boneNodes[b] = n;
        out.rootFold[b] = fold[j];
        out.nodeToBone[n] = static_cast<int>(b);
    }
    return true;
}

// ---- mesh ------------------------------------------------------------------------------------

const cgltf_accessor* findAttribute(const cgltf_primitive& prim, cgltf_attribute_type type, cgltf_int index = 0)
{
    for (cgltf_size a = 0; a < prim.attributes_count; ++a)
        if (prim.attributes[a].type == type && prim.attributes[a].index == index)
            return prim.attributes[a].data;
    return nullptr;
}

std::uint8_t linearToSrgb8(float c) noexcept
{
    c = std::clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

std::uint8_t unorm8(float c) noexcept
{
    c = std::clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(c * 255.0f));
}

// Quantises up to four weights to bytes summing to exactly 255 (largest remainder, ties -> lower slot).
void quantizeWeights(std::array<float, 4> w, std::array<std::uint8_t, 4>& out) noexcept
{
    float sum = 0.0f;
    for (float v : w)
        sum += v;
    std::array<int, 4> q{};
    std::array<float, 4> frac{};
    int total = 0;
    for (std::size_t k = 0; k < 4; ++k)
    {
        const float scaled = w[k] / sum * 255.0f;
        q[k] = static_cast<int>(std::floor(scaled));
        frac[k] = scaled - static_cast<float>(q[k]);
        total += q[k];
    }
    while (total < 255)
    {
        std::size_t best = 0;
        for (std::size_t k = 1; k < 4; ++k)
            if (frac[k] > frac[best])
                best = k;
        ++q[best];
        frac[best] = -1.0f;
        ++total;
    }
    for (std::size_t k = 0; k < 4; ++k)
        out[k] = static_cast<std::uint8_t>(q[k]);
}

struct VariantInfo
{
    assets::VariantGroup group = assets::VariantGroup::Always;
    std::uint8_t index = 0;
};

VariantInfo parseVariant(std::string_view name) noexcept
{
    const auto tryPrefix = [&](std::string_view prefix, assets::VariantGroup g, VariantInfo& out) {
        if (!name.starts_with(prefix) || name.size() == prefix.size() || name.size() > prefix.size() + 2)
            return false;
        int value = 0;
        for (char c : name.substr(prefix.size()))
        {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + (c - '0');
        }
        if (value > 15)
            return false;
        out.group = g;
        out.index = static_cast<std::uint8_t>(value);
        return true;
    };
    VariantInfo v;
    if (tryPrefix("hair_", assets::VariantGroup::Hair, v) || tryPrefix("face_", assets::VariantGroup::Face, v))
        return v;
    return {};
}

struct WorkVertex
{
    Vec3 position;
    Vec3 normal;
    std::array<float, 4> tangent{};
    std::array<float, 2> uv{};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    std::array<std::uint8_t, 4> joints{};
    std::array<std::uint8_t, 4> weights{};
};

bool resolveTexturePath(const GltfDocument& doc, const cgltf_image* image, const std::filesystem::path& root,
                        std::string& assetPath, std::filesystem::path& sourceFile, std::string& error)
{
    if (image->uri == nullptr || std::string_view(image->uri).starts_with("data:"))
        return error = "embedded texture images are not supported; reference an external PNG", false;
    std::string uri = image->uri;
    uri.resize(cgltf_decode_uri(uri.data()));
    const std::filesystem::path abs =
        (doc.path.parent_path() / core::pathFromUtf8(uri)).lexically_normal();
    const std::filesystem::path base = root.empty() ? doc.path.parent_path() : root;
    const std::filesystem::path rel = abs.lexically_relative(base.lexically_normal());
    assetPath = core::pathToUtf8(rel);
    std::replace(assetPath.begin(), assetPath.end(), '\\', '/');
    if (rel.empty() || !assets::isValidAssetPath(assetPath))
        return error = std::format("texture '{}' is outside the asset root", uri), false;
    sourceFile = abs;
    return true;
}

} // namespace

bool importMesh(const std::filesystem::path& gltfPath, const MeshImportOptions& options, assets::MeshData& out,
                std::string& error, std::vector<std::filesystem::path>* textureFiles)
{
    GltfDocument doc;
    if (!loadDocument(gltfPath, doc, error))
        return false;
    const cgltf_data* data = doc.data.get();
    const std::string fileName = core::pathToUtf8(gltfPath);

    std::vector<const cgltf_node*> meshNodes;
    for (const cgltf_node* n : collectSceneNodes(data))
        if (n->mesh != nullptr)
            meshNodes.push_back(n);
    if (meshNodes.empty())
        return error = std::format("{}: no mesh nodes in the scene", fileName), false;

    const cgltf_skin* skin = nullptr;
    for (const cgltf_node* n : meshNodes)
        if (n->skin != nullptr)
            skin = n->skin;
    SkeletonImport skel;
    if (skin != nullptr)
    {
        for (const cgltf_node* n : meshNodes)
            if (n->skin != skin)
                return error = std::format("{}: mesh node '{}' is not skinned to the common skin", fileName, nodeName(n)),
                       false;
        if (!buildSkeleton(skin, skel, error))
            return error = std::format("{}: {}", fileName, error), false;
    }
    const bool skinned = skin != nullptr;

    std::vector<WorkVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<assets::Submesh> submeshes;
    bool anyColor = false;
    bool allTangents = true;

    for (const cgltf_node* node : meshNodes)
    {
        const std::string name = nodeName(node);
        const VariantInfo variant = parseVariant(name);
        const Mat4 world = skinned ? Mat4::identity() : worldMatrix(node);
        Mat4 normalMatrix;
        {
            Mat4 inv;
            if (!assets::invert(world, inv))
                return error = std::format("{}: node '{}' has a singular transform", fileName, name), false;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    normalMatrix.at(r, c) = inv.at(c, r);
        }
        const bool mirrored = assets::determinant3x3(world) < 0.0f;

        struct PrimOut
        {
            std::vector<std::uint32_t> tris;
            bool textured = false;
            std::uint16_t flags = 0;
            std::string texture;
            std::array<float, 4> factor{1, 1, 1, 1};
        };
        std::vector<PrimOut> prims;

        const cgltf_mesh* mesh = node->mesh;
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p)
        {
            const cgltf_primitive& prim = mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles)
                return error = std::format("{}: node '{}': only triangle primitives are supported", fileName, name), false;
            const cgltf_accessor* posAcc = findAttribute(prim, cgltf_attribute_type_position);
            if (posAcc == nullptr)
                return error = std::format("{}: node '{}': primitive without POSITION", fileName, name), false;
            const cgltf_accessor* nrmAcc = findAttribute(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* tanAcc = findAttribute(prim, cgltf_attribute_type_tangent);
            const cgltf_accessor* uvAcc = findAttribute(prim, cgltf_attribute_type_texcoord);
            const cgltf_accessor* colAcc = findAttribute(prim, cgltf_attribute_type_color);
            const cgltf_accessor* jntAcc = findAttribute(prim, cgltf_attribute_type_joints);
            const cgltf_accessor* wgtAcc = findAttribute(prim, cgltf_attribute_type_weights);
            const cgltf_size count = posAcc->count;
            const auto checkCount = [&](const cgltf_accessor* a) { return a == nullptr || a->count == count; };
            if (!checkCount(nrmAcc) || !checkCount(tanAcc) || !checkCount(uvAcc) || !checkCount(colAcc) ||
                !checkCount(jntAcc) || !checkCount(wgtAcc))
                return error = std::format("{}: node '{}': attribute counts differ", fileName, name), false;
            if (skinned && (jntAcc == nullptr || wgtAcc == nullptr))
                return error = std::format("{}: node '{}': skinned primitive without JOINTS_0/WEIGHTS_0", fileName, name),
                       false;
            if (vertices.size() + count > 16'777'216)
                return error = std::format("{}: too many vertices", fileName), false;
            if (tanAcc == nullptr)
                allTangents = false;
            if (colAcc != nullptr)
                anyColor = true;

            PrimOut po;
            const cgltf_material* mat = prim.material;
            if (mat != nullptr)
            {
                if (mat->has_pbr_metallic_roughness)
                {
                    for (std::size_t k = 0; k < 4; ++k)
                        po.factor[k] = mat->pbr_metallic_roughness.base_color_factor[k];
                    const cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
                    if (tex != nullptr && tex->image != nullptr)
                    {
                        std::filesystem::path src;
                        if (!resolveTexturePath(doc, tex->image, options.assetRoot, po.texture, src, error))
                            return error = std::format("{}: node '{}': {}", fileName, name, error), false;
                        po.textured = true;
                        if (textureFiles != nullptr)
                            textureFiles->push_back(src);
                    }
                }
                if (mat->double_sided)
                    po.flags |= assets::kSubmeshDoubleSided;
                if (mat->alpha_mode == cgltf_alpha_mode_blend)
                    po.flags |= assets::kSubmeshAlphaBlend;
            }
            if (!po.textured)
                anyColor = true;

            const auto base = static_cast<std::uint32_t>(vertices.size());
            for (cgltf_size v = 0; v < count; ++v)
            {
                WorkVertex wv;
                cgltf_float f[4] = {0, 0, 0, 0};
                if (!cgltf_accessor_read_float(posAcc, v, f, 3))
                    return error = std::format("{}: cannot read POSITION", fileName), false;
                wv.position = gltfToEngine(assets::transformPoint(world, Vec3{f[0], f[1], f[2]}));
                if (nrmAcc != nullptr)
                {
                    f[0] = f[1] = f[2] = 0;
                    if (!cgltf_accessor_read_float(nrmAcc, v, f, 3))
                        return error = std::format("{}: cannot read NORMAL", fileName), false;
                    wv.normal = gltfToEngine(assets::normalize(assets::transformVector(normalMatrix, Vec3{f[0], f[1], f[2]})));
                }
                if (tanAcc != nullptr)
                {
                    cgltf_float t[4] = {0, 0, 0, 1};
                    if (!cgltf_accessor_read_float(tanAcc, v, t, 4))
                        return error = std::format("{}: cannot read TANGENT", fileName), false;
                    const Vec3 tv = gltfToEngine(assets::normalize(assets::transformVector(world, Vec3{t[0], t[1], t[2]})));
                    const float w = (t[3] < 0.0f ? -1.0f : 1.0f) * (mirrored ? -1.0f : 1.0f);
                    wv.tangent = {tv.x, tv.y, tv.z, -w};
                }
                if (uvAcc != nullptr)
                {
                    cgltf_float uv[2] = {0, 0};
                    if (!cgltf_accessor_read_float(uvAcc, v, uv, 2))
                        return error = std::format("{}: cannot read TEXCOORD_0", fileName), false;
                    wv.uv = {uv[0], uv[1]};
                }
                std::array<float, 4> col{1, 1, 1, 1};
                if (colAcc != nullptr)
                {
                    cgltf_float c[4] = {1, 1, 1, 1};
                    if (!cgltf_accessor_read_float(colAcc, v, c, cgltf_num_components(colAcc->type)))
                        return error = std::format("{}: cannot read COLOR_0", fileName), false;
                    col = {c[0], c[1], c[2], colAcc->type == cgltf_type_vec4 ? c[3] : 1.0f};
                }
                if (!po.textured)
                    for (std::size_t k = 0; k < 4; ++k)
                        col[k] *= po.factor[k];
                wv.color = {linearToSrgb8(col[0]), linearToSrgb8(col[1]), linearToSrgb8(col[2]), unorm8(col[3])};
                if (skinned)
                {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    cgltf_float w[4] = {0, 0, 0, 0};
                    if (!cgltf_accessor_read_uint(jntAcc, v, j, 4) || !cgltf_accessor_read_float(wgtAcc, v, w, 4))
                        return error = std::format("{}: cannot read JOINTS_0/WEIGHTS_0", fileName), false;
                    // Merge duplicate joints, keep the four largest weights.
                    std::array<std::pair<float, int>, 4> slots{};
                    int used = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float wk = std::isfinite(w[k]) ? w[k] : 0.0f;
                        if (!(wk > 0.0f))
                            continue;
                        if (j[k] >= skel.jointToBone.size())
                            return error = std::format("{}: joint index {} out of range", fileName, j[k]), false;
                        const int bone = skel.jointToBone[j[k]];
                        bool merged = false;
                        for (int s = 0; s < used; ++s)
                            if (slots[static_cast<std::size_t>(s)].second == bone)
                            {
                                slots[static_cast<std::size_t>(s)].first += wk;
                                merged = true;
                            }
                        if (!merged)
                            slots[static_cast<std::size_t>(used++)] = {wk, bone};
                    }
                    if (used == 0)
                        return error = std::format("{}: node '{}': vertex {} has no bone weights", fileName, name, v),
                               false;
                    std::stable_sort(slots.begin(), slots.begin() + used,
                                     [](const auto& a, const auto& b) { return a.first > b.first; });
                    std::array<float, 4> ws{};
                    for (int s = 0; s < used; ++s)
                    {
                        ws[static_cast<std::size_t>(s)] = slots[static_cast<std::size_t>(s)].first;
                        wv.joints[static_cast<std::size_t>(s)] = static_cast<std::uint8_t>(slots[static_cast<std::size_t>(s)].second);
                    }
                    quantizeWeights(ws, wv.weights);
                    for (std::size_t s = 0; s < 4; ++s)
                        if (wv.weights[s] == 0)
                            wv.joints[s] = 0;
                }
                vertices.push_back(wv);
            }

            // Indices (generated if absent), winding converted.
            std::vector<std::uint32_t> local;
            if (prim.indices != nullptr)
            {
                local.resize(prim.indices->count);
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                {
                    const cgltf_size idx = cgltf_accessor_read_index(prim.indices, i);
                    if (idx >= count)
                        return error = std::format("{}: node '{}': index out of range", fileName, name), false;
                    local[i] = static_cast<std::uint32_t>(idx);
                }
            }
            else
            {
                local.resize(count);
                for (cgltf_size i = 0; i < count; ++i)
                    local[i] = static_cast<std::uint32_t>(i);
            }
            if (local.size() % 3 != 0)
                return error = std::format("{}: node '{}': index count not a multiple of 3", fileName, name), false;
            // X mirror reverses the winding; a mirroring node transform reverses it back.
            const bool swap = !mirrored;
            for (std::size_t t = 0; t + 2 < local.size(); t += 3)
            {
                const std::uint32_t a = base + local[t];
                const std::uint32_t b = base + local[t + 1];
                const std::uint32_t c = base + local[t + 2];
                po.tris.push_back(a);
                po.tris.push_back(swap ? c : b);
                po.tris.push_back(swap ? b : c);
            }

            // Missing normals: area-weighted smooth normals of this primitive.
            if (nrmAcc == nullptr)
            {
                for (std::size_t t = 0; t + 2 < po.tris.size(); t += 3)
                {
                    WorkVertex& a = vertices[po.tris[t]];
                    WorkVertex& b = vertices[po.tris[t + 1]];
                    WorkVertex& c = vertices[po.tris[t + 2]];
                    const Vec3 n = assets::cross(b.position - a.position, c.position - a.position);
                    a.normal = a.normal + n;
                    b.normal = b.normal + n;
                    c.normal = c.normal + n;
                }
                for (std::size_t v = base; v < vertices.size(); ++v)
                {
                    Vec3 n = assets::normalize(vertices[v].normal);
                    vertices[v].normal = assets::length(n) > 0.0f ? n : Vec3{0, 1, 0};
                }
            }
            prims.push_back(std::move(po));
        }

        // Untextured primitives of a node merge into one submesh per flag set; textured ones stay separate.
        std::vector<std::uint16_t> flagGroups;
        for (const PrimOut& po : prims)
            if (!po.textured && std::find(flagGroups.begin(), flagGroups.end(), po.flags) == flagGroups.end())
                flagGroups.push_back(po.flags);
        for (std::uint16_t flags : flagGroups)
        {
            assets::Submesh sm;
            sm.indexStart = static_cast<std::uint32_t>(indices.size());
            for (const PrimOut& po : prims)
                if (!po.textured && po.flags == flags)
                    indices.insert(indices.end(), po.tris.begin(), po.tris.end());
            sm.indexCount = static_cast<std::uint32_t>(indices.size()) - sm.indexStart;
            sm.name = name;
            sm.variantGroup = variant.group;
            sm.variantIndex = variant.index;
            sm.flags = flags;
            if (sm.indexCount > 0)
                submeshes.push_back(std::move(sm));
        }
        for (const PrimOut& po : prims)
        {
            if (!po.textured || po.tris.empty())
                continue;
            assets::Submesh sm;
            sm.indexStart = static_cast<std::uint32_t>(indices.size());
            indices.insert(indices.end(), po.tris.begin(), po.tris.end());
            sm.indexCount = static_cast<std::uint32_t>(po.tris.size());
            sm.name = name;
            sm.texture = po.texture;
            sm.baseColor = po.factor;
            sm.variantGroup = variant.group;
            sm.variantIndex = variant.index;
            sm.flags = po.flags;
            submeshes.push_back(std::move(sm));
        }
    }

    if (vertices.empty() || indices.empty())
        return error = std::format("{}: mesh has no triangles", fileName), false;
    if (submeshes.size() > 1024)
        return error = std::format("{}: too many submeshes", fileName), false;

    assets::MeshData mesh;
    mesh.vertexFormat = assets::kVertexNormal | (allTangents ? assets::kVertexTangent : 0u) |
                        (anyColor ? assets::kVertexColor : 0u) | (skinned ? assets::kVertexSkin : 0u);
    mesh.vertexCount = static_cast<std::uint32_t>(vertices.size());
    const assets::VertexLayout layout = mesh.layout();
    mesh.vertices.assign(static_cast<std::size_t>(mesh.vertexCount) * layout.stride, 0);
    for (std::size_t v = 0; v < vertices.size(); ++v)
    {
        std::uint8_t* dst = mesh.vertices.data() + v * layout.stride;
        const WorkVertex& wv = vertices[v];
        std::memcpy(dst + layout.position, &wv.position, 12);
        std::memcpy(dst + layout.normal, &wv.normal, 12);
        if (layout.tangent != assets::kAbsent)
            std::memcpy(dst + layout.tangent, wv.tangent.data(), 16);
        std::memcpy(dst + layout.uv0, wv.uv.data(), 8);
        if (layout.color != assets::kAbsent)
            std::memcpy(dst + layout.color, wv.color.data(), 4);
        if (layout.joints != assets::kAbsent)
        {
            std::memcpy(dst + layout.joints, wv.joints.data(), 4);
            std::memcpy(dst + layout.weights, wv.weights.data(), 4);
        }
    }
    mesh.indexSize = mesh.vertexCount <= 65535 ? 2u : 4u;
    mesh.indices.resize(indices.size() * mesh.indexSize);
    for (std::size_t i = 0; i < indices.size(); ++i)
    {
        if (mesh.indexSize == 2)
        {
            const auto v16 = static_cast<std::uint16_t>(indices[i]);
            std::memcpy(mesh.indices.data() + i * 2, &v16, 2);
        }
        else
        {
            std::memcpy(mesh.indices.data() + i * 4, &indices[i], 4);
        }
    }
    mesh.submeshes = std::move(submeshes);
    if (skinned)
        mesh.skeleton = std::move(skel.skeleton);
    mesh.computeBounds();
    out = std::move(mesh);
    return true;
}

// ---- animation sidecar ---------------------------------------------------------------------------

std::filesystem::path animSidecarPath(const std::filesystem::path& gltfPath)
{
    std::filesystem::path p = gltfPath;
    p.replace_extension(".anims.toml");
    return p;
}

bool readAnimSidecar(const std::filesystem::path& sidecar, std::vector<ClipSettings>& out, std::string& error)
{
    out.clear();
    std::error_code ec;
    if (!std::filesystem::exists(sidecar, ec))
        return true;
    std::vector<std::byte> bytes;
    if (!assets::readFileBytes(sidecar, bytes, error))
        return false;
    const std::string where = core::pathToUtf8(sidecar);
    toml::table root;
    try
    {
        root = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), where);
    }
    catch (const toml::parse_error& e)
    {
        error = std::format("{}: {}", where, e.description());
        return false;
    }
    for (auto&& [key, value] : root)
        if (key.str() != "clip")
            return error = std::format("{}: unknown key '{}'", where, key.str()), false;
    const toml::array* clips = root["clip"].as_array();
    if (clips == nullptr)
        return root.contains("clip") ? (error = std::format("{}: 'clip' must be an array of tables", where), false)
                                     : true;
    std::set<std::string> seen;
    for (const toml::node& n : *clips)
    {
        const toml::table* t = n.as_table();
        if (t == nullptr)
            return error = std::format("{}: 'clip' entries must be tables", where), false;
        ClipSettings c;
        for (auto&& [key, value] : *t)
        {
            const std::string_view k = key.str();
            if (k == "name")
            {
                const auto s = value.value<std::string>();
                if (!s || s->empty())
                    return error = std::format("{}: clip name must be a non-empty string", where), false;
                c.name = *s;
            }
            else if (k == "loop")
            {
                const auto b = value.value<bool>();
                if (!b)
                    return error = std::format("{}: 'loop' must be a boolean", where), false;
                c.loop = *b;
            }
            else if (k == "move_speed")
            {
                const auto d = value.value<double>();
                if (!d || !std::isfinite(*d) || *d < 0.0)
                    return error = std::format("{}: 'move_speed' must be a number >= 0", where), false;
                c.moveSpeed = static_cast<float>(*d);
            }
            else
            {
                return error = std::format("{}: unknown clip key '{}'", where, k), false;
            }
        }
        if (c.name.empty())
            return error = std::format("{}: clip without name", where), false;
        if (!seen.insert(c.name).second)
            return error = std::format("{}: clip '{}' listed twice", where, c.name), false;
        out.push_back(std::move(c));
    }
    return true;
}

// ---- animation sampling --------------------------------------------------------------------------

namespace {

struct SamplerData
{
    std::vector<float> times;
    std::vector<float> values; // keys * comps (CUBICSPLINE: keys * 3 * comps)
    std::size_t comps = 0;
    cgltf_interpolation_type interpolation = cgltf_interpolation_type_linear;
};

bool readSampler(const cgltf_animation_sampler* s, std::size_t comps, SamplerData& out, std::string& error)
{
    out.comps = comps;
    out.interpolation = s->interpolation;
    const cgltf_size n = s->input->count;
    if (n == 0)
        return error = "animation sampler without keys", false;
    out.times.resize(n);
    for (cgltf_size i = 0; i < n; ++i)
    {
        cgltf_float t = 0;
        if (!cgltf_accessor_read_float(s->input, i, &t, 1) || !std::isfinite(t))
            return error = "cannot read animation input", false;
        if (i > 0 && t < out.times[i - 1])
            return error = "animation input times are not increasing", false;
        out.times[i] = t;
    }
    const cgltf_size expected = (s->interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1) * n;
    if (s->output->count < expected)
        return error = "animation output accessor too small", false;
    out.values.resize(expected * comps);
    for (cgltf_size i = 0; i < expected; ++i)
        if (!cgltf_accessor_read_float(s->output, i, out.values.data() + i * comps, comps))
            return error = "cannot read animation output", false;
    return true;
}

void sampleSampler(const SamplerData& s, float t, bool rotation, float* out)
{
    const std::size_t n = s.times.size();
    const std::size_t c = s.comps;
    const bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline;
    const auto value = [&](std::size_t key) { return s.values.data() + (cubic ? (key * 3 + 1) : key) * c; };
    if (n == 1 || t <= s.times.front())
    {
        std::copy_n(value(0), c, out);
    }
    else if (t >= s.times.back())
    {
        std::copy_n(value(n - 1), c, out);
    }
    else
    {
        const auto it = std::upper_bound(s.times.begin(), s.times.end(), t);
        const std::size_t k1 = static_cast<std::size_t>(it - s.times.begin());
        const std::size_t k0 = k1 - 1;
        const float dt = s.times[k1] - s.times[k0];
        const float u = dt > 0.0f ? (t - s.times[k0]) / dt : 0.0f;
        const float* a = value(k0);
        const float* b = value(k1);
        if (s.interpolation == cgltf_interpolation_type_step)
        {
            std::copy_n(a, c, out);
        }
        else if (cubic)
        {
            const float* m0 = s.values.data() + (k0 * 3 + 2) * c; // out-tangent of k0
            const float* m1 = s.values.data() + (k1 * 3 + 0) * c; // in-tangent of k1
            const float u2 = u * u, u3 = u2 * u;
            const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
            for (std::size_t i = 0; i < c; ++i)
                out[i] = h00 * a[i] + h10 * dt * m0[i] + h01 * b[i] + h11 * dt * m1[i];
        }
        else if (rotation)
        {
            const Quat q = assets::slerp(assets::normalize(Quat{a[0], a[1], a[2], a[3]}),
                                         assets::normalize(Quat{b[0], b[1], b[2], b[3]}), u);
            out[0] = q.x;
            out[1] = q.y;
            out[2] = q.z;
            out[3] = q.w;
        }
        else
        {
            for (std::size_t i = 0; i < c; ++i)
                out[i] = a[i] + (b[i] - a[i]) * u;
        }
    }
    if (rotation)
    {
        const Quat q = assets::normalize(Quat{out[0], out[1], out[2], out[3]});
        out[0] = q.x;
        out[1] = q.y;
        out[2] = q.z;
        out[3] = q.w;
    }
}

bool validClipName(std::string_view n) noexcept
{
    if (n.empty() || n.size() > 64 || n.front() == '.')
        return false;
    for (char ch : n)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
              ch == '-' || ch == '.'))
            return false;
    return true;
}

bool keysEqual(const Transform& a, const Transform& b) noexcept
{
    const auto close = [](Vec3 x, Vec3 y) { return assets::length(x - y) <= 1e-3f; };
    return close(a.translation, b.translation) && close(a.scale, b.scale) &&
           std::abs(assets::dot(a.rotation, b.rotation)) >= 1.0f - 1e-5f;
}

} // namespace

bool importAnimations(const std::filesystem::path& gltfPath, const AnimImportOptions& options,
                      std::vector<assets::AnimationClip>& out, std::string& error)
{
    out.clear();
    GltfDocument doc;
    if (!loadDocument(gltfPath, doc, error))
        return false;
    const cgltf_data* data = doc.data.get();
    const std::string fileName = core::pathToUtf8(gltfPath);
    if (data->animations_count == 0)
    {
        if (!options.clips.empty())
            return error = std::format("{}: sidecar lists clip '{}' but the file has no animations", fileName,
                                       options.clips.front().name),
                   false;
        return true;
    }
    if (!(options.sampleRate >= 1.0f && options.sampleRate <= 240.0f))
        return error = "sample rate out of range", false;

    // The skin used by the mesh nodes, else the first skin.
    const cgltf_skin* skin = nullptr;
    for (const cgltf_node* n : collectSceneNodes(data))
        if (n->mesh != nullptr && n->skin != nullptr)
        {
            skin = n->skin;
            break;
        }
    if (skin == nullptr && data->skins_count > 0)
        skin = &data->skins[0];
    if (skin == nullptr)
        return error = std::format("{}: animations need a skin", fileName), false;
    SkeletonImport skel;
    if (!buildSkeleton(skin, skel, error))
        return error = std::format("{}: {}", fileName, error), false;
    const std::size_t boneCount = skel.skeleton.bones.size();

    std::set<std::string> produced;
    for (cgltf_size a = 0; a < data->animations_count; ++a)
    {
        const cgltf_animation& anim = data->animations[a];
        const std::string name =
            anim.name != nullptr && anim.name[0] != '\0' ? std::string(anim.name) : std::format("anim{}", a);
        if (!validClipName(name))
            return error = std::format("{}: animation name '{}' is not a valid clip name", fileName, name), false;
        if (!produced.insert(name).second)
            return error = std::format("{}: duplicate animation name '{}'", fileName, name), false;

        // Channels per bone: [0] translation, [1] rotation, [2] scale.
        std::vector<std::array<const SamplerData*, 3>> channels(boneCount, {nullptr, nullptr, nullptr});
        std::map<const cgltf_animation_sampler*, std::unique_ptr<SamplerData>> samplers;
        float tMin = 0.0f, tMax = 0.0f;
        bool any = false;
        for (cgltf_size c = 0; c < anim.channels_count; ++c)
        {
            const cgltf_animation_channel& ch = anim.channels[c];
            const auto it = skel.nodeToBone.find(ch.target_node);
            if (ch.target_node == nullptr || it == skel.nodeToBone.end() || ch.sampler == nullptr)
                continue; // non-joint targets (e.g. the armature object) are ignored
            int slot = -1;
            std::size_t comps = 3;
            switch (ch.target_path)
            {
            case cgltf_animation_path_type_translation: slot = 0; break;
            case cgltf_animation_path_type_rotation: slot = 1; comps = 4; break;
            case cgltf_animation_path_type_scale: slot = 2; break;
            default: break;
            }
            if (slot < 0)
                continue;
            auto& sd = samplers[ch.sampler];
            if (!sd)
            {
                sd = std::make_unique<SamplerData>();
                if (!readSampler(ch.sampler, comps, *sd, error))
                    return error = std::format("{}: animation '{}': {}", fileName, name, error), false;
            }
            channels[static_cast<std::size_t>(it->second)][static_cast<std::size_t>(slot)] = sd.get();
            if (!any)
            {
                tMin = sd->times.front();
                tMax = sd->times.back();
                any = true;
            }
            tMin = std::min(tMin, sd->times.front());
            tMax = std::max(tMax, sd->times.back());
        }
        if (!any)
            return error = std::format("{}: animation '{}' does not animate any joint", fileName, name), false;

        assets::AnimationClip clip;
        clip.name = name;
        clip.sampleRate = options.sampleRate;
        const float srcDuration = tMax - tMin;
        const long frames = std::lround(static_cast<double>(srcDuration) * options.sampleRate);
        if (frames > 65535)
            return error = std::format("{}: animation '{}' is too long", fileName, name), false;
        clip.frameCount = static_cast<std::uint32_t>(frames) + 1;
        clip.duration = static_cast<float>(clip.frameCount - 1) / clip.sampleRate;
        clip.quantizedRotation = options.quantizeRotations;
        for (const ClipSettings& s : options.clips)
            if (s.name == name)
            {
                clip.loop = s.loop;
                clip.moveSpeed = s.moveSpeed;
            }

        clip.tracks.resize(boneCount);
        for (std::size_t b = 0; b < boneCount; ++b)
        {
            const cgltf_node* node = skel.boneNodes[b];
            assets::AnimationTrack& track = clip.tracks[b];
            track.bone = skel.skeleton.bones[b].name;
            track.keys.resize(clip.frameCount);
            // Rest TRS in glTF space.
            Transform rest;
            if (node->has_matrix)
            {
                if (!assets::decomposeTrs(localMatrix(node), rest.translation, rest.rotation, rest.scale))
                    return error = std::format("{}: joint '{}' matrix is not a TRS", fileName, track.bone), false;
            }
            else
            {
                if (node->has_translation)
                    rest.translation = {node->translation[0], node->translation[1], node->translation[2]};
                if (node->has_rotation)
                    rest.rotation = assets::normalize(
                        Quat{node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]});
                if (node->has_scale)
                    rest.scale = {node->scale[0], node->scale[1], node->scale[2]};
            }
            const bool isRoot = skel.skeleton.bones[b].parent < 0;
            const bool fold = isRoot && !isIdentity(skel.rootFold[b]);
            for (std::uint32_t k = 0; k < clip.frameCount; ++k)
            {
                const float t = clip.frameCount > 1
                                    ? tMin + srcDuration * static_cast<float>(k) / static_cast<float>(clip.frameCount - 1)
                                    : tMin;
                Transform x = rest;
                float v[4];
                if (const SamplerData* s = channels[b][0])
                {
                    sampleSampler(*s, t, false, v);
                    x.translation = {v[0], v[1], v[2]};
                }
                if (const SamplerData* s = channels[b][1])
                {
                    sampleSampler(*s, t, true, v);
                    x.rotation = {v[0], v[1], v[2], v[3]};
                }
                if (const SamplerData* s = channels[b][2])
                {
                    sampleSampler(*s, t, false, v);
                    x.scale = {v[0], v[1], v[2]};
                }
                if (fold)
                {
                    const Mat4 m = skel.rootFold[b] * x.toMatrix();
                    if (!assets::decomposeTrs(m, x.translation, x.rotation, x.scale))
                        return error = std::format("{}: joint '{}' key is not a TRS after folding", fileName, track.bone),
                               false;
                }
                Transform e;
                e.translation = gltfToEngine(x.translation);
                e.rotation = gltfToEngine(assets::normalize(x.rotation));
                e.scale = x.scale;
                if (k > 0 && assets::dot(e.rotation, track.keys[k - 1].rotation) < 0.0f)
                    e.rotation = {-e.rotation.x, -e.rotation.y, -e.rotation.z, -e.rotation.w};
                track.keys[k] = e;
            }
            // Close a loop exactly only if it already is closed (never hide a real seam).
            if (clip.loop && clip.frameCount > 1 && keysEqual(track.keys.front(), track.keys.back()))
                track.keys.back() = track.keys.front();
        }
        out.push_back(std::move(clip));
    }
    for (const ClipSettings& s : options.clips)
        if (!produced.contains(s.name))
            return error = std::format("{}: sidecar lists clip '{}' which is not in the file", fileName, s.name), false;
    return true;
}

} // namespace vo::tools::assetc
