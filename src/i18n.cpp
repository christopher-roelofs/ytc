#include "i18n.h"
#include "../third_party/json.hpp"
#include <array>
#include <fstream>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace i18n {

namespace {

// Canonical JSON keys, in Str enum order. Stable identifiers — these are the object
// keys in the .json files. Edit the VALUES in the files, never these.
const char* kKeys[(int)Str::COUNT] = {
    "Home","Search","FavoriteChannels","WatchLater","History","Settings","Exit",
    "TabAll","TabVideos","TabShorts","TabPlaylists","TabPosts",
    "SetMaxQuality","SetVolume","SetVideoDecode","SetAspect","SetAudioLang","SetCaptionLang",
    "SetAudioMenu","SetVideoMenu","SetCaptionsMenu","SetPlaybackMenu","SetBrowsingMenu",
    "SetCaptionSize","SetCaptionStyle",
    "SetHidePaced",
    "SetAskResume","SetHomeFeed","SetAutoplay","SetSponsorBlock","SetView","SetLanguage",
    "On","Off","Hardware","Software","AspectFit","AspectZoom","AspectStretch",
    "AppLanguage","OriginalTrack",
    "SizeSmall","SizeMedium","SizeLarge",
    "StyleWhite","StyleYellow","StyleWhiteOnBlack","StyleBlackOnWhite",
    "StyleYellowOnBlack","StyleYellowOnBlue",
    "Favorites","FavoritesPlusHistory",
    "ViewGrid","ViewCarousel","View3DCarousel","ViewCoverflow","AutoHighest",
    "Results","Loading","Searching","NoResults","NoVideosYet","NothingInTab",
    "NoResultsConn","WatchLaterEmpty","NoHistory","NoFavorites",
    "PressAToPlay","UpNext","LoadingDescription","LoadingCaptions",
    "FooterBrowse","FooterSubview","FooterSearch","FooterHomeMin","FooterResume","FooterDesc",
    "FooterPlayerPlay","FooterPlayerPause",
    "CcTranslate","CcTranslated",
    "ReadPost","PlayAttachedVideo","AddWatchLater","RemoveWatchLater",
    "ShowDescription","ShowChannelDescription","ShowPlaylistDescription",
    "AddFavorite","RemoveFavorite","GoToChannel","ClearHistoryItem",
    "MenuQuality","MenuSpeed","MenuCaptions","CcNone","MenuStats","Enabled","Disabled",
    "MenuAudioTrack",
    "FooterMenuValue","FooterMenuPlain","FooterPostOverlay","FooterScrollOverlay",
    "SearchHint","WaitingNetwork","Reconnecting","SwitchTabsHint",
    "FavHint2","FavHint3","WlHint2","WlHint3","HistHint2","HistHint3","HomeHint3","SearchHint3",
    "TypeToSearch",
    "CcUnavailable","AddedFav","RemovedFav","AddedWatchLater","RemovedWatchLater","HistoryCleared",
    "NoRecentUploads","PlaylistUnavailable","Cancelled","SkippedPrefix","SeekLimited",
    "FindingNext","NoMoreVideos","PacedResuming","NoMpv","PlayerFailed",
    "ResumeFrom","StartOver",
    "TileVideo","TileShort","TilePlaylist","TilePost","TileHasVideo","TileLive",
    "MenuCastToDevice","CastAddDevice","CastSearching","CastNoDevices","CastEnterCode",
    "CastingTo","CastConnecting","CastFailed","CastPairFailed",
    "FooterRemote","FooterCastPicker","FooterCastCode",
    "CastCodeTitle","CastCodeHint","KbEnter",
    "SetLinkedDevices","CastLinkDevice","CastNoLinked","CastRemoved","FooterManage",
    "CastRemoveConfirm","Yes","No","FooterConfirm",
    "MenuShowComments","CommentsTitle","CommentsLoading","CommentsEmpty","CommentPinned","FooterComments",
    "CommentReplies","CommentHideReplies","FooterCommentsSel",
    "CommentSortTop","CommentSortNewest",
    "MenuDownload","MenuRemoveDownload","Downloads",
    "Downloading","DownloadDone","DownloadFailed","DownloadBusy","DownloadRemoved",
    "FooterSearchResults","SortedByRelevance","SortedByDate","SortedByViews",
    "SortRelevance","SortDate","SortPopular",
    "MenuSearchFilters","FilterType","FilterDuration","FilterUploadDate","FilterPrioritize",
    "FilterChannels","DurShort","DurMedium","DurLong",
    "DateToday","DateWeek","DateMonth","DateYear",
    "FilterRelevance","FilterPopularity","FooterSearchEmpty",
};

// English is baked in — the source strings AND the ultimate fallback, so the UI is
// always usable even if the data/lang files are missing or a key is untranslated.
// (Every other language lives entirely in data/lang/<code>.json.) Order = enum order.
const char* kEnglish[(int)Str::COUNT] = {
    "Home","Search","Favorite Channels","Watch Later","History","Settings","Exit",
    "All","Videos","Shorts","Playlists","Posts",
    "Max Quality","Volume","Video Decode","Aspect Ratio","Audio Language","Caption Language",
    "Audio","Video","Captions","Playback","Browsing",
    "Caption Size","Caption Style",
    "Hide Paced Videos",
    "Ask to Resume","Home Feed","Autoplay","SponsorBlock","View","Language",
    "On","Off","Hardware","Software","Fit","Zoom","Stretch",
    "App Language","Original",
    "Small","Medium","Large",
    "White","Yellow","White on Black","Black on White",
    "Yellow on Black","Yellow on Blue",
    "Favorites","Favorites + History",
    "Grid","Carousel","3D Carousel","Coverflow","Auto (highest)",
    "results","Loading...","Searching...","No results","No videos yet","Nothing in this tab",
    "No results \xE2\x80\x94 check your connection, then press Y to search",
    "Watch Later is empty","No watch history yet","No favorite channels yet",
    "Press A to play","Up next","Loading description...","Loading captions...",
    "D-Pad: move    A: play/open    Select: options    Start: menu    Y: search    B: back",
    "Left/Right: browse    A: open    L/R: switch tabs    V: view    B: back",
    "D-Pad: move    A: type    X: clear    Y: search    B: cancel",
    "Y: search    Start: menu",
    "Up/Down: choose    A: confirm    B: cancel",
    "Left/Right: change    B: back",
    "[A] pause    [Select] options    [<>] seek 10s    [Start] menu    [B] back",
    "[A] resume    [Select] options    [<>] seek 10s    [Start] menu    [B] back",
    "Translate","translated",
    "Read Post","Play Attached Video","Add to Watch Later","Remove from Watch Later",
    "Show Description","Show Channel Description","Show Playlist Description",
    "Add Channel to Favorites","Remove Channel from Favorites","Go to Channel","Clear History",
    "Quality","Speed","Captions","none","Stats for Nerds","Enabled","Disabled",
    "Audio Track",
    "A: select    Left/Right: change    B: close","A: select    B: close",
    "A: play video    Up/Down: move    B: close","Up/Down: scroll    B: close",
    "Press Y to search","Waiting for network...","Reconnecting automatically",
    "L/R shoulders: switch tabs",
    "Open a channel, press Select, and Add to Favorites","Your favorites show up here for quick access",
    "Press Select on a video and Add to Watch Later","Saved videos show up here",
    "Videos you play show up here","History is stored locally on this device",
    "Your Home feed fills in once you add favorite channels","Try a different search, or check your connection",
    "type to search...",
    "Captions unavailable","Added to favorites","Removed from favorites","Added to Watch Later",
    "Removed from Watch Later","History cleared","No recent uploads (or channel unavailable)",
    "Playlist unavailable","cancelled","Skipped","Seeking limited on this video (paced stream)",
    "Finding next video...","No more videos","Paced stream: resuming near the start",
    "playback unavailable (no libmpv in this build)","player failed to start",
    "Resume from","Start from beginning",
    "Video","Short","Playlist","Post","has video","Live",
    "Cast to Device","Add a device","Searching for devices\xE2\x80\xA6",
    "No devices found on this network",
    "Enter the TV code  (YouTube \xE2\x96\xB8 Settings \xE2\x96\xB8 Link with TV code)",
    "Casting to","Connecting\xE2\x80\xA6","Couldn't cast to that device","Pairing failed \xE2\x80\x94 check the code",
    "A: play/pause    <>: seek    ^v: volume    B: stop casting",
    "A: select    B: cancel",
    "A: type    B: cancel",
    "Enter TV Code","enter code","Enter",
    "Linked Devices","Link a device","No linked devices yet","Removed",
    "A: remove    B: back",
    "Remove Device?","Yes","No","Left/Right: choose    A: confirm    B: cancel",
    "Show Comments","Comments","Loading comments\xE2\x80\xA6","No comments","Pinned",
    "Up/Down: scroll    B: close",
    "replies","Hide replies","Up/Down: select    A: replies    X: sort    B: close",
    "Top","Newest",
    "Download","Remove Download","Downloads",
    "Downloading","Downloaded","Download failed","A download is in progress","Download removed",
    "D-Pad: move    A: play/open    X: sort    Select: options    Start: menu    Y: search    B: back",
    "Sorted by relevance","Sorted by date","Sorted by views",
    "Relevance","Newest","Popular",
    "Search Filters","Type","Duration","Upload date","Prioritize",
    "Channels","Under 3 min","3 - 20 min","Over 20 min",
    "Today","This week","This month","This year",
    "Relevance","Popularity",
    "Select: filters    Y: search    Start: menu    B: back",
};

struct Lang {
    std::string code, name, hl, gl;
    std::unordered_map<std::string, std::string> str;   // key -> localized text
};

// The English fallback entry always exists at index 0, even before load().
std::vector<Lang> g_langs = { {"en", "English", "en", "US", {}} };
int g_lang = 0;

const Lang& cur()  { return g_langs[(g_lang >= 0 && g_lang < (int)g_langs.size()) ? g_lang : 0]; }

} // namespace

bool load(const std::string& dir) {
    std::string base = dir + "/lang/";
    std::ifstream mf(base + "languages.json");
    if (!mf) return false;                     // keep the built-in English entry
    json manifest = json::parse(mf, nullptr, false);
    if (manifest.is_discarded() || !manifest.contains("languages")) return false;

    std::vector<Lang> loaded;
    for (const auto& e : manifest["languages"]) {
        Lang L;
        L.code = e.value("code", "");
        L.name = e.value("name", L.code);
        L.hl   = e.value("hl", "en");
        L.gl   = e.value("gl", "US");
        if (L.code.empty()) continue;
        std::ifstream lf(base + L.code + ".json");
        if (lf) {
            json j = json::parse(lf, nullptr, false);
            if (!j.is_discarded() && j.is_object())
                for (auto it = j.begin(); it != j.end(); ++it)
                    if (it.value().is_string()) L.str[it.key()] = it.value().get<std::string>();
        }
        loaded.push_back(std::move(L));
    }
    if (loaded.empty()) return false;
    // Guarantee an English entry exists at index 0 (for the fallback chain).
    bool has_en = false;
    for (auto& L : loaded) if (L.hl == "en") { has_en = true; break; }
    if (!has_en) loaded.insert(loaded.begin(), {"en", "English", "en", "US", {}});
    g_langs = std::move(loaded);
    if (g_lang >= (int)g_langs.size()) g_lang = 0;
    return true;
}

int         language_count()     { return (int)g_langs.size(); }
const char* language_name(int i) { return (i >= 0 && i < (int)g_langs.size()) ? g_langs[i].name.c_str() : "English"; }
const char* language_hl(int i)   { return (i >= 0 && i < (int)g_langs.size()) ? g_langs[i].hl.c_str() : "en"; }
const char* language_gl(int i)   { return (i >= 0 && i < (int)g_langs.size()) ? g_langs[i].gl.c_str() : "US"; }
void        set_language(int i)  { g_lang = (i >= 0 && i < (int)g_langs.size()) ? i : 0; }
int         language()           { return g_lang; }

const char* key_name(Str s) {
    int k = (int)s;
    return (k >= 0 && k < (int)Str::COUNT) ? kKeys[k] : "";
}
int str_count() { return (int)Str::COUNT; }

const char* tr(Str s) {
    int k = (int)s;
    if (k < 0 || k >= (int)Str::COUNT) return "";
    const std::string& key = kKeys[k];
    // current language file -> English file (index 0) -> baked English default.
    auto it = cur().str.find(key);
    if (it != cur().str.end() && !it->second.empty()) return it->second.c_str();
    if (!g_langs.empty()) {
        auto e = g_langs[0].str.find(key);
        if (e != g_langs[0].str.end() && !e->second.empty()) return e->second.c_str();
    }
    return kEnglish[k];
}

} // namespace i18n
