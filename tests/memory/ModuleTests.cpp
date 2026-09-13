#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catch_amalgamated.hpp>

#include "memory/Module.h"

using namespace spl::memory;

// The test executable is a perfectly ordinary PE image, so it is the module under test.
TEST_CASE("Module: Main describes the test executable", "[memory]")
{
    const Module main = Module::Main();

    CHECK(main.GetBase() != 0);
    CHECK(main.GetSizeBytes() > 0);
    CHECK(main.GetFileName() == "spl_tests.exe");
    CHECK(main.GetPath().is_absolute());
    CHECK_FALSE(main.GetSections().empty());
}

TEST_CASE("Module: Contains covers the image and nothing else", "[memory]")
{
    const Module main = Module::Main();

    CHECK(main.Contains(main.GetBase()));
    CHECK(main.Contains(main.GetBase() + main.GetSizeBytes() - 1));
    CHECK_FALSE(main.Contains(main.GetBase() - 1));
    CHECK_FALSE(main.Contains(main.GetBase() + main.GetSizeBytes()));
    CHECK_FALSE(main.Contains(0));
}

TEST_CASE("Module: the code section is found and executable", "[memory]")
{
    const Module main = Module::Main();

    const Section* text = main.FindSection(".text");
    REQUIRE(text != nullptr);
    CHECK(text->executable);
    CHECK(text->sizeBytes > 0);
    CHECK(main.Contains(text->begin));
    CHECK(main.FindSection(".nosuchsection") == nullptr);
}

TEST_CASE("Module: scan regions are the executable sections", "[memory]")
{
    const Module main = Module::Main();

    const std::vector<Section> executable = main.GetExecutableSections();
    REQUIRE_FALSE(executable.empty()); // an image with no code would not be running
    CHECK(main.GetScanRegions().size() == executable.size());
    CHECK(std::ranges::all_of(main.GetScanRegions(),
                              [](const Section& section) { return section.sizeBytes > 0; }));
}

TEST_CASE("Module: a section's bytes start at the section address", "[memory]")
{
    const Module main = Module::Main();

    const Section* text = main.FindSection(".text");
    REQUIRE(text != nullptr);
    const std::span<const std::byte> bytes = main.GetBytes(*text);
    CHECK(reinterpret_cast<uintptr_t>(bytes.data()) == text->begin);
    CHECK(bytes.size() == text->sizeBytes);
}

TEST_CASE("Module: Find is nullopt for a module that is not loaded", "[memory]")
{
    CHECK(Module::Find(L"kernel32.dll").has_value()); // always loaded
    CHECK_FALSE(Module::Find(L"spl_no_such_module.dll").has_value());
}

TEST_CASE("Module: data scan regions are the non-executable sections", "[memory]")
{
    const Module main = Module::Main();

    const std::vector<Section> data = main.GetScanRegions(SectionKind::Data);
    REQUIRE_FALSE(data.empty()); // .rdata at the very least
    CHECK(std::ranges::none_of(data, [](const Section& section) { return section.executable; }));
    CHECK(main.FindSection(".rdata") != nullptr);
    CHECK(main.GetScanRegions(SectionKind::Any).size() ==
          main.GetScanRegions(SectionKind::Code).size() + data.size());
}
