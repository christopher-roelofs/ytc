#include "hangul_input.h"
#include <cassert>
#include <iostream>
struct Input {
    std::string s; int p=0; hangul::Composer c;
    void add(const std::u32string& keys) { for(auto k:keys) c.add(s,p,k); }
};
int main() {
    for(auto test:std::vector<std::pair<std::u32string,std::string>>{
        {U"ㅎㅏㄴㄱㅡㄹ","한글"},{U"ㄱㅏㅁㅅㅏㅎㅏㅂㄴㅣㄷㅏ","감사합니다"},
        {U"ㄱㅗㅏㄴ","관"},{U"ㄱㅜㅓㄴ","권"},{U"ㄱㅗㅐ","괘"},{U"ㄱㅜㅔ","궤"},
        {U"ㄱㅡㅣ","긔"},{U"ㄷㅏㄹㄱ","닭"},{U"ㄷㅏㄹㄱㅏ","달가"},
        {U"ㄱㅏㄱㅅㅏ","각사"},{U"ㄲㅗㅊ","꽃"},{U"ㅇㅣㅆㅇㅓ","있어"},
        {U"ㅂㅜㅅㅏㄴ","부산"},{U"ㅗㅏ","ㅘ"}}) {
        Input x; x.add(test.first); assert(x.s==test.second);
        for(size_t i=0;i<test.first.size();++i) x.c.backspace(x.s,x.p);
        assert(x.s.empty() && x.p==0);
    }
    Input x; x.s="기존한글";x.p=x.s.size();x.add(U"ㅇㅓ");assert(x.s=="기존한글어");
    x.c.commit();x.c.backspace(x.s,x.p);assert(x.s=="기존한글");
    x.p=hangul::prev(x.s,x.p);x.add(U"ㅇㅏ");assert(x.s=="기존한아글");
    x.c.backspace(x.s,x.p);assert(x.s=="기존한ㅇ글");
    x.c.backspace(x.s,x.p);assert(x.s=="기존한글");
    std::string u="A한😀글Z";size_t p=0;int n=0;while(p<u.size()){auto q=hangul::next(u,p);assert(hangul::prev(u,q)==p);p=q;++n;}assert(n==5);
    assert(hangul::key('q',false)==U'ㅂ' && hangul::key('q',true)==U'ㅃ');
    // Exhaust all modern initial/medial/final combinations and round-trip UTF-8.
    for(auto i:hangul::initial)for(auto m:hangul::medial)for(auto f:hangul::final){auto c=hangul::syllable(i,m,f);auto s=hangul::utf8(c);assert(hangul::decode(s,0)==c);}
    std::cout<<"PASS: composition, compound vowels/finals, migration, deletion, cursor, 11172 syllables\n";
}
