// One-shot: emit data/lang/languages.json + <code>.json from the compiled table.
// Build:  g++ -std=c++17 tools/dump_i18n.cpp src/i18n.cpp -o /tmp/dump_i18n
// Run:    /tmp/dump_i18n <out_dir>   (writes <out_dir>/languages.json + <code>.json)
#include "../src/i18n.h"
#include <cstdio>
#include <string>
using namespace i18n;

static std::string esc(const char* s) {
    std::string o;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
        else o += *p;
    }
    return o;
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : ".";
    // manifest
    {
        std::string path = dir + "/languages.json";
        FILE* f = std::fopen(path.c_str(), "w");
        std::fprintf(f, "{\n  \"languages\": [\n");
        for (int i = 0; i < language_count(); ++i) {
            std::fprintf(f, "    { \"code\": \"%s\", \"name\": \"%s\", \"hl\": \"%s\", \"gl\": \"%s\" }%s\n",
                         language_hl(i), esc(language_name(i)).c_str(),
                         language_hl(i), language_gl(i),
                         i + 1 < language_count() ? "," : "");
        }
        std::fprintf(f, "  ]\n}\n");
        std::fclose(f);
        std::printf("wrote %s\n", path.c_str());
    }
    // one file per language
    for (int i = 0; i < language_count(); ++i) {
        set_language(i);
        std::string path = dir + "/" + language_hl(i) + ".json";
        FILE* f = std::fopen(path.c_str(), "w");
        std::fprintf(f, "{\n");
        for (int k = 0; k < str_count(); ++k) {
            const char* key = key_name((Str)k);
            const char* val = tr((Str)k);
            std::fprintf(f, "  \"%s\": \"%s\"%s\n", key, esc(val).c_str(),
                         k + 1 < str_count() ? "," : "");
        }
        std::fprintf(f, "}\n");
        std::fclose(f);
        std::printf("wrote %s (%d strings)\n", path.c_str(), str_count());
    }
    return 0;
}
