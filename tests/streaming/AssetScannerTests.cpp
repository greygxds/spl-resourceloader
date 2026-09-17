#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch_amalgamated.hpp>

#include "resource/Resource.h"
#include "streaming/AssetScanner.h"
#include "streaming/AssetType.h"
#include "streaming/RscHeader.h"
#include "streaming/StreamAsset.h"
#include "tests/streaming/StreamTree.h"

using spl::resource::Resource;
using spl::resource::ResourceId;
using spl::streaming::AssetDisposition;
using spl::streaming::AssetScanner;
using spl::streaming::AssetType;
using spl::streaming::StreamAsset;
using spl::tests::RscHeaderBytes;
using spl::tests::StreamTree;

namespace
{
constexpr ResourceId kOwner{7};

AssetScanner::Result ScanOf(const Resource& resource, bool validateRscHeaders = true,
                            uint32_t assetSizeWarningMiB = 48)
{
    return AssetScanner::Scan(resource, kOwner,
                              AssetScanner::Options{.validateRscHeaders = validateRscHeaders,
                                                    .assetSizeWarningMiB = assetSizeWarningMiB});
}

std::vector<std::string> FileNamesOf(const std::vector<StreamAsset>& assets)
{
    std::vector<std::string> names;
    names.reserve(assets.size());
    for (const StreamAsset& asset : assets)
    {
        names.push_back(asset.fileName);
    }
    return names;
}

const StreamAsset* Find(const std::vector<StreamAsset>& assets, std::string_view fileName)
{
    const auto match = std::ranges::find_if(assets, [fileName](const StreamAsset& asset)
                                            { return asset.fileName == fileName; });
    return match != assets.end() ? &*match : nullptr;
}

bool Mentions(const std::vector<std::string>& messages, std::string_view text)
{
    return std::ranges::any_of(messages, [text](const std::string& message)
                               { return message.find(text) != std::string::npos; });
}
} // namespace

TEST_CASE("AssetScanner: a resource without stream/ has no assets", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("plain");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.empty());
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: recurses into subfolders and flattens the names", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddAsset("map_one", "Prop_A.YDR", 165);
    tree.AddAsset("map_one", "[props]/nested/bench.ydr", 165);
    tree.AddAsset("map_one", "textures/city.ytd", 13);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 3);
    REQUIRE(result.warnings.empty());

    const StreamAsset* prop = Find(result.assets, "prop_a.ydr");
    REQUIRE(prop != nullptr);
    REQUIRE(prop->owner == kOwner);
    REQUIRE(prop->resourceName == "map_one");
    REQUIRE(prop->streamingName == "prop_a");
    REQUIRE(prop->relativePath == "stream/Prop_A.YDR");
    REQUIRE(prop->type == AssetType::Drawable);
    REQUIRE(prop->disposition == AssetDisposition::Planned);
    REQUIRE(prop->fileSizeBytes == 16);

    const StreamAsset* bench = Find(result.assets, "bench.ydr");
    REQUIRE(bench != nullptr);
    REQUIRE(bench->relativePath == "stream/[props]/nested/bench.ydr");

    REQUIRE(Find(result.assets, "city.ytd") != nullptr);
}

TEST_CASE("AssetScanner: dot folders are skipped", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddAsset("map_one", "keep.ydr", 165);
    tree.AddAsset("map_one", ".backup/old.ydr", 165);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(FileNamesOf(result.assets) == std::vector<std::string>{"keep.ydr"});
}

TEST_CASE("AssetScanner: files with no streaming meaning are left out", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddAsset("map_one", "keep.ydr", 165);
    tree.AddFile("map_one", "keep.ydr.stream_raw", "server companion");
    tree.AddFile("map_one", "desktop.ini", "[.ShellClassInfo]");
    tree.AddFile("map_one", "Thumbs.db", "thumbnails");
    tree.AddFile("map_one", "notes.txt", "todo");
    tree.AddFile("map_one", "README.md", "# readme");
    tree.AddFile("map_one", "LICENSE", "no extension at all");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(FileNamesOf(result.assets) == std::vector<std::string>{"keep.ydr"});
    REQUIRE(result.warnings.empty());
    REQUIRE(result.notes.size() == 6);
}

TEST_CASE("AssetScanner: an RPF in stream/ is reported and skipped", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddFile("map_one", "pack.rpf", "RPF7");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().disposition == AssetDisposition::SkippedUnsupported);
    REQUIRE(Mentions(result.warnings, "RPF archive"));
}

TEST_CASE("AssetScanner: a '^' in the name becomes the ped component folder", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("peds");
    tree.AddAsset("peds", "mp_m_freemode_01_mp_m_pack^uppr_diff_000_a_uni.ytd", 13);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    const StreamAsset& asset = result.assets.front();
    REQUIRE(asset.disposition == AssetDisposition::Planned);
    REQUIRE(asset.fileName == "mp_m_freemode_01_mp_m_pack/uppr_diff_000_a_uni.ytd");
    REQUIRE(asset.streamingName == "mp_m_freemode_01_mp_m_pack/uppr_diff_000_a_uni");
    REQUIRE(asset.relativePath == "stream/mp_m_freemode_01_mp_m_pack^uppr_diff_000_a_uni.ytd");
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: only the first '^' is a folder separator", "[streaming]")
{
    REQUIRE(spl::streaming::ToStreamingFileName("a^b^c.ydd") == "a/b^c.ydd");
    REQUIRE(spl::streaming::ToStreamingFileName("prop_a.ydr") == "prop_a.ydr");
}

TEST_CASE("AssetScanner: a drawable collection .ymt is planned", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("peds");
    tree.AddAsset("peds", "mp_f_freemode_01_mp_f_pack.ymt", 2);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().type == AssetType::Metadata);
    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(result.assets.front().streamingName == "mp_f_freemode_01_mp_f_pack");
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: an extension of no known type is planned for its own store", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddFile("map_one", "cloth.yld", "no header check for these");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    const StreamAsset& asset = result.assets.front();
    REQUIRE(asset.type == AssetType::OtherModule);
    REQUIRE(asset.extension == "yld");
    REQUIRE(asset.disposition == AssetDisposition::Planned);
    REQUIRE_FALSE(asset.rsc.has_value());
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: files that never belong in stream/ are listed with a reason",
          "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddFile("map_one", "preview.png", "not a game file");
    tree.AddFile("map_one", "main.ysc", "a script");
    tree.AddFile("map_one", "busy_spinner.gfx", "the game's own spinner");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 3);
    for (const StreamAsset& asset : result.assets)
    {
        CHECK(asset.disposition == AssetDisposition::SkippedUnsupported);
    }
    REQUIRE(Find(result.assets, "preview.png")->type == AssetType::Unknown);
    REQUIRE(Find(result.assets, "main.ysc")->dispositionReason.find("scripts") !=
            std::string::npos);
    REQUIRE(result.warnings.empty()); // nothing here is the user's problem to fix
}

TEST_CASE("AssetScanner: particle effects, navmeshes and path nodes are planned", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddAsset("map_one", "effects.ypt", 68);
    tree.AddAsset("map_one", "navmesh[10][20].ynv", 2);
    tree.AddAsset("map_one", "nodes0.ynd", 1);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 3);
    for (const StreamAsset& asset : result.assets)
    {
        CHECK(asset.disposition == AssetDisposition::Planned);
    }
    REQUIRE(Find(result.assets, "effects.ypt")->type == AssetType::ParticleFx);
}

TEST_CASE("AssetScanner: a clip dictionary is planned", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("weapon");
    tree.AddAsset("weapon", "anim@melee@holster.ycd", 46);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().type == AssetType::ClipDictionary);
    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: a PSO packfile manifest is planned", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddPsoFile("map_one", "_manifest.ymf");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().type == AssetType::PackfileManifest);
    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(result.assets.front().rsc->IsPsoMetadata());
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: a PSO scenario ymt is planned", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddPsoFile("map_one", "ymt/1635093454.ymt"); // a scenario region, as CodeWalker saves it

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().type == AssetType::Metadata);
    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(result.assets.front().rsc->IsPsoMetadata());
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: only a ymf or ymt may be a PSO file", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddPsoFile("map_one", "props.ydr");
    tree.AddFile("map_one", "broken.ymf", "<CPackFileMetaData />");
    tree.AddFile("map_one", "broken.ymt", "<CScenarioPointRegion />");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 3);
    for (const StreamAsset& asset : result.assets)
    {
        REQUIRE(asset.disposition == AssetDisposition::SkippedInvalid);
    }
    REQUIRE(result.warnings.size() == 3);
}

TEST_CASE("AssetScanner: a file without an RSC header is skipped as invalid", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddFile("map_one", "old/broken.ydr", "<Drawable>not compiled</Drawable>");
    tree.AddAsset("map_one", "good.ydr", 165);

    const AssetScanner::Result result = ScanOf(resource);

    const StreamAsset* broken = Find(result.assets, "broken.ydr");
    REQUIRE(broken != nullptr);
    REQUIRE(broken->disposition == AssetDisposition::SkippedInvalid);
    REQUIRE(broken->rsc.has_value());
    REQUIRE_FALSE(broken->rsc->IsResource());
    REQUIRE(Mentions(result.warnings, "not a compiled RAGE resource"));

    const StreamAsset* good = Find(result.assets, "good.ydr");
    REQUIRE(good != nullptr);
    REQUIRE(good->disposition == AssetDisposition::Planned);
    REQUIRE(good->rsc->format == spl::streaming::RscHeader::Format::Rsc7);
}

TEST_CASE("AssetScanner: validation off keeps a file without a header", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddFile("map_one", "broken.ydr", "<Drawable>not compiled</Drawable>");

    const AssetScanner::Result result = ScanOf(resource, /*validateRscHeaders*/ false);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: an unexpected resource version is a warning only", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddAsset("map_one", "future.ydr", 200);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(Mentions(result.warnings, "resource version 200"));
}

TEST_CASE("AssetScanner: an oversized asset is a warning only", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    // 8 pages of 16 MiB of virtual memory, against a 48 MiB threshold.
    tree.AddAsset("map_one", "huge.ydr", 165, (8U << 17) | 0xFU, 0);

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.front().disposition == AssetDisposition::Planned);
    REQUIRE(Mentions(result.warnings, "warning threshold"));
}

TEST_CASE("AssetScanner: a gfx file needs no RSC header", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("ui");
    tree.AddFile("ui", "hud.gfx", "CWS flash movie");

    const AssetScanner::Result result = ScanOf(resource);

    REQUIRE(result.assets.size() == 1);
    REQUIRE(result.assets.front().type == AssetType::Scaleform);
    REQUIRE_FALSE(result.assets.front().rsc.has_value());
    REQUIRE(result.warnings.empty());
}

TEST_CASE("AssetScanner: scan order is sorted, so duplicates resolve the same way twice",
          "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("map_one");
    tree.AddAsset("map_one", "b_second/prop.ydr", 165);
    tree.AddAsset("map_one", "A_first/prop.ydr", 165);

    const AssetScanner::Result first = ScanOf(resource);
    const AssetScanner::Result second = ScanOf(resource);

    REQUIRE(first.assets.size() == 2);
    REQUIRE(first.assets.front().relativePath == "stream/A_first/prop.ydr");
    REQUIRE(first.assets.front().relativePath == second.assets.front().relativePath);
}

TEST_CASE("AssetScanner: an empty file is skipped as invalid", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("broken");
    tree.AddFile("broken", "empty.ydr", "");

    const AssetScanner::Result result = ScanOf(resource);

    const StreamAsset* asset = Find(result.assets, "empty.ydr");
    REQUIRE(asset != nullptr);
    CHECK(asset->disposition == AssetDisposition::SkippedInvalid);
    CHECK(Mentions(result.warnings, "is empty"));
}

TEST_CASE("AssetScanner: a file above the size limit is skipped as invalid", "[streaming]")
{
    StreamTree tree;
    const Resource resource = tree.AddStreamResource("huge");
    tree.AddAsset("huge", "huge.ydr", 165);
    std::filesystem::resize_file(tree.Path() / "huge" / "stream" / "huge.ydr",
                                 AssetScanner::kMaxFileSizeBytes + 1);

    const AssetScanner::Result result = ScanOf(resource);

    const StreamAsset* asset = Find(result.assets, "huge.ydr");
    REQUIRE(asset != nullptr);
    CHECK(asset->disposition == AssetDisposition::SkippedInvalid);
    CHECK(Mentions(result.warnings, "above the 256 MiB limit"));
}
