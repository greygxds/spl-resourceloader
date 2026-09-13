#include <cstddef>
#include <filesystem>
#include <string>

#include <catch_amalgamated.hpp>

#include "memory/ImageFile.h"
#include "memory/Module.h"
#include "tests/TempTree.h"

using spl::memory::ImageFile;
using spl::memory::Module;

TEST_CASE("ImageFile: lays out the test executable like the loader did", "[memory]")
{
    const Module running = Module::Main();
    const auto loaded = ImageFile::Load(running.GetPath());
    REQUIRE(loaded);

    const Module& image = loaded.GetValue().GetModule();
    CHECK(image.GetSizeBytes() == running.GetSizeBytes());
    CHECK(image.GetBase() != running.GetBase());
    CHECK(image.GetPath() == running.GetPath());
    REQUIRE(image.GetSections().size() == running.GetSections().size());

    // The bytes differ where relocations were applied to the running copy, so compare the
    // layout rather than the contents.
    for (std::size_t index = 0; index < image.GetSections().size(); ++index)
    {
        const auto& ours = image.GetSections()[index];
        const auto& theirs = running.GetSections()[index];
        CHECK(ours.name == theirs.name);
        CHECK(ours.begin - image.GetBase() == theirs.begin - running.GetBase());
        CHECK(ours.executable == theirs.executable);
    }
}

TEST_CASE("ImageFile: refuses what is not a 64-bit PE", "[memory]")
{
    const spl::tests::TempDir dir;
    dir.WriteFile("notes.txt", "MZ but nothing else");

    CHECK_FALSE(ImageFile::Load(dir.Path() / "notes.txt"));
    CHECK_FALSE(ImageFile::Load(dir.Path() / "missing.exe"));
}

TEST_CASE("ImageFile: captures a running process's main module", "[memory]")
{
    const Module running = Module::Main();
    const auto captured = ImageFile::Capture(running.GetPath().filename().wstring());
    REQUIRE(captured);

    const Module& image = captured.GetValue().GetModule();
    CHECK(image.GetSizeBytes() == running.GetSizeBytes());
    CHECK(image.GetPath().filename() == running.GetPath().filename());
    CHECK_FALSE(ImageFile::Capture(L"spl_no_such_process.exe"));
}
