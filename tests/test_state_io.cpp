// Tests for core::services::state-io helpers (parse/format/typed accessors).
//
// Filesystem helpers (EnsureConfigDir, AtomicWriteText, ReadTextFile) are
// exercised in a fresh temp dir to stay isolated from the developer's actual
// ~/.config/ibkr-trading-app. We override $HOME (and $USERPROFILE on Windows)
// for the duration of the test block so EnsureConfigDir lands in /tmp.

#include "core/services/state-io.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
// MSVC has no setenv/unsetenv — use _putenv_s. Empty value on _putenv_s
// unsets the variable, matching unsetenv() semantics. POSIX setenv with
// overwrite=1 maps directly.
inline void SetEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}
inline void UnsetEnvVar(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}
}  // namespace

using core::services::AtomicWriteText;
using core::services::ConfigFilePath;
using core::services::EnsureConfigDir;
using core::services::FormatStateBlocks;
using core::services::GetBool;
using core::services::GetDouble;
using core::services::GetInt;
using core::services::GetString;
using core::services::ParseStateBlocks;
using core::services::ReadTextFile;
using core::services::SetBool;
using core::services::SetDouble;
using core::services::SetInt;
using core::services::SetString;
using core::services::StateBlock;

namespace {

// Test fixture: redirects $HOME (and $USERPROFILE on Windows, since
// EnsureConfigDir falls back to it) so EnsureConfigDir hits a temp dir.
struct HomeOverride {
    std::string oldHome;
    std::string oldUserProfile;
    bool hadHome = false;
    bool hadUserProfile = false;
    std::filesystem::path tempHome;

    HomeOverride() {
        if (const char* h = std::getenv("HOME")) {
            hadHome = true;
            oldHome = h;
        }
        if (const char* u = std::getenv("USERPROFILE")) {
            hadUserProfile = true;
            oldUserProfile = u;
        }
        tempHome = std::filesystem::temp_directory_path() /
                   ("ibkr-stateio-test-" + std::to_string(std::rand()));
        std::filesystem::create_directories(tempHome);
        SetEnvVar("HOME", tempHome.string().c_str());
        SetEnvVar("USERPROFILE", tempHome.string().c_str());
    }

    ~HomeOverride() {
        if (hadHome) SetEnvVar("HOME", oldHome.c_str());
        else         UnsetEnvVar("HOME");
        if (hadUserProfile) SetEnvVar("USERPROFILE", oldUserProfile.c_str());
        else                UnsetEnvVar("USERPROFILE");
        std::error_code ec;
        std::filesystem::remove_all(tempHome, ec);
    }
};

}  // namespace

// ── Parser: basic shapes ─────────────────────────────────────────────────────

TEST_CASE("ParseStateBlocks: empty input -> no blocks", "[state-io]") {
    auto blocks = ParseStateBlocks("");
    REQUIRE(blocks.empty());
}

TEST_CASE("ParseStateBlocks: comment-only input -> no blocks", "[state-io]") {
    auto blocks = ParseStateBlocks("# this is a comment\n# another\n");
    REQUIRE(blocks.empty());
}

TEST_CASE("ParseStateBlocks: blank lines ignored", "[state-io]") {
    auto blocks = ParseStateBlocks("\n\n\n");
    REQUIRE(blocks.empty());
}

TEST_CASE("ParseStateBlocks: single block with no INSTANCE line", "[state-io]") {
    auto blocks = ParseStateBlocks("FOO:bar\nBAZ:qux\n");
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].instance == -1);
    REQUIRE(blocks[0].fields.size() == 2);
    REQUIRE(blocks[0].fields["FOO"] == "bar");
    REQUIRE(blocks[0].fields["BAZ"] == "qux");
}

TEST_CASE("ParseStateBlocks: single INSTANCE block", "[state-io]") {
    auto blocks = ParseStateBlocks("INSTANCE:3\nSYMBOL:AAPL\nUSE_RTH:1\n");
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].instance == 3);
    REQUIRE(blocks[0].fields["SYMBOL"] == "AAPL");
    REQUIRE(blocks[0].fields["USE_RTH"] == "1");
}

TEST_CASE("ParseStateBlocks: multiple INSTANCE blocks", "[state-io]") {
    auto blocks = ParseStateBlocks(
        "INSTANCE:0\nSYMBOL:AAPL\n"
        "INSTANCE:1\nSYMBOL:MSFT\nUSE_RTH:0\n"
        "INSTANCE:2\nSYMBOL:TSLA\n");
    REQUIRE(blocks.size() == 3);
    REQUIRE(blocks[0].instance == 0);
    REQUIRE(blocks[0].fields["SYMBOL"] == "AAPL");
    REQUIRE(blocks[1].instance == 1);
    REQUIRE(blocks[1].fields["SYMBOL"] == "MSFT");
    REQUIRE(blocks[1].fields["USE_RTH"] == "0");
    REQUIRE(blocks[2].instance == 2);
    REQUIRE(blocks[2].fields["SYMBOL"] == "TSLA");
}

TEST_CASE("ParseStateBlocks: comments + blank lines interspersed", "[state-io]") {
    auto blocks = ParseStateBlocks(
        "# schema:v1\n"
        "INSTANCE:0\n"
        "\n"
        "# chart 0 stuff\n"
        "SYMBOL:AAPL\n"
        "\n"
        "INSTANCE:1\n"
        "SYMBOL:MSFT\n");
    REQUIRE(blocks.size() == 2);
    REQUIRE(blocks[0].fields["SYMBOL"] == "AAPL");
    REQUIRE(blocks[1].fields["SYMBOL"] == "MSFT");
}

TEST_CASE("ParseStateBlocks: malformed lines skipped (no colon)", "[state-io]") {
    auto blocks = ParseStateBlocks(
        "INSTANCE:0\n"
        "this line has no colon\n"
        "SYMBOL:AAPL\n");
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].fields.size() == 1);
    REQUIRE(blocks[0].fields["SYMBOL"] == "AAPL");
}

TEST_CASE("ParseStateBlocks: value containing colon preserves rightmost colons", "[state-io]") {
    // URL-style values must round-trip — the parser splits on the FIRST colon.
    auto blocks = ParseStateBlocks("BASE_URL:https://api.anthropic.com/v1\n");
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].fields["BASE_URL"] == "https://api.anthropic.com/v1");
}

TEST_CASE("ParseStateBlocks: leading/trailing whitespace trimmed", "[state-io]") {
    auto blocks = ParseStateBlocks("  INSTANCE:0  \n  SYMBOL:AAPL  \n");
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].instance == 0);
    REQUIRE(blocks[0].fields["SYMBOL"] == "AAPL");
}

TEST_CASE("ParseStateBlocks: bad INSTANCE value defaults to -1", "[state-io]") {
    auto blocks = ParseStateBlocks("INSTANCE:not-a-number\nSYMBOL:AAPL\n");
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].instance == -1);
    REQUIRE(blocks[0].fields["SYMBOL"] == "AAPL");
}

TEST_CASE("ParseStateBlocks: WINDOW:name block delimiter", "[state-io]") {
    // Single WINDOW block
    auto blocks = ParseStateBlocks(
        "WINDOW:portfolio\n"
        "SORT_COL:3\n"
        "SORT_ASC:0\n"
    );
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].instance == -1);
    REQUIRE(blocks[0].windowName == "portfolio");
    REQUIRE(GetInt(blocks[0], "SORT_COL", 0) == 3);
    REQUIRE(GetBool(blocks[0], "SORT_ASC", true) == false);
}

TEST_CASE("ParseStateBlocks: multiple WINDOW blocks", "[state-io]") {
    auto blocks = ParseStateBlocks(
        "WINDOW:portfolio\n"
        "SORT_COL:1\n"
        "\n"
        "WINDOW:orders\n"
        "FILTER_SYMBOL:MSFT\n"
        "FILTER_SIDE:1\n"
        "\n"
        "WINDOW:wsh\n"
        "FILTER_TYPE:2\n"
        "SORT_ASC:1\n"
    );
    REQUIRE(blocks.size() == 3);
    REQUIRE(blocks[0].windowName == "portfolio");
    REQUIRE(GetInt(blocks[0], "SORT_COL", 0) == 1);
    REQUIRE(blocks[1].windowName == "orders");
    REQUIRE(GetString(blocks[1], "FILTER_SYMBOL", "") == "MSFT");
    REQUIRE(GetInt(blocks[1], "FILTER_SIDE", 0) == 1);
    REQUIRE(blocks[2].windowName == "wsh");
    REQUIRE(GetInt(blocks[2], "FILTER_TYPE", 0) == 2);
    REQUIRE(GetBool(blocks[2], "SORT_ASC", false) == true);
}

TEST_CASE("ParseStateBlocks: INSTANCE and WINDOW blocks intermixed", "[state-io]") {
    auto blocks = ParseStateBlocks(
        "INSTANCE:0\n"
        "SYMBOL:AAPL\n"
        "\n"
        "WINDOW:portfolio\n"
        "SORT_COL:3\n"
    );
    REQUIRE(blocks.size() == 2);
    REQUIRE(blocks[0].instance == 0);
    REQUIRE(GetString(blocks[0], "SYMBOL", "") == "AAPL");
    REQUIRE(blocks[1].windowName == "portfolio");
    REQUIRE(GetInt(blocks[1], "SORT_COL", 0) == 3);
}

// ── Round-trip: Format → Parse ───────────────────────────────────────────────

TEST_CASE("FormatStateBlocks -> ParseStateBlocks: round-trip", "[state-io]") {
    std::vector<StateBlock> blocks(3);
    blocks[0].instance = 0;
    SetString(blocks[0], "SYMBOL", "AAPL");
    SetBool  (blocks[0], "USE_RTH", true);
    SetInt   (blocks[0], "BB_PERIOD", 20);
    SetDouble(blocks[0], "BB_SIGMA", 2.5);
    blocks[1].instance = 1;
    SetString(blocks[1], "SYMBOL", "MSFT");
    blocks[2].windowName = "portfolio";
    SetInt   (blocks[2], "SORT_COL", 3);
    SetBool  (blocks[2], "SORT_ASC", false);

    std::string text = FormatStateBlocks(blocks);
    auto roundTrip = ParseStateBlocks(text);

    REQUIRE(roundTrip.size() == 3);
    REQUIRE(roundTrip[0].instance == 0);
    REQUIRE(GetString(roundTrip[0], "SYMBOL", "")     == "AAPL");
    REQUIRE(GetBool  (roundTrip[0], "USE_RTH", false) == true);
    REQUIRE(GetInt   (roundTrip[0], "BB_PERIOD", 0)   == 20);
    REQUIRE(GetDouble(roundTrip[0], "BB_SIGMA", 0.0)  == Catch::Approx(2.5));
    REQUIRE(roundTrip[1].instance == 1);
    REQUIRE(GetString(roundTrip[1], "SYMBOL", "")     == "MSFT");
    REQUIRE(roundTrip[2].windowName == "portfolio");
    REQUIRE(GetInt  (roundTrip[2], "SORT_COL", 0)     == 3);
    REQUIRE(GetBool (roundTrip[2], "SORT_ASC", true)  == false);
}

// ── Typed accessors: defaults and clamping ───────────────────────────────────

TEST_CASE("GetBool: accepts multiple truthy/falsy forms", "[state-io]") {
    StateBlock b;
    b.fields["A"] = "1";
    b.fields["B"] = "0";
    b.fields["C"] = "true";
    b.fields["D"] = "FALSE";
    b.fields["E"] = "yes";
    b.fields["F"] = "no";
    b.fields["G"] = "maybe";   // unrecognised → default

    REQUIRE(GetBool(b, "A", false) == true);
    REQUIRE(GetBool(b, "B", true)  == false);
    REQUIRE(GetBool(b, "C", false) == true);
    REQUIRE(GetBool(b, "D", true)  == false);
    REQUIRE(GetBool(b, "E", false) == true);
    REQUIRE(GetBool(b, "F", true)  == false);
    REQUIRE(GetBool(b, "G", true)  == true);   // unrecognised value → dflt
    REQUIRE(GetBool(b, "MISSING", true) == true);   // missing key → dflt
}

TEST_CASE("GetInt: missing key, parse failure, range clamping", "[state-io]") {
    StateBlock b;
    b.fields["OK"]      = "42";
    b.fields["TOO_HI"]  = "9999";
    b.fields["TOO_LO"]  = "-100";
    b.fields["GARBAGE"] = "not-an-int";

    REQUIRE(GetInt(b, "OK", 0)                  == 42);
    REQUIRE(GetInt(b, "MISSING", 7)             == 7);
    REQUIRE(GetInt(b, "TOO_HI", 0, 0, 100)      == 100);
    REQUIRE(GetInt(b, "TOO_LO", 0, 0, 100)      == 0);
    REQUIRE(GetInt(b, "GARBAGE", 5)             == 5);
}

TEST_CASE("GetDouble: missing key, NaN/inf rejection, clamping", "[state-io]") {
    StateBlock b;
    b.fields["OK"]      = "2.5";
    b.fields["TOO_HI"]  = "10.0";
    b.fields["GARBAGE"] = "not-a-num";
    b.fields["NAN_STR"] = "nan";
    b.fields["INF_STR"] = "inf";

    REQUIRE(GetDouble(b, "OK", 0.0)            == Catch::Approx(2.5));
    REQUIRE(GetDouble(b, "MISSING", 1.0)       == Catch::Approx(1.0));
    REQUIRE(GetDouble(b, "TOO_HI", 0.0, 0.0, 5.0) == Catch::Approx(5.0));
    REQUIRE(GetDouble(b, "GARBAGE", 0.5)       == Catch::Approx(0.5));
    REQUIRE(GetDouble(b, "NAN_STR", 0.5)       == Catch::Approx(0.5));
    REQUIRE(GetDouble(b, "INF_STR", 0.5)       == Catch::Approx(0.5));
}

TEST_CASE("GetString: missing key returns default", "[state-io]") {
    StateBlock b;
    b.fields["A"] = "hello";
    REQUIRE(GetString(b, "A", "world")       == "hello");
    REQUIRE(GetString(b, "MISSING", "world") == "world");
    REQUIRE(GetString(b, "MISSING", "")      == "");
}

// ── Filesystem helpers (isolated via $HOME override) ─────────────────────────

TEST_CASE("EnsureConfigDir + atomic write + read round-trip", "[state-io][fs]") {
    HomeOverride home;
    std::string dir = EnsureConfigDir();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(std::filesystem::exists(dir));

    std::string path = ConfigFilePath("test.cfg");
    REQUIRE_FALSE(path.empty());

    REQUIRE(AtomicWriteText(path, "hello\nworld\n"));

    bool exists = false;
    std::string read = ReadTextFile(path, &exists);
    REQUIRE(exists);
    REQUIRE(read == "hello\nworld\n");
}

TEST_CASE("ReadTextFile: missing file returns empty + exists=false", "[state-io][fs]") {
    HomeOverride home;
    EnsureConfigDir();   // create the dir but no file
    std::string path = ConfigFilePath("does-not-exist.cfg");

    bool exists = true;   // start non-default to ensure the call sets it
    std::string read = ReadTextFile(path, &exists);
    REQUIRE_FALSE(exists);
    REQUIRE(read.empty());
}

TEST_CASE("AtomicWriteText leaves no .tmp leftovers on success", "[state-io][fs]") {
    HomeOverride home;
    EnsureConfigDir();
    std::string path = ConfigFilePath("clean.cfg");

    REQUIRE(AtomicWriteText(path, "first\n"));
    REQUIRE(AtomicWriteText(path, "second\n"));   // overwrite

    REQUIRE_FALSE(std::filesystem::exists(path + ".tmp"));

    bool exists = false;
    REQUIRE(ReadTextFile(path, &exists) == "second\n");
    REQUIRE(exists);
}
