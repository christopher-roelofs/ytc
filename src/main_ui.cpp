// UI entry point.
//   ./yt_ui "<query>"                  interactive (KMSDRM/X11), gamepad+keyboard
//   YTC_SHOT=out ./yt_ui "<q>"    headless: render frames to out_NN.png
//
// Headless mode uses the SDL "offscreen" driver so the exact GLES2 UI path is
// exercised with no display, letting us verify the UI visually via screenshots.
#include "ui.h"
#include "i18n.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>   // readlink (exe_dir)

static const char* config_path() {
    const char* e = std::getenv("YTC_CONFIG");
    return e ? e : "config/clients.json";
}

// Directory containing the running executable (Linux), for locating bundled
// data files independent of the current working directory.
static std::string exe_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    std::string p(buf);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

static ui::App::Action map_key(SDL_Keycode k) {
    using A = ui::App::Action;
    switch (k) {
        case SDLK_LEFT:  return A::Left;
        case SDLK_RIGHT: return A::Right;
        case SDLK_UP:    return A::Up;
        case SDLK_DOWN:  return A::Down;
        case SDLK_RETURN: case SDLK_SPACE: return A::Select;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: return A::Back;
        default: return A::None;
    }
}
static ui::App::Action map_button(Uint8 b) {
    using A = ui::App::Action;
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return A::Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return A::Right;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    return A::Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return A::Down;
        case SDL_CONTROLLER_BUTTON_A:          return A::Select;
        case SDL_CONTROLLER_BUTTON_B:          return A::Back;
        default: return A::None;
    }
}

int main(int argc, char** argv) {
    std::string query = argc > 1 ? argv[1] : "";   // no query => Latest/empty state
    const char* shot = std::getenv("YTC_SHOT");
    bool headless = shot != nullptr;

    // Driver: respect an explicit SDL_VIDEODRIVER from the environment (e.g.
    // kmsdrm on a device whose SDL lacks the offscreen backend); otherwise
    // default screenshots to offscreen and interactive to auto.
    std::string driver;
    if (std::getenv("SDL_VIDEODRIVER")) driver = "";      // honor the env value
    else if (headless)                  driver = "offscreen";
    // Initial window size (YTC_WINSIZE=WxH), e.g. 640x480 for a handheld.
    int win_w = 1280, win_h = 720;
    if (const char* ws = std::getenv("YTC_WINSIZE")) {
        int a, b; if (sscanf(ws, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) { win_w = a; win_h = b; }
    }
    auto win = gfx::Window::create(win_w, win_h, "YTC", driver);
    if (!win) { std::fprintf(stderr, "window create failed\n"); return 1; }

    // Load UI translations before the App reads the saved language. Same data-dir
    // search as the controller DB: cwd, <exe>/data, <exe>/../data, system dirs.
    // YTC_LANGDIR overrides. English is baked in, so failure here is non-fatal.
    {
        std::string d = exe_dir();
        const char* env = std::getenv("YTC_LANGDIR");
        std::string cands[] = { env ? std::string(env) : std::string(),
                                "data", d + "/data", d + "/../data",
                                "/opt/ytc", "/usr/share/ytc" };
        for (const auto& c : cands) {
            if (c.empty()) continue;
            if (i18n::load(c)) { std::fprintf(stderr, "i18n: loaded %d languages from %s/lang\n",
                                              i18n::language_count(), c.c_str()); break; }
        }
    }

    gfx::Renderer rn;
    std::unique_ptr<ui::App> app_ptr;
    try {
        app_ptr = std::make_unique<ui::App>(config_path(), win.get());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: cannot start (%s)\n", e.what());
        return 1;   // exit cleanly instead of std::terminate
    }
    ui::App& app = *app_ptr;
    if (!query.empty()) {
        std::fprintf(stderr, "searching '%s'...\n", query.c_str());
        app.search(query);        // guarded internally; never throws (CLI/test convenience)
    } else {
        app.load_home();          // always start on Home (favorite-channel RSS; may be empty)
    }

    if (headless) {
        // Render a short navigation sequence, waiting for thumbnails between shots.
        struct Step { const char* label; ui::App::Action act; int settle_ms; };
        Step steps[] = {
            {"00_loading", ui::App::Action::None,  0},
            {"01_loaded",  ui::App::Action::None,  2500},
            {"02_right",   ui::App::Action::Right, 200},
            {"03_down",    ui::App::Action::Down,  200},
            {"04_down",    ui::App::Action::Down,  400},
        };
        // Optional playback capture: select the first result, let mpv buffer +
        // decode, then screenshot the player (video frame + overlay).
        Step play_steps[] = {
            {"05_home",    ui::App::Action::Up,     100},
            {"06_home",    ui::App::Action::Up,     100},
            {"07_loading", ui::App::Action::Select, 0},     // spinner (resolve in flight)
            {"08_playing", ui::App::Action::None,   6000},  // resolve done, video plays
        };
        // Debug: play a specific video id straight through the real App/player
        // path (with mpv logging), to reproduce per-video failures.
        if (const char* pid = std::getenv("YTC_PLAYID")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            app.input(ui::App::Action::Select);
            int w = 0;
            do { app.pump_async(); app.render(rn); win->swap(); SDL_Delay(50); w += 50;
                 if (w == 6000) win->screenshot(std::string(shot) + "_playing.png"); }
            while (w < 12000);
            std::fprintf(stderr, "PLAYID %s final mode=%d\n", pid, (int)app.mode());
            return 0;
        }
        if (const char* pid = std::getenv("YTC_PAUSETEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            app.input(ui::App::Action::Select);           // start playback
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(6000);
            std::fprintf(stderr, "PAUSETEST: playing, paused=%d\n", app.player_paused());
            app.render(rn); win->screenshot(std::string(shot) + "_playing_faded.png");
            app.input(ui::App::Action::Select);            // pause (Select toggles)
            settle(400);
            std::fprintf(stderr, "PAUSETEST: after Select, paused=%d\n", app.player_paused());
            app.render(rn); win->screenshot(std::string(shot) + "_paused.png");
            app.input(ui::App::Action::Select);            // resume before menu test
            settle(300);
            app.open_main_menu();                          // Start over the player
            settle(400);
            std::fprintf(stderr, "PAUSETEST: menu open, paused=%d\n", app.player_paused());
            app.render(rn); win->screenshot(std::string(shot) + "_menu_over_player.png");
            app.input(ui::App::Action::Back);              // close menu (no navigate)
            settle(400);
            std::fprintf(stderr, "PAUSETEST: menu closed, paused=%d\n", app.player_paused());
            return 0;
        }
        if (const char* pid = std::getenv("YTC_RESUMETEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            app.input(ui::App::Action::Select);          // first play (no prompt)
            settle(6000);
            app.input(ui::App::Action::Right); settle(500);   // seek forward (~+10s)
            app.input(ui::App::Action::Right); settle(1500);
            std::fprintf(stderr, "RESUMETEST: mode=%d before Back\n", (int)app.mode());
            app.input(ui::App::Action::Back);            // saves resume position
            settle(400);
            app.input(ui::App::Action::Select);          // play again -> should prompt
            settle(300);
            app.render(rn); win->screenshot(std::string(shot) + "_prompt.png");
            std::fprintf(stderr, "RESUMETEST: after 2nd select mode=%d\n", (int)app.mode());
            app.input(ui::App::Action::Select);          // confirm Resume
            settle(6000);
            std::fprintf(stderr, "RESUMETEST: resumed mode=%d\n", (int)app.mode());
            return 0;
        }
        if (const char* pid = std::getenv("YTC_QUALTEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            app.input(ui::App::Action::Select);            // start playback
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(8000);
            std::fprintf(stderr, "QUALTEST: playing mode=%d\n", (int)app.mode());
            app.input(ui::App::Action::Menu);              // open options menu
            settle(300);
            app.render(rn); win->screenshot(std::string(shot) + "_optmenu.png");
            // Move down to the Quality row, raise it, then close (re-resolve + resume).
            for (int i = 0; i < 3; ++i) app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Right);             // raise quality -> dirty
            settle(200);
            app.render(rn); win->screenshot(std::string(shot) + "_optmenu_q.png");
            app.input(ui::App::Action::Back);              // close -> replay_current(pos)
            settle(9000);                                  // allow re-resolve + reload
            std::fprintf(stderr, "QUALTEST: after change mode=%d\n", (int)app.mode());
            app.render(rn); win->screenshot(std::string(shot) + "_after.png");
            return 0;
        }
        if (const char* pid = std::getenv("YTC_SEEKTEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            app.input(ui::App::Action::Select);
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(5000);
            std::fprintf(stderr, "SEEKTEST: before, mode=%d\n", (int)app.mode());
            // Aggressive: many rapid seeks with tiny gaps, mixing directions.
            for (int i = 0; i < 16 && app.mode() == ui::App::Mode::Playing; ++i) {
                app.input(i % 4 == 3 ? ui::App::Action::Left : ui::App::Action::Right);
                settle(40);
            }
            std::fprintf(stderr, "SEEKTEST: after rapid seeks, mode=%d\n", (int)app.mode());
            settle(4000);   // let the debounced seek fire + stream settle
            // Several more waves pressing against the cache edge (mimics real use).
            for (int wave = 0; wave < 4 && app.mode() == ui::App::Mode::Playing; ++wave) {
                for (int i = 0; i < 8; ++i) { app.input(ui::App::Action::Right); settle(60); }
                settle(2500);
                std::fprintf(stderr, "SEEKTEST: wave %d done, mode=%d\n", wave, (int)app.mode());
            }
            settle(3000);
            std::fprintf(stderr, "SEEKTEST: final mode=%d (2=Playing,0=Grid)\n",
                         (int)app.mode());
            return 0;
        }
        if (const char* pid = std::getenv("YTC_VOLTEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(2500);                              // let visitor_data warm up first
            app.input(ui::App::Action::Select);
            settle(8000);
            for (int i = 0; i < 3; ++i) app.input(ui::App::Action::Up);    // +15%
            app.render(rn); win->screenshot(std::string(shot) + "_volup.png");
            for (int i = 0; i < 8; ++i) app.input(ui::App::Action::Down);  // -40%
            app.render(rn); win->screenshot(std::string(shot) + "_voldn.png");
            std::fprintf(stderr, "VOLTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_POSTVIDTEST")) {
            yt::SearchResult p;
            p.kind = yt::SearchResult::Kind::Post;
            p.video_id = "dQw4w9WgXcQ";
            p.title = "Big announcement — check out our new video! #update";
            p.post_text = "Big announcement — check out our new video! #update\n\n"
                          "Thanks everyone for the support. We put together a longer "
                          "explanation below that should scroll past a couple of lines "
                          "so we can see the post text focus state working correctly.";
            p.thumbnail_url = "https://i.ytimg.com/vi/dQw4w9WgXcQ/mqdefault.jpg";
            p.published_text = "2 days ago";
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1500);
            app.set_results({p});
            app.input(ui::App::Action::Select);      // open the post view
            settle(1500);
            app.render(rn); win->screenshot(std::string(shot) + "_video.png");
            app.input(ui::App::Action::Down);        // focus the post text
            settle(200);
            app.render(rn); win->screenshot(std::string(shot) + "_text.png");
            std::fprintf(stderr, "POSTVIDTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_AUTOPLAYSEQ")) {
            std::vector<yt::SearchResult> rs(2);
            rs[0].video_id = "aaaaaaaaaaa"; rs[0].title = "First Video (just ended)";
            rs[1].video_id = "bbbbbbbbbbb"; rs[1].title = "Second Video Title Here";
            rs[1].author = "Some Channel";
            app.set_results(rs);
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(300);
            app.test_end_playback(0);                 // simulate EOF of item 0
            settle(200); app.render(rn); win->screenshot(std::string(shot) + "_armed.png");
            settle(1200); app.render(rn); win->screenshot(std::string(shot) + "_upnext.png");
            std::fprintf(stderr, "AUTOPLAYSEQ done\n");
            return 0;
        }
        if (const char* pid = std::getenv("YTC_CCPLAYTEST")) {
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(3000);
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;  // channel_id empty
            app.set_results({sr});
            app.input(ui::App::Action::Select); settle(8000);      // play + async cc fetch
            app.input(ui::App::Action::Menu);                       // options menu
            for (int i = 0; i < 4; ++i) app.input(ui::App::Action::Down); // -> Captions row
            app.input(ui::App::Action::Right);                      // select first caption
            settle(2500);
            std::fprintf(stderr, "CCPLAYTEST done\n");
            return 0;
        }
        if (const char* pid = std::getenv("YTC_SBPLAYTEST")) {
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(3000);                          // let startup home-load finish first
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});                 // NOW set the target (won't be overwritten)
            app.input(ui::App::Action::Select); settle(9000);
            app.render(rn); win->screenshot(std::string(shot) + "_sb.png");
            std::fprintf(stderr, "SBPLAYTEST done\n");
            return 0;
        }
        if (const char* pid = std::getenv("YTC_SPEEDTEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(2500); app.input(ui::App::Action::Select); settle(8000);
            app.input(ui::App::Action::Menu);                          // options menu
            app.render(rn); win->screenshot(std::string(shot) + "_menu.png");
            // Find + cycle the Speed row: walk down and Right a couple times.
            for (int i = 0; i < 5; ++i) app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Right); app.input(ui::App::Action::Right);
            app.render(rn); win->screenshot(std::string(shot) + "_speed.png");
            std::fprintf(stderr, "SPEEDTEST done\n");
            return 0;
        }
        if (const char* pid = std::getenv("YTC_STATSTEST")) {
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            app.input(ui::App::Action::Select);
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(8000);
            app.input(ui::App::Action::Menu);              // options menu
            for (int i = 0; i < 4; ++i) app.input(ui::App::Action::Down);  // -> Stats row
            app.input(ui::App::Action::Right);             // enable stats
            app.render(rn); win->screenshot(std::string(shot) + "_menu.png");
            app.input(ui::App::Action::Back);              // close menu
            settle(600);
            app.render(rn); win->screenshot(std::string(shot) + "_stats.png");
            std::fprintf(stderr, "STATSTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_VIDPAGE")) {  // channel Videos tab: scroll-driven load-more
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1500);
            app.input(ui::App::Action::Select);      // open channel (first result)
            settle(2500);
            app.input(ui::App::Action::Up);          // tabs
            app.input(ui::App::Action::Right);       // Videos
            settle(2500);
            app.input(ui::App::Action::Down);        // into grid
            for (int i = 0; i < 30; ++i) { app.input(ui::App::Action::Down); settle(250); }
            std::fprintf(stderr, "VIDPAGE done sel=%d\n", app.selected_index());
            return 0;
        }
        if (std::getenv("YTC_HOMEPAGE")) {  // Home feed: scroll to bottom -> per-channel load-more
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(3000);                                // let the home feed load
            std::fprintf(stderr, "HOMEPAGE initial count=%d\n", app.results_count());
            for (int i = 0; i < 80; ++i) { app.input(ui::App::Action::Down); settle(120); }
            settle(3000);                                // let a load-more round finish
            std::fprintf(stderr, "HOMEPAGE after-scroll count=%d\n", app.results_count());
            return 0;
        }
        if (std::getenv("YTC_CHANPAGE")) {  // channel: every tab should page on scroll
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1000);
            app.open_main_menu();
            app.input(ui::App::Action::Down);            // -> Favorite Channels
            app.input(ui::App::Action::Select);
            settle(1000);
            app.input(ui::App::Action::Select);          // open first favorite channel
            settle(3000);                                // channel All tab
            const char* names[] = {"Videos", "Shorts", "Playlists", "Posts"};
            for (int t = 0; t < 4; ++t) {
                app.cycle_tab(+1);                       // -> next tab
                settle(3000);
                int before = app.results_count();
                for (int i = 0; i < 40; ++i) { app.input(ui::App::Action::Down); settle(120); }
                settle(2500);
                int after = app.results_count();
                std::fprintf(stderr, "CHANPAGE %-9s %d -> %d %s\n", names[t], before, after,
                             after > before ? "PAGED" : "(no growth)");
            }
            return 0;
        }
        if (std::getenv("YTC_HOMEPOSTS")) {  // Home Posts sub-tab: load + scroll -> load-more
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(3000);
            for (int i = 0; i < 4; ++i) app.cycle_tab(+1);   // All -> Posts (tab 4)
            settle(4000);
            app.render(rn); win->screenshot(std::string(shot) + "_posts.png");
            std::fprintf(stderr, "HOMEPOSTS initial count=%d\n", app.results_count());
            for (int i = 0; i < 60; ++i) { app.input(ui::App::Action::Down); settle(120); }
            settle(3000);
            std::fprintf(stderr, "HOMEPOSTS after-scroll count=%d\n", app.results_count());
            return 0;
        }
        if (std::getenv("YTC_HOMEPL")) {  // Home Playlists sub-tab: scroll -> load-more
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(3000);
            app.cycle_tab(+1); app.cycle_tab(+1); app.cycle_tab(+1);  // -> Playlists
            settle(4000);
            std::fprintf(stderr, "HOMEPL initial count=%d\n", app.results_count());
            for (int i = 0; i < 60; ++i) { app.input(ui::App::Action::Down); settle(120); }
            settle(3000);
            std::fprintf(stderr, "HOMEPL after-scroll count=%d\n", app.results_count());
            return 0;
        }
        if (std::getenv("YTC_HOMESHORTS")) {  // Home Shorts sub-tab: scroll -> load-more
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(3000);                                // let the home feed load
            app.cycle_tab(+1);                           // All -> Videos
            app.cycle_tab(+1);                           // Videos -> Shorts
            settle(400);
            std::fprintf(stderr, "HOMESHORTS initial count=%d\n", app.results_count());
            for (int i = 0; i < 60; ++i) { app.input(ui::App::Action::Down); settle(120); }
            settle(3000);
            std::fprintf(stderr, "HOMESHORTS after-scroll count=%d\n", app.results_count());
            return 0;
        }
        if (std::getenv("YTC_FAVTABTEST")) {  // regression: favorites -> channel -> tab Right
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1000);
            app.open_main_menu();
            app.input(ui::App::Action::Down);        // -> Favorite Channels
            app.input(ui::App::Action::Select);
            settle(800);
            app.input(ui::App::Action::Select);      // open first favorite channel
            settle(2500);
            app.input(ui::App::Action::Up);          // focus tabs
            app.input(ui::App::Action::Right);       // -> Videos (async)
            settle(2500);
            app.render(rn); win->screenshot(std::string(shot) + "_after_right.png");
            std::fprintf(stderr, "FAVTABTEST done, in_subview=%d\n", (int)app.in_subview());
            return 0;
        }
        if (std::getenv("YTC_HOMETABTEST")) {  // Home tab strip walk-through
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1500);
            app.render(rn); win->screenshot(std::string(shot) + "_all.png");
            app.input(ui::App::Action::Up);          // focus tabs
            const char* names[] = {"videos", "shorts", "playlists"};
            for (int i = 0; i < 3; ++i) {
                app.input(ui::App::Action::Right);
                settle(i == 2 ? 6000 : 300);         // playlists tab fetches async
                app.render(rn);
                win->screenshot(std::string(shot) + "_" + names[i] + ".png");
            }
            std::fprintf(stderr, "HOMETABTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_TABTEST")) {  // channel tab strip walk-through
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1500);
            app.input(ui::App::Action::Select);      // open the first result (a channel)
            settle(2500);
            app.render(rn); win->screenshot(std::string(shot) + "_all.png");
            app.input(ui::App::Action::Up);          // focus the tab strip
            app.render(rn); win->screenshot(std::string(shot) + "_tabfocus.png");
            const char* names[] = {"videos", "shorts", "playlists", "posts"};
            for (int i = 0; i < 4; ++i) {
                app.input(ui::App::Action::Right);   // next tab (fetches)
                settle(1800);
                app.render(rn);
                win->screenshot(std::string(shot) + "_" + names[i] + ".png");
            }
            app.input(ui::App::Action::Down);        // back into the grid
            app.render(rn); win->screenshot(std::string(shot) + "_gridfocus.png");
            // Deep stack: open a playlist FROM the Playlists tab, then unwind.
            app.input(ui::App::Action::Select);      // open first playlist
            settle(2000);
            app.render(rn); win->screenshot(std::string(shot) + "_pl_open.png");
            app.input(ui::App::Action::Back);        // -> channel (Playlists tab)
            settle(300);
            app.render(rn); win->screenshot(std::string(shot) + "_back1.png");
            app.input(ui::App::Action::Back);        // -> original search results
            settle(300);
            app.render(rn); win->screenshot(std::string(shot) + "_back2.png");
            std::fprintf(stderr, "TABTEST done\n");
            return 0;
        }
        if (const char* pid = std::getenv("YTC_DESCTEST")) {  // description overlay
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1800);
            // Grid tile path: menu -> Show Description (async fetch).
            app.input(ui::App::Action::Menu);
            app.input(ui::App::Action::Down); app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Select);
            settle(2500);
            app.render(rn); win->screenshot(std::string(shot) + "_grid_desc.png");
            app.input(ui::App::Action::Back);        // close overlay
            // Playing path: play, menu -> Show Description (instant).
            yt::SearchResult sr; sr.video_id = pid; sr.title = pid;
            app.set_results({sr});
            app.input(ui::App::Action::Select);
            settle(7000);
            app.input(ui::App::Action::Menu);
            app.input(ui::App::Action::Down);        // -> Show Description
            app.input(ui::App::Action::Select);
            settle(300);
            for (int i = 0; i < 4; ++i) app.input(ui::App::Action::Down);  // scroll
            app.render(rn); win->screenshot(std::string(shot) + "_play_desc.png");
            std::fprintf(stderr, "DESCTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_CHDESCTEST")) {  // channel description from a playlist row
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(2000);
            int guard = 0;                        // find + open a playlist
            while (guard++ < 60 && app.selected() && !app.selected()->is_playlist()) {
                int before = app.selected_index();
                app.input(ui::App::Action::Right);
                if (app.selected_index() == before) app.input(ui::App::Action::Down);
                if (app.selected_index() == before) break;
            }
            app.input(ui::App::Action::Select);   // open playlist
            settle(2500);
            app.input(ui::App::Action::Menu);     // options on the first video row
            app.render(rn); win->screenshot(std::string(shot) + "_rowmenu.png");
            // items: [Fav toggle (has channel_id)], [WatchLater], [ShowDesc], [ShowChannelDesc], [GoToChannel]
            for (int i = 0; i < 3; ++i) app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Select);   // Show Channel Description
            settle(2500);
            app.render(rn); win->screenshot(std::string(shot) + "_chdesc.png");
            std::fprintf(stderr, "CHDESCTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_PLDESCTEST")) {  // playlist description overlay
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(2000);
            int guard = 0;
            while (guard++ < 60 && app.selected() && !app.selected()->is_playlist()) {
                int before = app.selected_index();
                app.input(ui::App::Action::Right);
                if (app.selected_index() == before) app.input(ui::App::Action::Down);
                if (app.selected_index() == before) break;
            }
            app.input(ui::App::Action::Menu);
            app.input(ui::App::Action::Down);       // -> Show Description
            app.input(ui::App::Action::Select);
            settle(2500);
            app.render(rn); win->screenshot(std::string(shot) + "_pldesc.png");
            std::fprintf(stderr, "PLDESCTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_WLPLTEST")) {  // playlist -> Watch Later round trip
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(2000);
            int guard = 0;                        // walk to the first playlist tile
            while (guard++ < 60 && app.selected() && !app.selected()->is_playlist()) {
                int before = app.selected_index();
                app.input(ui::App::Action::Right);
                if (app.selected_index() == before) app.input(ui::App::Action::Down);
                if (app.selected_index() == before) break;
            }
            if (!app.selected() || !app.selected()->is_playlist()) {
                std::fprintf(stderr, "WLPLTEST: no playlist found\n"); return 0;
            }
            app.input(ui::App::Action::Menu);     // options menu
            app.render(rn); win->screenshot(std::string(shot) + "_menu.png");
            app.input(ui::App::Action::Select);   // Add to Watch Later
            settle(300);
            app.open_main_menu();                 // -> Watch Later view
            app.input(ui::App::Action::Down); app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Select);
            settle(800);
            app.render(rn); win->screenshot(std::string(shot) + "_wl.png");
            app.input(ui::App::Action::Select);   // open the saved playlist
            settle(2500);
            app.render(rn); win->screenshot(std::string(shot) + "_opened.png");
            std::fprintf(stderr, "WLPLTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_PLAYLISTTEST")) {  // open the first playlist tile
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            settle(2000);
            app.render(rn); win->screenshot(std::string(shot) + "_results.png");
            int guard = 0;
            while (guard++ < 60 && app.selected() && !app.selected()->is_playlist()) {
                int before = app.selected_index();
                app.input(ui::App::Action::Right);            // walk the grid
                if (app.selected_index() == before) app.input(ui::App::Action::Down);
                if (app.selected_index() == before) break;    // stuck at the end
            }
            if (app.selected() && app.selected()->is_playlist()) {
                std::fprintf(stderr, "PLAYLISTTEST: opening '%s'\n",
                             app.selected()->title.c_str());
                app.input(ui::App::Action::Select);
                settle(2500);
                app.render(rn); win->screenshot(std::string(shot) + "_open.png");
                app.input(ui::App::Action::Back);    // pop back
                settle(400);
                app.render(rn); win->screenshot(std::string(shot) + "_back.png");
            } else std::fprintf(stderr, "PLAYLISTTEST: no playlist tile found\n");
            std::fprintf(stderr, "PLAYLISTTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_TOGGLETEST")) {  // hide-shorts toggle round trip
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            auto toggle_shorts = [&]{
                app.open_main_menu();
                for (int i = 0; i < 4; ++i) app.input(ui::App::Action::Down);  // -> Settings
                app.input(ui::App::Action::Select);
                app.input(ui::App::Action::Down); app.input(ui::App::Action::Down); // -> Hide Shorts
                app.input(ui::App::Action::Right);                             // flip
                app.input(ui::App::Action::Back);                              // -> main menu
                app.input(ui::App::Action::Back);                              // close
            };
            settle(2000);
            app.render(rn); win->screenshot(std::string(shot) + "_1_before.png");
            toggle_shorts();   // ON: shorts filtered in place
            settle(600);
            app.render(rn); win->screenshot(std::string(shot) + "_2_hidden.png");
            toggle_shorts();   // OFF: view refetches, shorts restored
            settle(1500);
            app.render(rn); win->screenshot(std::string(shot) + "_3_restored.png");
            std::fprintf(stderr, "TOGGLETEST done\n");
            return 0;
        }
        if (const char* wms = std::getenv("YTC_WAITSHOT")) {  // settle N ms, screenshot
            int ms = atoi(wms); int w = 0;
            do { app.pump_async(); app.render(rn); win->swap(); SDL_Delay(50); w += 50; }
            while (w < ms);
            if (const char* pr = std::getenv("YTC_PRERIGHT"))
                for (int k = 0; k < atoi(pr); ++k) { app.input(ui::App::Action::Right);
                    for (int j=0;j<6;j++){app.pump_async();app.render(rn);win->swap();SDL_Delay(30);} }
            app.render(rn); win->screenshot(std::string(shot) + "_wait.png");
            std::fprintf(stderr, "WAITSHOT done\n");
            return 0;
        }
        if (std::getenv("YTC_PAGETEST")) {   // scroll down; results should grow via load-more
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn); win->swap();
                SDL_Delay(50); w+=50; } while (w<ms); };
            settle(1500);
            if (std::getenv("YTC_PAGE_CHANNEL") && app.selected() && app.selected()->is_channel()) {
                app.input(ui::App::Action::Select);   // open the channel view first
                settle(2000);
            }
            for (int i = 0; i < 5; ++i) { app.input(ui::App::Action::Down); settle(160); }
            app.render(rn); win->screenshot(std::string(shot) + "_bottomsel.png");
            for (int i = 0; i < 34; ++i) { app.input(ui::App::Action::Down); settle(250); }
            std::fprintf(stderr, "PAGETEST done sel=%d\n", app.selected_index());
            return 0;
        }
        if (std::getenv("YTC_CHANOPEN")) {   // open channel at sel 0, wait for async info
            app.input(ui::App::Action::Select);
            int w = 0;
            do { app.pump_async(); app.render(rn); win->swap(); SDL_Delay(50); w += 50; }
            while (w < 5000);
            app.render(rn);
            win->screenshot(std::string(shot) + "_chan.png");
            std::fprintf(stderr, "CHANOPEN done\n");
            return 0;
        }
        auto run_step = [&](const Step& st) {
            if (st.act != ui::App::Action::None) app.input(st.act);
            int waited = 0;
            do { app.pump_async(); app.render(rn); win->swap();
                 SDL_Delay(50); waited += 50; }
            while (waited < st.settle_ms);
            app.render(rn);
            std::string path = std::string(shot) + "_" + st.label + ".png";
            win->screenshot(path);
            std::fprintf(stderr, "wrote %s (mode=%d)\n", path.c_str(), (int)app.mode());
        };
        for (auto& st : steps) run_step(st);
        if (std::getenv("YTC_PLAYTEST"))
            for (auto& st : play_steps) run_step(st);
        if (std::getenv("YTC_SEQTEST")) {
            // Reproduce the "2nd+ video fails" bug: play several grid items in a
            // row (select -> play -> back -> next). Each must actually PLAY.
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            int n = std::getenv("YTC_SEQN") ? atoi(std::getenv("YTC_SEQN")) : 4;
            for (int i = 0; i < n; ++i) {
                app.input(ui::App::Action::Select);   // play current selection
                settle(4000);
                bool playing = (app.mode() == ui::App::Mode::Playing);
                std::fprintf(stderr, "SEQ item %d: %s\n", i,
                             playing ? "PLAYING (ok)" : "NOT playing (FAIL)");
                if (playing && shot)
                    win->screenshot(std::string(shot) + "_seq" + std::to_string(i) + ".png");
                app.input(ui::App::Action::Back);     // back to grid
                settle(400);
                app.input(ui::App::Action::Right);    // next item
                settle(200);
            }
            return 0;
        }
        if (std::getenv("YTC_CAROUSELSHOT")) {
            app.toggle_view();
            for (int i = 0; i < 3; ++i) app.input(ui::App::Action::Right);
            int w = 0;
            do { app.pump_async(); app.render(rn); win->swap(); SDL_Delay(40); w += 40; }
            while (w < 3500);   // let thumbnails load + the ease animation settle
            app.render(rn);
            win->screenshot(std::string(shot) + "_carousel.png");
            std::fprintf(stderr, "wrote carousel screenshot\n");
            return 0;
        }
        if (std::getenv("YTC_MENUSHOT")) {
            app.input(ui::App::Action::Menu);          // open options menu on sel 0
            app.render(rn);
            win->screenshot(std::string(shot) + "_menu.png");
            app.input(ui::App::Action::Select);        // activate first item (favorite)
            app.render(rn);
            win->screenshot(std::string(shot) + "_menu_done.png");
            std::fprintf(stderr, "MENUSHOT done\n");
            return 0;
        }
        if (std::getenv("YTC_CLEARHISTTEST")) {
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            app.open_main_menu();
            for (int i = 0; i < 3; ++i) app.input(ui::App::Action::Down);   // -> History
            app.input(ui::App::Action::Select);                            // open History view
            settle(1200);
            app.input(ui::App::Action::Menu);                              // options on first tile
            app.render(rn); win->screenshot(std::string(shot) + "_histmenu.png");
            // Walk to the "Clear History" row (last item) and activate it.
            for (int i = 0; i < 8; ++i) app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Select);
            settle(600);
            app.render(rn); win->screenshot(std::string(shot) + "_histcleared.png");
            std::fprintf(stderr, "CLEARHISTTEST done\n");
            return 0;
        }
        if (std::getenv("YTC_SETTINGSSHOT")) {
            app.open_main_menu();
            for (int i = 0; i < 4; ++i) app.input(ui::App::Action::Down);  // -> Settings
            app.input(ui::App::Action::Select);                            // open Settings
            app.render(rn); win->screenshot(std::string(shot) + "_settings.png");
            app.input(ui::App::Action::Down);                              // Volume
            app.input(ui::App::Action::Down);                              // Video Decode
            app.input(ui::App::Action::Right);                             // toggle -> Software
            app.render(rn); win->screenshot(std::string(shot) + "_settings_sw.png");
            std::fprintf(stderr, "SETTINGSSHOT done\n");
            return 0;
        }
        if (std::getenv("YTC_MAINMENUSHOT")) {
            app.open_main_menu();                        // Start-button top-level menu
            app.render(rn);
            win->screenshot(std::string(shot) + "_mainmenu.png");
            // Walk down to "Watch Later" (index 2) and open it.
            app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Select);
            for (int i = 0; i < 8; ++i) { app.pump_async(); app.render(rn); SDL_Delay(60); }
            win->screenshot(std::string(shot) + "_watchlater.png");
            // Back to grid, open menu, go to History (index 3).
            app.open_main_menu();
            app.input(ui::App::Action::Down); app.input(ui::App::Action::Down);
            app.input(ui::App::Action::Down); app.input(ui::App::Action::Select);
            for (int i = 0; i < 8; ++i) { app.pump_async(); app.render(rn); SDL_Delay(60); }
            win->screenshot(std::string(shot) + "_history.png");
            // Favorites view (wait longer: channel_info + avatar download per channel).
            app.open_main_menu();
            app.input(ui::App::Action::Down); app.input(ui::App::Action::Select);
            for (int i = 0; i < 60; ++i) { app.pump_async(); app.render(rn); SDL_Delay(60); }
            win->screenshot(std::string(shot) + "_favorites.png");
            // Settings submenu: open, then cycle Max Quality twice.
            app.open_main_menu();
            for (int i = 0; i < 4; ++i) app.input(ui::App::Action::Down);  // -> Settings
            app.input(ui::App::Action::Select);                            // open Settings
            app.render(rn); win->screenshot(std::string(shot) + "_settings.png");
            app.input(ui::App::Action::Right);                             // raise quality
            app.input(ui::App::Action::Right);                             // raise again
            app.render(rn); win->screenshot(std::string(shot) + "_settings_cycled.png");
            std::fprintf(stderr, "MAINMENUSHOT done\n");
            return 0;
        }
        if (std::getenv("YTC_SEARCHSHOT")) {
            app.input(ui::App::Action::Search);          // open the OSK
            app.render(rn);
            win->screenshot(std::string(shot) + "_kb_empty.png");
            app.input_text("blender");                   // simulate typing
            for (int i = 0; i < 3; ++i) app.input(ui::App::Action::Down);  // move cursor
            app.render(rn);
            win->screenshot(std::string(shot) + "_kb_typed.png");
            std::fprintf(stderr, "wrote OSK screenshots\n");
        }
        return 0;
    }

    // Load the community controller-mapping DB so buttons land on the right
    // SDL_CONTROLLER_BUTTON_* slots (SDL's built-in set misses many pads/
    // handhelds). Tried in order; YTC_GAMEPADDB overrides the path.
    {
        std::string dir = exe_dir();
        // Search cwd/data, <exe>/data, <exe>/../data, then system dirs.
        auto load = [&](const char* env, const char* base, const char* label) {
            std::string cands[] = { env ? std::string(env) : std::string(),
                                    std::string("data/") + base,
                                    dir + "/data/" + base,
                                    dir + "/../data/" + base,
                                    std::string("/opt/ytc/") + base,
                                    std::string("/usr/share/ytc/") + base };
            for (const auto& c : cands) {
                if (c.empty()) continue;
                int n = SDL_GameControllerAddMappingsFromFile(c.c_str());
                if (n >= 0) { std::fprintf(stderr, "gamepad: %s %d from %s\n", label, n, c.c_str()); return; }
            }
        };
        load(std::getenv("YTC_GAMEPADDB"), "gamecontrollerdb.txt", "loaded");
        // Local overrides win (loaded last) — fix pads not in the DB / wrong layout.
        load(std::getenv("YTC_GAMEPADDB_LOCAL"), "gamecontrollerdb_local.txt", "+overrides");
    }

    SDL_JoystickEventState(SDL_ENABLE);   // also deliver raw button events (for mapping diagnosis)

    // Interactive loop.
    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        pad = SDL_GameControllerOpen(i);
        if (!pad) continue;
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
        std::fprintf(stderr, "gamepad: \"%s\" (guid %s)\n",
                     SDL_GameControllerName(pad) ? SDL_GameControllerName(pad) : "?", guid);
        if (char* m = SDL_GameControllerMapping(pad)) {
            std::fprintf(stderr, "gamepad mapping: %s\n", m); SDL_free(m);
        }
        break;
    }

    // Gamepad diagnostic: log raw button indices + SDL names, act on nothing,
    // so every button can be pressed without quitting. End by closing the window.
    if (std::getenv("YTC_PADTEST")) {
        std::fprintf(stderr, "PADTEST: press A, B, X, Y one at a time (nothing will act).\n");
        bool run = true;
        while (run) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) run = false;
                else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) run = false;
                else if (e.type == SDL_JOYBUTTONDOWN)
                    std::fprintf(stderr, "  RAW button index = %d\n", e.jbutton.button);
                else if (e.type == SDL_CONTROLLERBUTTONDOWN)
                    std::fprintf(stderr, "     -> SDL maps it to: %s\n",
                        SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button));
            }
            app.pump_async(); app.render(rn); win->swap(); SDL_Delay(16);
        }
        return 0;
    }

    bool running = true;
    using A = ui::App::Action;
    using M = ui::App::Mode;
    // Back never quits: at the root grid it does nothing; in a channel subview it
    // pops one level; while playing/searching/in a menu the App handles it. Exit is
    // an explicit item in the Start menu (sets app.wants_quit()).
    auto handle = [&](A a) {
        if (a != A::None) app.input(a);
    };
    // Hold-to-seek: controller d-pad buttons don't auto-repeat, so track Left/Right
    // held during playback and inject repeated seek presses (feeds the App's
    // accumulate+debounce scrubber: the preview races while held, the seek fires on
    // release). Speeds up after 2.5s of holding.
    int   seek_dir = 0;             // -1 rewinding, +1 fast-forwarding, 0 idle
    Uint32 seek_hold_start = 0, seek_last_rep = 0;
    // Hold-to-navigate in the grid/carousel: the d-pad doesn't auto-repeat, so track a
    // held direction and inject repeated moves that ACCELERATE the longer it's held.
    A nav_action = A::None;
    Uint8 nav_btn = 255;
    Uint32 nav_hold_start = 0, nav_last_rep = 0;
    SDL_StartTextInput();   // enable SDL_TEXTINPUT events for the OSK
    bool swallow_text = false;  // eat the "/" TEXTINPUT that follows opening search via slash
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = false; }
            else if (e.type == SDL_WINDOWEVENT &&
                     (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                      e.window.event == SDL_WINDOWEVENT_RESIZED)) {
                win->refresh_size();
                app.on_resize();
            }
            else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (app.mode() == M::Search) {
                    // Typing goes through SDL_TEXTINPUT; here we handle control keys.
                    switch (k) {
                        case SDLK_RETURN: case SDLK_KP_ENTER: app.input(A::Search); break; // submit
                        case SDLK_ESCAPE: app.input(A::Back); break;                       // cancel
                        case SDLK_BACKSPACE: app.backspace(); break;
                        case SDLK_LEFT:  app.input(A::Left);  break;
                        case SDLK_RIGHT: app.input(A::Right); break;
                        case SDLK_UP:    app.input(A::Up);    break;
                        case SDLK_DOWN:  app.input(A::Down);  break;
                        default: break;
                    }
                } else if (k == SDLK_m) {
                    app.input(A::Menu);              // options menu for the current item
                } else if (k == SDLK_TAB) {
                    if (app.menu_open()) app.input(A::Back); else app.open_main_menu(); // top-level menu
                } else if (k == SDLK_SLASH) {
                    app.input(A::Search);            // open the keyboard
                    swallow_text = true;             // don't let this "/" land in the query
                } else if (k == SDLK_v) {
                    app.toggle_view();               // grid <-> carousel
                } else if (k == SDLK_q) {
                    app.cycle_tab(-1);               // keyboard L-shoulder: prev tab
                } else if (k == SDLK_e) {
                    app.cycle_tab(+1);               // keyboard R-shoulder: next tab
                } else {
                    handle(map_key(k));
                }
            }
            else if (e.type == SDL_TEXTINPUT) {
                if (swallow_text) { swallow_text = false; continue; }
                if (app.mode() == M::Search) app.input_text(e.text.text);
            }
            else if (e.type == SDL_JOYBUTTONDOWN) {
                if (std::getenv("YTC_DEBUG"))
                    std::fprintf(stderr, "RAW joystick button index: %d\n", e.jbutton.button);
            }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                Uint8 b = e.cbutton.button;
                if (std::getenv("YTC_DEBUG"))
                    std::fprintf(stderr, "gamepad button: %s\n",
                        SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b));
                if (b == SDL_CONTROLLER_BUTTON_Y) app.input(A::Search); // open/submit
                else if (b == SDL_CONTROLLER_BUTTON_BACK) app.input(A::Menu);   // Select btn = options
                else if (b == SDL_CONTROLLER_BUTTON_START) {                    // Start = top-level menu
                    if (app.menu_open()) app.input(A::Back); else app.open_main_menu();
                }
                else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)  app.cycle_tab(-1);  // L = prev tab
                else if (b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) app.cycle_tab(+1);  // R = next tab
                else handle(map_button(b));
                // Begin hold-to-seek tracking (playback only, menu closed).
                if (app.mode() == M::Playing && !app.menu_open() &&
                    (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                     b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                    seek_dir = (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? -1 : +1;
                    seek_hold_start = seek_last_rep = SDL_GetTicks();
                }
                // Begin hold-to-navigate tracking (grid/carousel browse, menu closed).
                if (app.mode() == M::Grid && !app.menu_open()) {
                    A na = map_button(b);
                    if (na == A::Up || na == A::Down || na == A::Left || na == A::Right) {
                        nav_action = na; nav_btn = b;
                        nav_hold_start = nav_last_rep = SDL_GetTicks();
                    }
                }
            }
            else if (e.type == SDL_CONTROLLERBUTTONUP) {
                Uint8 b = e.cbutton.button;
                if ((b == SDL_CONTROLLER_BUTTON_DPAD_LEFT  && seek_dir < 0) ||
                    (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && seek_dir > 0))
                    seek_dir = 0;
                if (b == nav_btn) { nav_action = A::None; nav_btn = 255; }
            }
        }
        if (app.wants_quit()) running = false;   // Exit chosen from the Start menu
        // Hold-to-seek repeats: after a 350ms hold, inject a seek press every 120ms
        // (50ms once held past 2.5s -> deep scrubbing without button mashing).
        if (seek_dir != 0) {
            if (app.mode() != M::Playing || app.menu_open()) {
                seek_dir = 0;                    // context changed; stop the hold
            } else {
                Uint32 now = SDL_GetTicks();
                Uint32 held = now - seek_hold_start;
                Uint32 interval = held > 2500 ? 50 : 120;
                if (held > 350 && now - seek_last_rep >= interval) {
                    app.input(seek_dir < 0 ? A::Left : A::Right);
                    seek_last_rep = now;
                }
            }
        }
        // Hold-to-navigate repeats with ACCELERATION: after a ~300ms hold the cursor
        // starts stepping ~150ms apart and speeds up to ~40ms the longer it's held.
        if (nav_action != A::None) {
            if (app.mode() != M::Grid || app.menu_open()) {
                nav_action = A::None; nav_btn = 255;   // context changed; stop
            } else {
                Uint32 now = SDL_GetTicks();
                Uint32 held = now - nav_hold_start;
                long interval = 150 - (long)(held / 12);   // ramp down as it's held
                if (interval < 40) interval = 40;
                if (held > 300 && now - nav_last_rep >= (Uint32)interval) {
                    app.input(nav_action);
                    nav_last_rep = now;
                }
            }
        }
        app.pump_async();
        app.render(rn);
        win->swap();
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    if (pad) SDL_GameControllerClose(pad);
    return 0;
}
