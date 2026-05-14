// Shared persistence helpers — config-dir resolution, atomic text writes,
// line-based "INSTANCE:N + KEY:value" block parser/formatter.
//
// Used by every per-feature .cfg file in ~/.config/ibkr-trading-app/.
// Header-only / inline; no IB API / ImGui dependency so tests-core can link it
// without pulling in any UI or trading code.

#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace core::services {

// ── Path helpers ──────────────────────────────────────────────────────────────

// Returns ~/.config/ibkr-trading-app, creating it if missing.
// Returns empty string on failure (no $HOME, mkdir failed). Caller treats
// empty as "persistence disabled" — never throws.
inline std::string EnsureConfigDir() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    if (!home || !*home) return std::string();
    std::string dir = std::string(home) + "/.config/ibkr-trading-app";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return std::string();
    return dir;
}

// Convenience: full path to <filename> inside the config dir.
// Returns empty when EnsureConfigDir failed.
inline std::string ConfigFilePath(const std::string& filename) {
    std::string dir = EnsureConfigDir();
    if (dir.empty()) return std::string();
    return dir + "/" + filename;
}

// ── File I/O ──────────────────────────────────────────────────────────────────

// Atomic write: writes to <path>.tmp then renames over <path>. Returns true on
// success. On failure leaves the existing file (if any) untouched.
inline bool AtomicWriteText(const std::string& path, const std::string& contents) {
    if (path.empty()) return false;
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!f.good()) {
            f.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Fallback for the rare cross-device case (shouldn't happen for same-dir
        // .tmp + target, but be defensive).
        std::filesystem::copy_file(tmp, path,
            std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
        return !ec;
    }
    return true;
}

// Reads the entire file into a string. Sets *exists=false if missing (not an
// error — first launch). Returns empty string for missing or unreadable files.
inline std::string ReadTextFile(const std::string& path, bool* exists = nullptr) {
    if (exists) *exists = false;
    if (path.empty()) return std::string();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::string();
    if (exists) *exists = true;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ── State block format ───────────────────────────────────────────────────────
//
//   INSTANCE:0
//   SYMBOL:AAPL
//   USE_RTH:0
//
//   INSTANCE:1
//   SYMBOL:MSFT
//
// Lines starting with '#' are comments. Blank lines separate visually but are
// not required between blocks (a fresh INSTANCE: line starts a new block).
//
// For singletons that don't have an instance index, use a sentinel value
// (e.g. WINDOW:portfolio). The parser exposes "instance" as -1 when an
// INSTANCE: line is absent — caller decides what that means.

struct StateBlock {
    int instance = -1;
    std::string windowName;  // non-empty for singleton blocks ("portfolio", "orders", "wsh")
    std::unordered_map<std::string, std::string> fields;
};

namespace state_io_detail {

inline std::string Trim(std::string_view s) {
    auto first = std::find_if_not(s.begin(), s.end(),
        [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return std::string();
    return std::string(first, last);
}

}  // namespace state_io_detail

// Parses the line-based format. Tolerant: unknown lines, malformed lines,
// and comments are skipped. A trailing block with no INSTANCE: line is still
// emitted with instance=-1 if it has any fields.
inline std::vector<StateBlock> ParseStateBlocks(std::string_view contents) {
    std::vector<StateBlock> blocks;
    StateBlock cur;
    bool curDirty = false;
    auto flush = [&]() {
        if (curDirty || cur.instance != -1 || !cur.windowName.empty()) {
            blocks.push_back(std::move(cur));
            cur = StateBlock{};
            curDirty = false;
        }
    };

    size_t pos = 0;
    while (pos <= contents.size()) {
        size_t end = contents.find('\n', pos);
        if (end == std::string_view::npos) end = contents.size();
        std::string_view line = contents.substr(pos, end - pos);
        pos = end + 1;

        std::string trimmed = state_io_detail::Trim(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#') continue;

        size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;     // malformed — skip
        std::string key = trimmed.substr(0, colon);
        std::string val = trimmed.substr(colon + 1);

        if (key == "INSTANCE") {
            flush();
            try { cur.instance = std::stoi(val); } catch (...) { cur.instance = -1; }
            curDirty = true;
        } else if (key == "WINDOW") {
            flush();
            cur.windowName = val;
            curDirty = true;
        } else {
            cur.fields[std::move(key)] = std::move(val);
            curDirty = true;
        }
    }
    flush();
    return blocks;
}

// Reverse builder. Writes:
//   INSTANCE:<n>      (only when block.instance >= 0)
//   KEY:value         (one line per field, fields emitted in insertion order
//                      isn't guaranteed by unordered_map — sort for stability)
//
// Each block ends with a blank line for human readability.
inline std::string FormatStateBlocks(const std::vector<StateBlock>& blocks) {
    std::ostringstream ss;
    for (const auto& b : blocks) {
        if (!b.windowName.empty())
            ss << "WINDOW:" << b.windowName << "\n";
        else if (b.instance >= 0)
            ss << "INSTANCE:" << b.instance << "\n";
        // Sort keys so the file is deterministic across runs (eases diffing).
        std::vector<std::string> keys;
        keys.reserve(b.fields.size());
        for (const auto& [k, _] : b.fields) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys)
            ss << k << ":" << b.fields.at(k) << "\n";
        ss << "\n";
    }
    return ss.str();
}

// ── Typed accessors with clamping ────────────────────────────────────────────
//
// Missing keys return the default. Out-of-range numerics are clamped to the
// supplied bounds (so a corrupted file with KEY:99999999 doesn't blow up an
// internal index).

inline bool GetBool(const StateBlock& b, const std::string& key, bool dflt) {
    auto it = b.fields.find(key);
    if (it == b.fields.end()) return dflt;
    const std::string& v = it->second;
    if (v.empty()) return dflt;
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES") return true;
    if (v == "0" || v == "false" || v == "FALSE" || v == "no" || v == "NO") return false;
    return dflt;
}

inline int GetInt(const StateBlock& b, const std::string& key, int dflt,
                  int lo = INT_MIN, int hi = INT_MAX) {
    auto it = b.fields.find(key);
    if (it == b.fields.end()) return dflt;
    try {
        long parsed = std::stol(it->second);
        if (parsed < lo) return lo;
        if (parsed > hi) return hi;
        return static_cast<int>(parsed);
    } catch (...) { return dflt; }
}

inline double GetDouble(const StateBlock& b, const std::string& key, double dflt,
                        double lo = -std::numeric_limits<double>::infinity(),
                        double hi =  std::numeric_limits<double>::infinity()) {
    auto it = b.fields.find(key);
    if (it == b.fields.end()) return dflt;
    try {
        double parsed = std::stod(it->second);
        if (std::isnan(parsed) || std::isinf(parsed)) return dflt;
        if (parsed < lo) return lo;
        if (parsed > hi) return hi;
        return parsed;
    } catch (...) { return dflt; }
}

inline std::string GetString(const StateBlock& b, const std::string& key,
                             const std::string& dflt) {
    auto it = b.fields.find(key);
    return (it == b.fields.end()) ? dflt : it->second;
}

// ── Field setters (insertion side, used by Serialize functions) ──────────────

inline void SetBool  (StateBlock& b, const std::string& key, bool v) {
    b.fields[key] = v ? "1" : "0";
}
inline void SetInt   (StateBlock& b, const std::string& key, int v) {
    b.fields[key] = std::to_string(v);
}
inline void SetDouble(StateBlock& b, const std::string& key, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    b.fields[key] = buf;
}
inline void SetString(StateBlock& b, const std::string& key, const std::string& v) {
    b.fields[key] = v;
}

}  // namespace core::services
