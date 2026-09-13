#include <chrono>
#include <string>

#include <catch_amalgamated.hpp>
#include <spdlog/fmt/fmt.h>

#include "config/LoaderConfig.h"
#include "resource/ResourceManager.h"
#include "streaming/StreamingPlan.h"
#include "tests/streaming/StreamTree.h"

// Hidden ("[.]"): it writes 50 000 files, which takes far longer than the check itself. Run it
// against a Release build with: spl_tests.exe "[benchmark]"
TEST_CASE("Discovery: 500 resources with 50 000 files plan in under 2 s", "[.][benchmark]")
{
    constexpr int kResources = 500;
    constexpr int kFilesPerResource = 100;

    spl::tests::StreamTree tree;
    const std::string header = spl::tests::RscHeaderBytes(165);
    for (int resource = 0; resource < kResources; ++resource)
    {
        const std::string name = fmt::format("pack_{:03}", resource);
        tree.AddResource(name);
        for (int file = 0; file < kFilesPerResource; ++file)
        {
            tree.AddFile(name, fmt::format("props/{}_{:03}.ydr", name, file), header);
        }
    }

    const spl::config::LoaderConfig config;
    const auto started = std::chrono::steady_clock::now();
    spl::resource::ResourceManager manager;
    manager.Discover(config, tree.Root());
    const auto discoveredMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    const spl::streaming::StreamingPlan plan = spl::streaming::StreamingPlan::Build(
        manager.GetResources(), config.streaming, config.diagnostics);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();

    INFO("discovery and plan took " << elapsedMs << " ms");
    CHECK(plan.Early().size() == static_cast<std::size_t>(kResources * kFilesPerResource));
    CHECK(elapsedMs < 2000);
    WARN("discovery and plan took " << elapsedMs << " ms, of which discovery " << discoveredMs
                                    << " ms");
}
