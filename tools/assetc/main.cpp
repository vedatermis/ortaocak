// assetc - offline asset compiler: glTF -> .vmesh/.vanim, terrain description -> .vterrain, PNG copy.
// Formats: docs/ASSET_FORMATS.md. Pipeline and conventions: docs/ASSET_PIPELINE.md.
//
// Exit codes: 0 ok (also: no arguments = usage), 1 bad arguments, 2 conversion/IO failure.

#include "tools/assetc/Build.h"
#include "tools/assetc/GltfImport.h"
#include "tools/assetc/Info.h"
#include "tools/assetc/TerrainCompiler.h"

#include "shared/assets/Assets.h"
#include "shared/core/Log.h"
#include "shared/core/Paths.h"
#include "shared/core/Version.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace ac = vo::tools::assetc;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailed = 2;

void printUsage()
{
    std::printf("assetc %s (%s %s)\n", vo::version::kString, vo::version::kProductName, vo::version::kBuildConfig);
    std::printf("usage:\n"
                "  assetc mesh <in.glb> <out.vmesh> [--root <assets_src>]\n"
                "  assetc anim <in.glb> <outdir> [--quantize]      one <clip>.vanim per glTF animation\n"
                "  assetc terrain <desc.terrain.toml> <out.vterrain>\n"
                "  assetc info <file.vmesh|.vanim|.vterrain>\n"
                "  assetc build <assets_src> <assets> [--force] [--quantize]\n");
}

int fail(const std::string& error)
{
    VO_LOG_ERROR("{}", error);
    return kExitFailed;
}

bool writeOut(const fs::path& path, const std::vector<std::byte>& bytes, std::string& error)
{
    return vo::assets::writeFileBytes(path, bytes, error);
}

int run(const std::vector<std::string>& args)
{
    if (args.empty())
    {
        printUsage();
        return kExitOk;
    }
    const std::string& cmd = args[0];
    std::vector<std::string> pos;
    bool quantize = false, force = false;
    fs::path root;
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--quantize")
            quantize = true;
        else if (args[i] == "--force")
            force = true;
        else if (args[i] == "--root" && i + 1 < args.size())
            root = vo::core::pathFromUtf8(args[++i]);
        else if (args[i].starts_with("--"))
        {
            std::fprintf(stderr, "assetc: unknown option '%s'\n", args[i].c_str());
            return kExitUsage;
        }
        else
            pos.push_back(args[i]);
    }
    const auto need = [&](std::size_t n) {
        if (pos.size() == n)
            return true;
        printUsage();
        return false;
    };
    std::string error;

    if (cmd == "-h" || cmd == "--help" || cmd == "help" || cmd == "--version")
    {
        printUsage();
        return kExitOk;
    }
    if (cmd == "mesh")
    {
        if (!need(2))
            return kExitUsage;
        ac::MeshImportOptions o;
        o.assetRoot = root;
        vo::assets::MeshData mesh;
        std::vector<std::byte> bytes;
        if (!ac::importMesh(vo::core::pathFromUtf8(pos[0]), o, mesh, error) ||
            !vo::assets::serializeMesh(mesh, bytes, error) || !writeOut(vo::core::pathFromUtf8(pos[1]), bytes, error))
            return fail(error);
        std::printf("wrote %s (%u vertices, %u triangles, %zu submeshes, %zu bones)\n", pos[1].c_str(),
                    mesh.vertexCount, mesh.indexCount() / 3, mesh.submeshes.size(), mesh.skeleton.bones.size());
        return kExitOk;
    }
    if (cmd == "anim")
    {
        if (!need(2))
            return kExitUsage;
        const fs::path in = vo::core::pathFromUtf8(pos[0]);
        ac::AnimImportOptions o;
        o.quantizeRotations = quantize;
        std::vector<vo::assets::AnimationClip> clips;
        if (!ac::readAnimSidecar(ac::animSidecarPath(in), o.clips, error) ||
            !ac::importAnimations(in, o, clips, error))
            return fail(error);
        if (clips.empty())
            return fail(vo::core::pathToUtf8(in) + ": no animations");
        for (vo::assets::AnimationClip& clip : clips)
        {
            const fs::path out = vo::core::pathFromUtf8(pos[1]) / vo::core::pathFromUtf8(clip.name + ".vanim");
            vo::assets::AnimationFile file;
            file.clips.push_back(std::move(clip));
            std::vector<std::byte> bytes;
            if (!vo::assets::serializeAnimation(file, bytes, error) || !writeOut(out, bytes, error))
                return fail(error);
            std::printf("wrote %s\n", vo::core::pathToUtf8(out).c_str());
        }
        return kExitOk;
    }
    if (cmd == "terrain")
    {
        if (!need(2))
            return kExitUsage;
        vo::assets::TerrainData t;
        std::vector<std::byte> bytes;
        if (!ac::compileTerrain(vo::core::pathFromUtf8(pos[0]), t, error) ||
            !vo::assets::serializeTerrain(t, bytes, error) || !writeOut(vo::core::pathFromUtf8(pos[1]), bytes, error))
            return fail(error);
        std::printf("wrote %s (%ux%u heights, %zu spawns, %zu objects)\n", pos[1].c_str(), t.resolution, t.resolution,
                    t.spawns.size(), t.objects.size());
        return kExitOk;
    }
    if (cmd == "info")
    {
        if (!need(1))
            return kExitUsage;
        std::string text;
        if (!ac::describeFile(vo::core::pathFromUtf8(pos[0]), text, error))
            return fail(error);
        std::fputs(text.c_str(), stdout);
        return kExitOk;
    }
    if (cmd == "build")
    {
        if (!need(2))
            return kExitUsage;
        ac::BuildOptions o;
        o.force = force;
        o.quantizeRotations = quantize;
        std::error_code ec;
        const fs::path exe = vo::core::executableDir();
        for (const char* name : {"assetc.exe", "assetc"})
            if (fs::exists(exe / name, ec))
            {
                o.toolTime = fs::last_write_time(exe / name, ec);
                break;
            }
        ac::BuildStats stats;
        const bool ok = ac::buildAssets(vo::core::pathFromUtf8(pos[0]), vo::core::pathFromUtf8(pos[1]), o, stats);
        std::printf("assetc build: %d converted, %d copied, %d up to date, %d failed\n", stats.converted,
                    stats.copied, stats.skipped, stats.failed);
        return ok ? kExitOk : kExitFailed;
    }
    std::fprintf(stderr, "assetc: unknown command '%s'\n", cmd.c_str());
    printUsage();
    return kExitUsage;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i)
            args.push_back(vo::core::pathToUtf8(fs::path(argv[i])));
        return run(args);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "assetc: fatal: %s\n", e.what());
        return kExitFailed;
    }
}
