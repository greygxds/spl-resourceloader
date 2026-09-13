#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "tests/TempTree.h"
#include "util/Glob.h"

using spl::tests::TempDir;
using spl::util::GlobFiles;
using spl::util::IsLiteralPattern;
using spl::util::MatchesGlob;

TEST_CASE("Glob: a star matches inside one segment only", "[util]")
{
    REQUIRE(MatchesGlob("*.ytyp", "props.ytyp"));
    REQUIRE(MatchesGlob("stream/*.ytyp", "stream/props.ytyp"));
    REQUIRE_FALSE(MatchesGlob("stream/*.ytyp", "stream/sub/props.ytyp"));
    REQUIRE_FALSE(MatchesGlob("*.ytyp", "props.ydr"));
}

TEST_CASE("Glob: a question mark matches one character", "[util]")
{
    REQUIRE(MatchesGlob("a?c.meta", "abc.meta"));
    REQUIRE_FALSE(MatchesGlob("a?c.meta", "ac.meta"));
    REQUIRE_FALSE(MatchesGlob("a?c", "a/c"));
}

TEST_CASE("Glob: a double star spans any number of segments", "[util]")
{
    REQUIRE(MatchesGlob("stream/**/*.ytyp", "stream/props.ytyp"));
    REQUIRE(MatchesGlob("stream/**/*.ytyp", "stream/sub/props.ytyp"));
    REQUIRE(MatchesGlob("stream/**/*.ytyp", "stream/a/b/c/props.ytyp"));
    REQUIRE(MatchesGlob("**/*.meta", "data/x.meta"));
    REQUIRE(MatchesGlob("**", "anything/at/all.txt"));
}

TEST_CASE("Glob: matching ignores case", "[util]")
{
    REQUIRE(MatchesGlob("Stream/*.YTYP", "stream/props.ytyp"));
    REQUIRE(MatchesGlob("stream/*.ytyp", "STREAM/PROPS.YTYP"));
}

TEST_CASE("Glob: a literal pattern matches only itself", "[util]")
{
    REQUIRE(MatchesGlob("data/handling.meta", "data/handling.meta"));
    REQUIRE_FALSE(MatchesGlob("data/handling.meta", "data/other.meta"));
}

TEST_CASE("Glob: IsLiteralPattern spots wildcards", "[util]")
{
    REQUIRE(IsLiteralPattern("stream/props.ytyp"));
    REQUIRE_FALSE(IsLiteralPattern("stream/*.ytyp"));
    REQUIRE_FALSE(IsLiteralPattern("a?.meta"));
}

TEST_CASE("Glob: GlobFiles returns sorted relative paths", "[util]")
{
    const TempDir dir;
    dir.WriteFile("stream/b.ytyp", "x");
    dir.WriteFile("stream/a.ytyp", "x");
    dir.WriteFile("stream/sub/c.ytyp", "x");
    dir.WriteFile("stream/ignore.txt", "x");

    REQUIRE(GlobFiles(dir.Path(), "stream/*.ytyp") ==
            std::vector<std::string>{"stream/a.ytyp", "stream/b.ytyp"});
    REQUIRE(GlobFiles(dir.Path(), "stream/**/*.ytyp") ==
            std::vector<std::string>{"stream/a.ytyp", "stream/b.ytyp", "stream/sub/c.ytyp"});
}

TEST_CASE("Glob: GlobFiles matches a literal path", "[util]")
{
    const TempDir dir;
    dir.WriteFile("data/handling.meta", "x");

    REQUIRE(GlobFiles(dir.Path(), "data/handling.meta") ==
            std::vector<std::string>{"data/handling.meta"});
    REQUIRE(GlobFiles(dir.Path(), "data/absent.meta").empty());
}

TEST_CASE("Glob: GlobFiles ignores directories and a missing root", "[util]")
{
    const TempDir dir;
    dir.AddDirectory("stream/folder.ytyp");
    dir.WriteFile("stream/real.ytyp", "x");

    REQUIRE(GlobFiles(dir.Path(), "stream/*.ytyp") == std::vector<std::string>{"stream/real.ytyp"});
    REQUIRE(GlobFiles(dir.Path() / "absent", "*").empty());
}
