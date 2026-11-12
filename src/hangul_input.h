// Two-set composition adapted from the spruce/ES keyboard reference.
// Provenance and retained third-party notices: docs/KOREAN.md and licenses/.
// UTF-8 byte offsets are always kept on codepoint boundaries.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
namespace hangul {
inline size_t prev(const std::string& s, size_t p) {
    if (!p) return 0;
    --p; while (p && (static_cast<unsigned char>(s[p]) & 0xc0) == 0x80) --p;
    return p;
}
inline size_t next(const std::string& s, size_t p) {
    if (p >= s.size()) return s.size();
    ++p; while (p < s.size() && (static_cast<unsigned char>(s[p]) & 0xc0) == 0x80) ++p;
    return p;
}
inline char32_t decode(const std::string& s, size_t p) {
    unsigned char c = s[p];
    if (c < 128) return c;
    int n = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    char32_t v = c & (n == 2 ? 31 : n == 3 ? 15 : 7);
    for (int j=1; j<n && p+j<s.size(); ++j) v = (v<<6) | (s[p+j]&63);
    return v;
}
inline std::string utf8(char32_t c) {
    std::string s;
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xc0|(c>>6)); s += char(0x80|(c&63)); }
    else if (c < 0x10000) { s += char(0xe0|(c>>12)); s += char(0x80|((c>>6)&63)); s += char(0x80|(c&63)); }
    else { s += char(0xf0|(c>>18)); s += char(0x80|((c>>12)&63)); s += char(0x80|((c>>6)&63)); s += char(0x80|(c&63)); }
    return s;
}
inline const std::u32string initial = U"ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ";
inline const std::u32string medial = U"ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅘㅙㅚㅛㅜㅝㅞㅟㅠㅡㅢㅣ";
inline const std::u32string final = U" ㄱㄲㄳㄴㄵㄶㄷㄹㄺㄻㄼㄽㄾㄿㅀㅁㅂㅄㅅㅆㅇㅈㅊㅋㅌㅍㅎ";
inline const std::vector<std::u32string> vowels = {U"ㅗㅏㅘ",U"ㅗㅐㅙ",U"ㅗㅣㅚ",U"ㅜㅓㅝ",U"ㅜㅔㅞ",U"ㅜㅣㅟ",U"ㅡㅣㅢ"};
inline const std::vector<std::u32string> finals = {U"ㄱㅅㄳ",U"ㄴㅈㄵ",U"ㄴㅎㄶ",U"ㄹㄱㄺ",U"ㄹㅁㄻ",U"ㄹㅂㄼ",U"ㄹㅅㄽ",U"ㄹㅌㄾ",U"ㄹㅍㄿ",U"ㄹㅎㅀ",U"ㅂㅅㅄ"};
inline char32_t join(const std::vector<std::u32string>& table, char32_t a, char32_t b) {
    for (const auto& r : table) if (r[0]==a && r[1]==b) return r[2];
    return 0;
}
inline char32_t syllable(char32_t i, char32_t m, char32_t f=U' ') {
    return 0xac00 + (initial.find(i)*21 + medial.find(m))*28 + final.find(f);
}
inline std::u32string append(char32_t last, char32_t key) {
    if (initial.find(last)!=initial.npos && medial.find(key)!=medial.npos) return {syllable(last,key)};
    if (char32_t v = join(vowels,last,key)) return {v};
    if (last>=0xac00 && last<=0xd7a3) {
        unsigned n=last-0xac00;
        char32_t i=initial[n/588], m=medial[n/28%21], f=final[n%28];
        if (f==U' ') {
            if (key!=U' ' && final.find(key)!=final.npos) return {syllable(i,m,key)};
            if (char32_t v=join(vowels,m,key)) return {syllable(i,v)};
        } else if (medial.find(key)!=medial.npos) {
            char32_t retained=U' ', moved=f;
            for (const auto& r : finals) if (r[2]==f) { retained=r[0]; moved=r[1]; break; }
            return {syllable(i,m,retained),syllable(moved,key)};
        } else if (char32_t v=join(finals,f,key)) return {syllable(i,m,v)};
    }
    return {last,key};
}
inline char32_t key(char c, bool shift) {
    const std::string latin="qwertyuiopasdfghjklzxcvbnm";
    const std::u32string normal=U"ㅂㅈㄷㄱㅅㅛㅕㅑㅐㅔㅁㄴㅇㄹㅎㅗㅓㅏㅣㅋㅌㅊㅍㅠㅜㅡ";
    const std::u32string upper=U"ㅃㅉㄸㄲㅆㅛㅕㅑㅒㅖㅁㄴㅇㄹㅎㅗㅓㅏㅣㅋㅌㅊㅍㅠㅜㅡ";
    size_t n=latin.find(c);
    return n==latin.npos ? static_cast<unsigned char>(c) : (shift ? upper[n] : normal[n]);
}
class Composer {
    struct Edit { size_t start; std::string before, after; };
    std::vector<Edit> history_;
public:
    void commit() { history_.clear(); }
    void add(std::string& text, int& caret, char32_t k) {
        size_t start=caret;
        std::string before, after;
        if (!history_.empty() && caret>0) {
            start=prev(text,caret); before=text.substr(start,caret-start);
            for (char32_t c:append(decode(text,start),k)) after+=utf8(c);
        } else after=utf8(k);
        text.replace(start,caret-start,after); caret=int(start+after.size());
        history_.push_back({start,std::move(before),std::move(after)});
    }
    void backspace(std::string& text, int& caret) {
        if (!history_.empty()) {
            const auto e=history_.back(); history_.pop_back();
            text.replace(e.start,e.after.size(),e.before); caret=int(e.start+e.before.size());
        } else if (caret>0) {
            size_t p=prev(text,caret); text.erase(p,caret-p); caret=int(p);
        }
    }
};
} // namespace hangul
