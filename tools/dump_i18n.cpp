// Regenerate the data/lang string files after adding keys to i18n::Str.
// Build:  g++ -std=c++17 tools/dump_i18n.cpp src/i18n.cpp -o /tmp/dump_i18n
// Run:    /tmp/dump_i18n data/lang
//
// - en.json is rewritten with ALL keys from the baked-in English table (it is
//   the canonical template translators copy from).
// - Every other <code>.json listed in languages.json keeps its existing
//   translations, re-emitted in enum order; keys no longer in the enum are
//   dropped (warned); keys it never translated are NOT added — the runtime
//   falls back to English for those, so partial files stay partial.
// - languages.json is never rewritten (it's hand-maintained); it is only
//   created, with the single built-in English entry, if missing entirely.
#include "../src/i18n.h"
#include "../third_party/json.hpp"
#include <cstdio>
#include <fstream>
#include <string>

using json = nlohmann::json;
using namespace i18n;

// One key/value row; the value is serialized by json so escaping is correct.
static void emit(FILE* f, const char* key, const std::string& val, bool last) {
    std::fprintf(f, "  \"%s\": %s%s\n", key, json(val).dump().c_str(), last ? "" : ",");
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    // NOTE: do NOT call i18n::load() — with nothing loaded tr() serves the
    // baked-in English table, which is exactly the template we want for en.json.

    std::ifstream mf(dir + "/languages.json");
    if (!mf) {
        std::string path = dir + "/languages.json";
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        std::fprintf(f, "{\n  \"languages\": [\n"
                        "    { \"code\": \"en\", \"name\": \"English\", \"hl\": \"en\", \"gl\": \"US\" }\n"
                        "  ]\n}\n");
        std::fclose(f);
        std::printf("wrote %s (bootstrap: English only)\n", path.c_str());
    }

    json manifest;
    {
        std::ifstream in(dir + "/languages.json");
        manifest = json::parse(in, nullptr, false);
    }
    if (manifest.is_discarded() || !manifest.contains("languages")) {
        std::fprintf(stderr, "%s/languages.json is unreadable — fix it first\n", dir.c_str());
        return 1;
    }

    for (const auto& e : manifest["languages"]) {
        std::string code = e.value("code", "");
        if (code.empty()) continue;
        std::string path = dir + "/" + code + ".json";
        bool is_en = code == "en";

        json old = json::object();
        if (!is_en) {
            std::ifstream in(path);
            if (in) {
                old = json::parse(in, nullptr, false);
                if (old.is_discarded() || !old.is_object()) {
                    std::fprintf(stderr, "%s is unreadable — skipped (fix or delete it)\n", path.c_str());
                    continue;
                }
            }
        }

        // Collect the rows first so the last one can omit its comma.
        std::vector<std::pair<const char*, std::string>> rows;
        for (int k = 0; k < str_count(); ++k) {
            const char* key = key_name((Str)k);
            if (is_en) rows.emplace_back(key, tr((Str)k));
            else if (auto it = old.find(key); it != old.end() && it->is_string())
                rows.emplace_back(key, it->get<std::string>());
        }
        // Anything left in the file but not in the enum is stale — drop, loudly.
        for (auto it = old.begin(); it != old.end(); ++it) {
            bool known = false;
            for (int k = 0; k < str_count() && !known; ++k)
                known = it.key() == key_name((Str)k);
            if (!known) std::printf("%s: dropped stale key \"%s\"\n", path.c_str(), it.key().c_str());
        }

        FILE* f = std::fopen(path.c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        std::fprintf(f, "{\n");
        for (size_t i = 0; i < rows.size(); ++i)
            emit(f, rows[i].first, rows[i].second, i + 1 == rows.size());
        std::fprintf(f, "}\n");
        std::fclose(f);
        std::printf("wrote %s (%zu of %d strings)\n", path.c_str(), rows.size(), str_count());
    }
    return 0;
}
