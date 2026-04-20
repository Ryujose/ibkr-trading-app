#pragma once

#include <string>
#include <cstring>
#include <cstdlib>

// Wall Street Horizon (WSH) corporate event data.
// IB returns one JSON blob per event via wshEventData(). This header provides
// a minimal hand-rolled extractor — no external JSON dependency.
namespace WshData {

struct WshEvent {
    int         conId;
    std::string date;         // "YYYY-MM-DD" (truncated from IB's ISO datetime)
    std::string type;         // "Earnings", "Dividend", "Split", etc.
    std::string description;
    std::string importance;   // "High", "Medium", "Low"
};

// Extract a JSON string field: "key":"value" → value
inline std::string ExtractStr(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return {};
    pos += pat.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

// Extract a JSON numeric field: "key":number → number
inline int ExtractInt(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return 0;
    pos += pat.size();
    return std::atoi(json.c_str() + pos);
}

// Parse a single WSH event JSON blob into a WshEvent.
// IB date format: "2023-10-25T00:00:00" — we keep only YYYY-MM-DD (first 10 chars).
inline WshEvent ParseWshEvent(const std::string& json) {
    WshEvent e;
    e.conId       = ExtractInt(json, "conid");
    e.type        = ExtractStr(json, "event_type");
    e.description = ExtractStr(json, "description");
    e.importance  = ExtractStr(json, "importance");

    std::string rawDate = ExtractStr(json, "date");
    // Truncate ISO datetime to date portion: "2023-10-25T..." → "2023-10-25"
    if (rawDate.size() > 10 && rawDate[10] == 'T')
        rawDate = rawDate.substr(0, 10);
    e.date = rawDate;
    return e;
}

}  // namespace WshData
