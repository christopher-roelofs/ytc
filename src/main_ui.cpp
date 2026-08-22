// UI entry point.
//   ./yt_ui "<query>"                  interactive (KMSDRM/X11), gamepad+keyboard
//   YTNATIVE_SHOT=out ./yt_ui "<q>"    headless: render frames to out_NN.png
//
// Headless mode uses the SDL "offscreen" driver so the exact GLES2 UI path is
// exercised with no display, letting us verify the UI visually via screenshots.
#include "ui.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>   // readlink (exe_dir)

static const char* config_path() {
    const char* e = std::getenv("YTNATIVE_CONFIG");
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
    std::string query = argc > 1 ? argv[1] : "lofi hip hop radio";
    const char* shot = std::getenv("YTNATIVE_SHOT");
    bool headless = shot != nullptr;

    // Driver: respect an explicit SDL_VIDEODRIVER from the environment (e.g.
    // kmsdrm on a device whose SDL lacks the offscreen backend); otherwise
    // default screenshots to offscreen and interactive to auto.
    std::string driver;
    if (std::getenv("SDL_VIDEODRIVER")) driver = "";      // honor the env value
    else if (headless)                  driver = "offscreen";
    // Initial window size (YTNATIVE_WINSIZE=WxH), e.g. 640x480 for a handheld.
    int win_w = 1280, win_h = 720;
    if (const char* ws = std::getenv("YTNATIVE_WINSIZE")) {
        int a, b; if (sscanf(ws, "%dx%d", &a, &b) == 2 && a > 0 && b > 0) { win_w = a; win_h = b; }
    }
    auto win = gfx::Window::create(win_w, win_h, "ytnative", driver);
    if (!win) { std::fprintf(stderr, "window create failed\n"); return 1; }

    gfx::Renderer rn;
    ui::App app(config_path(), win.get());
    std::fprintf(stderr, "searching '%s'...\n", query.c_str());
    app.search(query);

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
        if (const char* pid = std::getenv("YTNATIVE_PLAYID")) {
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
        if (std::getenv("YTNATIVE_PLAYTEST"))
            for (auto& st : play_steps) run_step(st);
        if (std::getenv("YTNATIVE_SEQTEST")) {
            // Reproduce the "2nd+ video fails" bug: play several grid items in a
            // row (select -> play -> back -> next). Each must actually PLAY.
            auto settle = [&](int ms){ int w=0; do { app.pump_async(); app.render(rn);
                win->swap(); SDL_Delay(50); w+=50; } while (w<ms); };
            int n = std::getenv("YTNATIVE_SEQN") ? atoi(std::getenv("YTNATIVE_SEQN")) : 4;
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
        if (std::getenv("YTNATIVE_CAROUSELSHOT")) {
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
        if (std::getenv("YTNATIVE_SEARCHSHOT")) {
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
    // handhelds). Tried in order; YTNATIVE_GAMEPADDB overrides the path.
    {
        std::string dir = exe_dir();
        // Search cwd/data, <exe>/data, <exe>/../data, then system dirs.
        auto load = [&](const char* env, const char* base, const char* label) {
            std::string cands[] = { env ? std::string(env) : std::string(),
                                    std::string("data/") + base,
                                    dir + "/data/" + base,
                                    dir + "/../data/" + base,
                                    std::string("/opt/ytnative/") + base,
                                    std::string("/usr/share/ytnative/") + base };
            for (const auto& c : cands) {
                if (c.empty()) continue;
                int n = SDL_GameControllerAddMappingsFromFile(c.c_str());
                if (n >= 0) { std::fprintf(stderr, "gamepad: %s %d from %s\n", label, n, c.c_str()); return; }
            }
        };
        load(std::getenv("YTNATIVE_GAMEPADDB"), "gamecontrollerdb.txt", "loaded");
        // Local overrides win (loaded last) — fix pads not in the DB / wrong layout.
        load(std::getenv("YTNATIVE_GAMEPADDB_LOCAL"), "gamecontrollerdb_local.txt", "+overrides");
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
    if (std::getenv("YTNATIVE_PADTEST")) {
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
    // Back quits from the grid, but only returns to the grid while playing.
    auto handle = [&](A a) {
        if (a == A::Back && app.mode() == M::Grid) running = false;
        else if (a != A::None) app.input(a);
    };
    SDL_StartTextInput();   // enable SDL_TEXTINPUT events for the OSK
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
                } else if (k == SDLK_SLASH || k == SDLK_TAB) {
                    app.input(A::Search);            // open the keyboard
                } else if (k == SDLK_v) {
                    app.toggle_view();               // grid <-> carousel
                } else {
                    handle(map_key(k));
                }
            }
            else if (e.type == SDL_TEXTINPUT) {
                if (app.mode() == M::Search) app.input_text(e.text.text);
            }
            else if (e.type == SDL_JOYBUTTONDOWN) {
                if (std::getenv("YTNATIVE_DEBUG"))
                    std::fprintf(stderr, "RAW joystick button index: %d\n", e.jbutton.button);
            }
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                Uint8 b = e.cbutton.button;
                if (std::getenv("YTNATIVE_DEBUG"))
                    std::fprintf(stderr, "gamepad button: %s\n",
                        SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b));
                if (b == SDL_CONTROLLER_BUTTON_Y) app.input(A::Search); // open/submit
                else if (b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) app.toggle_view();
                else handle(map_button(b));
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
