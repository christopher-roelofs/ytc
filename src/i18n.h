// Tiny compile-time-keyed UI localization. The renderer already caches Unicode
// glyphs on demand from the bundled DejaVuSans (Latin + accents, Cyrillic, Greek),
// so these languages display with no extra font. Content titles/metadata are
// localized separately by YouTube via the Innertube hl/gl context.
//
// Internal identity keys (view_label_, tab identity) stay English; translate ONLY
// at the point of display with tr(). Anything without a translation falls back to
// English, so partial coverage is safe.
#pragma once
#include <string>

namespace i18n {

// Translatable UI strings. Add a key here and a row in the table in i18n.cpp.
enum class Str {
    // Top-level menu / view headers
    Home, Search, FavoriteChannels, WatchLater, History, Settings, Exit,
    // Tab strip
    TabAll, TabVideos, TabShorts, TabPlaylists, TabPosts,
    // Settings row labels
    SetMaxQuality, SetVolume, SetVideoDecode, SetHidePaced, SetHideShorts,
    SetAskResume, SetHomeFeed, SetAutoplay, SetSponsorBlock, SetView, SetLanguage,
    // Settings values
    On, Off, Hardware, Software, Favorites, FavoritesPlusHistory,
    ViewGrid, ViewCarousel, View3DCarousel, ViewCoverflow, AutoHighest,
    // Status / empty states / hints
    Results, Loading, Searching, NoResults, NoVideosYet, NothingInTab,
    NoResultsConn, WatchLaterEmpty, NoHistory, NoFavorites,
    PressAToPlay, UpNext, LoadingDescription, LoadingCaptions,
    // Footers (whole-string per context)
    FooterBrowse, FooterSubview, FooterSearch, FooterHomeMin, FooterResume, FooterDesc,
    FooterPlayerPlay, FooterPlayerPause,
    CcTranslate,
    COUNT
};

// Load languages.json + each <code>.json from `dir` (the data dir; files live in
// dir/lang/). Safe to call once at startup. Returns false if the manifest is
// missing (the built-in English fallback still works). Any language/string absent
// from the files falls back to English.
bool load(const std::string& dir);

int         language_count();
const char* language_name(int idx);   // endonym, shown in its own script
const char* language_hl(int idx);     // Innertube hl (e.g. "es")
const char* language_gl(int idx);     // Innertube gl (e.g. "ES")

void set_language(int idx);           // clamps to range
int  language();                      // current index

const char* tr(Str s);                // current language, English fallback

// Canonical string key for a Str (e.g. "Home") — the JSON object key. Stable.
const char* key_name(Str s);
int         str_count();              // == (int)Str::COUNT

} // namespace i18n
