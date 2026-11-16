// Entry point: ksphmi [--demo] [--windowed] [--address HOST] [--screenshot FILE] [--bench N] ...
// Settings: config.toml + config.local.toml + KSPHMI_* environment (config.hpp); see --help.
#include <SDL.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <time.h>

#include "bootstate.hpp"
#include "canvas.hpp"
#include "config.hpp"
#include "display.hpp"
#include "fbdev.hpp"
#include "fmt.hpp"
#include "keyboard.hpp"
#include "redux_source.hpp"
#include "log.hpp"
#include "png.hpp"
#include "intro.hpp"
#include "splash.hpp"
#include "telemetry.hpp"

using namespace ksphmi;

namespace {

const char* LOG = "ksphmi";
volatile std::sig_atomic_t quit_signal = 0;
void on_signal(int) { quit_signal = 1; }
/// SIGUSR1: save the next frame exactly as the screen's renderer drew it (GPU included) to live_shot_path().
volatile std::sig_atomic_t shot_signal = 0;
void on_shot_signal(int) { shot_signal = 1; }

std::string live_shot_path() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return std::string(dir && *dir ? dir : "/tmp") + "/ksphmi-shot.png";
}

struct Args {
    std::string config, address, size, screenshot, skin, layer_cache, splash_state, monitor;
    bool demo = false, windowed = false, desktop = false, offscreen = false, splash = false, bench_anim = false;
    bool vessel_heat = false;
    double seconds = 1.5;
    int fps = 0, bench = 0;
};

void usage(FILE* out) {
    std::fprintf(out,
                 "usage: ksphmi [--config FILE] [--demo] [--address HOST] [--windowed | --desktop] [--size WxH]\n"
                 "              [--skin classic|apollo|steampunk] [--monitor auto|N]\n"
                 "              [--screenshot FILE] [--seconds S] [--fps N] [--bench N [--bench-anim]] [--offscreen]\n"
                 "              [--layer-cache auto|on|off]\n"
                 "       ksphmi --splash [--screenshot FILE --size WxH [--splash-state STAGE]]\n\n"
                 "KSP flight data display (kRPC client), for the Raspberry Pi.\n\n"
                 "  --config FILE      settings file instead of config.toml + config.local.toml\n"
                 "  --demo             simulated orbit, no game needed\n"
                 "  --address HOST     kRPC server address (overrides the settings, disables discovery)\n"
                 "  --skin NAME        classic, apollo or steampunk (overrides [display] skin)\n"
                 "  --windowed         window instead of full screen (development on a PC)\n"
                 "  --desktop          on the PC that runs the game (./ci hmi): fills the chosen secondary monitor\n"
                 "                     (full-screen desktop), else a window; KSPHMI_WSLG=1: always a window\n"
                 "  --monitor M        auto (the last display when there are several) or a display index\n"
                 "                     (overrides [display] monitor)\n"
                 "  --size WxH         window or screen size, for example 1920x1080\n"
                 "  --screenshot FILE  draw for --seconds, save the frame as PNG and exit (off-screen)\n"
                 "  --seconds S        how long --screenshot waits for data (default 1.5)\n"
                 "  --fps N            frames per second (default: [display] fps)\n"
                 "  --bench N          draw N demo frames, print HMI_RENDER_MS / HMI_PRESENT_MS and exit\n"
                 "  --bench-anim       with --bench: one sample, only the NavBall attitude changes (NavBall-only\n"
                 "                     frames over the cached layer; full frames when the layer cache is off)\n"
                 "  --layer-cache M    auto (GPU renderer only), on or off (overrides [display] layer_cache)\n"
                 "  --vessel-heat      start the VESSEL panel on heat instead of fuel (Ctrl+0 switches)\n"
                 "  --offscreen        draw in memory only (never touches the screen; for --bench)\n"
                 "  --splash           animate the boot loading screen on $KSPHMI_FBDEV (default /dev/fb0) until\n"
                 "                     the HMI writes $XDG_RUNTIME_DIR/ksphmi.ready; reads no settings\n"
                 "                     (ksphmi-splash.service, early boot)\n"
                 "  --splash-state S   with --splash --screenshot: system, network, offline, hmi, ready or failed\n");
}

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc) throw std::runtime_error(k + " needs a value");
            out = argv[++i];
        };
        std::string v;
        if (k == "-h" || k == "--help") {
            usage(stdout);
            std::exit(0);
        } else if (k == "--config") value(a.config);
        else if (k == "--address") value(a.address);
        else if (k == "--skin") value(a.skin);
        else if (k == "--size") value(a.size);
        else if (k == "--screenshot") value(a.screenshot);
        else if (k == "--seconds") value(v), a.seconds = std::stod(v);
        else if (k == "--fps") value(v), a.fps = std::stoi(v);
        else if (k == "--bench") value(v), a.bench = std::stoi(v);
        else if (k == "--demo") a.demo = true;
        else if (k == "--windowed") a.windowed = true;
        else if (k == "--desktop") a.desktop = a.windowed = true;  // never the kiosk outputs or the boot hand-over
        else if (k == "--monitor") value(a.monitor);
        else if (k == "--offscreen") a.offscreen = true;
        else if (k == "--vessel-heat") a.vessel_heat = true;
        else if (k == "--bench-anim") a.bench_anim = true;
        else if (k == "--layer-cache") value(a.layer_cache);
        else if (k == "--splash") a.splash = true;
        else if (k == "--splash-state") value(a.splash_state);
        else throw std::runtime_error("unknown option " + k);
    }
    return true;
}

/// CPU time of this thread, in seconds: tells waiting (vsync / page flip) from work inside present.
double thread_cpu() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;  // 100 ns units
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    auto ticks = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}

/// Where frames go: an SDL window (GPU renderer when available; kmsdrm on the Pi console), the Linux
/// framebuffer, or memory only. The last two use SDL's software renderer on an ARGB8888 surface.
/// Output "auto" (the Pi service) tries the GPU through SDL first and falls back to the framebuffer when SDL
/// cannot open the display or only offers a software renderer: measured on the Pi 4 at 2520x1680, the V3D GPU
/// draws a frame in 4.5 ms against 37 ms in software (present 6 ms against 9 ms).
class Output {
public:
    Output(const Config& cfg, const Args& args, int want_w, int want_h) {
        const char* env = std::getenv("KSPHMI_OUTPUT");
        std::string output = env && *env ? env : cfg.display.output;
        bool offscreen = args.offscreen || !args.screenshot.empty();
#if !defined(__linux__)
        // The framebuffer is the Pi's (Linux /dev/fb0): elsewhere "fbdev" and "auto" draw through SDL.
        if (!offscreen && output == "fbdev" && !args.windowed) {
            log::warning(LOG, "fbdev output needs Linux, using SDL");
            output = "sdl";
        }
#endif
        if (!offscreen && output == "auto" && !args.windowed) {
            std::string why;
            if (open_sdl(cfg, args, want_w, want_h, true, why)) return;
            close_sdl();
#if defined(__linux__)
            log::warning(LOG, "GPU output unavailable (" + why + "), using the framebuffer");
            output = "fbdev";
#else
            log::warning(LOG, "GPU output unavailable (" + why + "), using SDL's software renderer");
            output = "sdl";
#endif
        }
        if (offscreen || (output == "fbdev" && !args.windowed)) {
            if (!offscreen) {
                fb_ = std::make_unique<Framebuffer>(cfg.display.fbdev);
                want_w = fb_->width(), want_h = fb_->height();
                kind_ = "fbdev " + cfg.display.fbdev + " " + std::to_string(fb_->bits_per_pixel()) + " bpp";
            } else {
                if (want_w <= 0 || want_h <= 0) want_w = 1280, want_h = 800;
                kind_ = "off-screen";
            }
            surface_ = SDL_CreateRGBSurfaceWithFormat(0, want_w, want_h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!surface_) throw std::runtime_error(std::string("SDL surface: ") + SDL_GetError());
            renderer_ = SDL_CreateSoftwareRenderer(surface_);
            if (!renderer_) throw std::runtime_error(std::string("software renderer: ") + SDL_GetError());
            w_ = want_w, h_ = want_h;
            return;
        }
        std::string why;
        if (!open_sdl(cfg, args, want_w, want_h, false, why)) throw std::runtime_error(why);
    }

    ~Output() {
        close_sdl();
        if (surface_) SDL_FreeSurface(surface_);
    }

    void repaint_all() {
        if (fb_) fb_->repaint_all();
    }

    void present() {
        if (window_) SDL_RenderPresent(renderer_);
        else {
            // The CPU surface must include every queued software-renderer command before scanout.
            if (SDL_RenderFlush(renderer_) != 0)
                throw std::runtime_error(std::string("software renderer flush: ") + SDL_GetError());
            if (fb_) fb_->write(surface_);
        }
    }

    bool screenshot(const std::string& path) {
        if (surface_) return SDL_RenderFlush(renderer_) == 0 && save_png(surface_, path);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w_, h_, 32, SDL_PIXELFORMAT_ARGB8888);
        bool ok = s && SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
                  save_png(s, path);
        if (s) SDL_FreeSurface(s);
        return ok;
    }

    /// Re-reads the renderer's pixel size after a window or display change; true when it differs.
    bool refresh_size() {
        if (!window_) return false;
        int w = 0, h = 0;
        if (SDL_GetRendererOutputSize(renderer_, &w, &h) != 0 || w <= 0 || h <= 0) return false;
        if (w == w_ && h == h_) return false;
        w_ = w, h_ = h;
        return true;
    }

    SDL_Renderer* renderer() const { return renderer_; }
    bool video() const { return video_; }
    bool accelerated() const { return accelerated_; }
    int w() const { return w_; }
    int h() const { return h_; }
    const std::string& kind() const { return kind_; }

private:
    /// SDL window + renderer. need_gpu: fail (instead of using SDL's software renderer) when no accelerated one exists.
    bool open_sdl(const Config& cfg, const Args& args, int want_w, int want_h, bool need_gpu, std::string& why) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return why = std::string("SDL video: ") + SDL_GetError(), false;
        video_ = true;
        const int displays = SDL_GetNumVideoDisplays();
        const int monitor = monitor_index(cfg.display.monitor, displays);
        // --desktop shares the PC with the game, on the chosen monitor. A secondary monitor gets the whole of it
        // (full-screen desktop: no mode change, the game keeps its own screen); the primary one a plain window.
        // Under WSLg (./ci hmi sets KSPHMI_WSLG=1) a secondary monitor gets a plain window instead: WSLg
        // (2026-11-15, secondary monitor at 175% scale) showed nothing for full-screen, borderless or maximized
        // X11 windows; only a plain decorated window was visible. Its display bounds are physical pixels while
        // Windows places the window in logical units, so centering misses: anchor that window at the display
        // origin (exact) and let ./ci hmi pass that monitor's logical work area as --size.
        const char* wslg_env = std::getenv("KSPHMI_WSLG");
        const bool wslg = wslg_env && *wslg_env && std::string(wslg_env) != "0";
        SDL_Rect bounds{};
        const bool anchor = args.desktop && monitor != 0 && wslg && SDL_GetDisplayBounds(monitor, &bounds) == 0;
        const bool desktop_fill = args.desktop && monitor != 0 && !wslg;  // --desktop also sets windowed
        bool fullscreen = !args.desktop && cfg.display.fullscreen && !args.windowed;
        Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
        if (fullscreen) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_DisplayMode mode;
            if (SDL_GetDesktopDisplayMode(monitor, &mode) == 0) want_w = mode.w, want_h = mode.h;
        }
        if (desktop_fill) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            if (want_w <= 0 || want_h <= 0) want_w = 1280, want_h = 800;  // the window size before it fills
        }
        if (want_w <= 0 || want_h <= 0) {
            SDL_DisplayMode mode;
            if (fullscreen && SDL_GetDesktopDisplayMode(monitor, &mode) == 0) want_w = mode.w, want_h = mode.h;
            else want_w = 1280, want_h = 800;
        }
        log::info(LOG, fmt::str("display %d of %d (monitor %s)%s", monitor, displays, cfg.display.monitor.c_str(),
                                fullscreen ? ", full screen" : desktop_fill ? ", full-screen desktop" : ", window"));
        const int x = anchor ? bounds.x : SDL_WINDOWPOS_CENTERED_DISPLAY(monitor);
        const int y = anchor ? bounds.y : SDL_WINDOWPOS_CENTERED_DISPLAY(monitor);
        window_ = SDL_CreateWindow("KSP HMI", x, y, want_w, want_h, flags);
        if (!window_) return why = std::string("SDL window: ") + SDL_GetError(), false;
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            if (need_gpu) return why = std::string("no GPU renderer: ") + SDL_GetError(), false;
            log::warning(LOG, std::string("no GPU renderer (") + SDL_GetError() + "), using software");
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) return why = std::string("SDL renderer: ") + SDL_GetError(), false;
        SDL_RendererInfo info;
        SDL_GetRendererInfo(renderer_, &info);
        accelerated_ = (info.flags & SDL_RENDERER_ACCELERATED) != 0;
        if (need_gpu && !accelerated_) return why = std::string("only a software renderer (") + info.name + ")", false;
        SDL_GetRendererOutputSize(renderer_, &w_, &h_);
        if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
        kind_ = std::string("SDL ") + SDL_GetCurrentVideoDriver() + " / " + info.name;
        return true;
    }

    void close_sdl() {
        if (renderer_) SDL_DestroyRenderer(renderer_), renderer_ = nullptr;
        if (window_) SDL_DestroyWindow(window_), window_ = nullptr;
        if (video_) SDL_QuitSubSystem(SDL_INIT_VIDEO), video_ = false;
        accelerated_ = false;
    }

    std::unique_ptr<Framebuffer> fb_;
    SDL_Surface* surface_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool video_ = false, accelerated_ = false;
    int w_ = 0, h_ = 0;
    std::string kind_;
};

/// SDL key event -> Key. Letters and digits by key code (the character on the user's layout, as KSP's own key
/// bindings read them); , . / also by physical position, since some layouts need Shift for them.
KeyEvent key_event(const SDL_KeyboardEvent& k) {
    KeyEvent e;
    e.ctrl = (k.keysym.mod & KMOD_CTRL) != 0;
    e.alt = (k.keysym.mod & KMOD_ALT) != 0;
    const SDL_Keycode s = k.keysym.sym;
    const SDL_Scancode sc = k.keysym.scancode;
    if (s >= SDLK_0 && s <= SDLK_9) return e.key = Key::Digit, e.digit = int(s - SDLK_0), e;
    if (s >= SDLK_KP_1 && s <= SDLK_KP_9) return e.key = Key::Digit, e.digit = int(s - SDLK_KP_1) + 1, e;
    if (s == SDLK_KP_0) return e.key = Key::Digit, e.digit = 0, e;
    if (s == SDLK_COMMA || sc == SDL_SCANCODE_COMMA) return e.key = Key::Comma, e;
    if (s == SDLK_PERIOD || sc == SDL_SCANCODE_PERIOD) return e.key = Key::Period, e;
    if (s == SDLK_SLASH || sc == SDL_SCANCODE_SLASH) return e.key = Key::Slash, e;
    static const std::pair<SDL_Keycode, Key> keys[] = {
        {SDLK_ESCAPE, Key::Escape}, {SDLK_t, Key::T}, {SDLK_r, Key::R}, {SDLK_f, Key::F}, {SDLK_z, Key::Z},
        {SDLK_x, Key::X}, {SDLK_g, Key::G}, {SDLK_u, Key::U}, {SDLK_b, Key::B}, {SDLK_m, Key::M}, {SDLK_SPACE, Key::Space},
        {SDLK_BACKSPACE, Key::Backspace}, {SDLK_F5, Key::F5}, {SDLK_F9, Key::F9}, {SDLK_w, Key::W},
        {SDLK_s, Key::S}, {SDLK_a, Key::A}, {SDLK_d, Key::D}, {SDLK_q, Key::Q}, {SDLK_e, Key::E}, {SDLK_h, Key::H},
        {SDLK_n, Key::N}, {SDLK_i, Key::I}, {SDLK_k, Key::K}, {SDLK_j, Key::J}, {SDLK_l, Key::L},
        {SDLK_LSHIFT, Key::LeftShift}, {SDLK_LCTRL, Key::LeftCtrl}};
    for (const auto& [code, key] : keys)
        if (s == code) return e.key = key, e;
    return e;
}

std::unique_ptr<Source> make_source(const Config& cfg, const Args& args) {
    if (args.demo || cfg.source == "demo") return std::make_unique<DemoSource>(cfg.demo.speed, cfg.krpc.rate_hz);
    KrpcSource::Options o;
    o.address = cfg.krpc.address;
    o.rpc_port = cfg.krpc.rpc_port;
    o.stream_port = cfg.krpc.stream_port;
    o.rate_hz = cfg.krpc.rate_hz;
    o.retry_seconds = cfg.krpc.retry_seconds;
    o.discovery = cfg.krpc.discovery && args.address.empty();
    // KSP 2 Redux (docs/hmi.rst "KSP 2 Redux source"): config source, or KSPHMI_SOURCE for older Pi drop-ins.
    const char* env = std::getenv("KSPHMI_SOURCE");
    const std::string mode = env && *env ? env : cfg.source;
    if (mode == "redux" || mode == "auto") return std::make_unique<ReduxSource>(o, mode == "auto");
    return std::make_unique<KrpcSource>(o);
}

/// --splash: before any settings, network or SDL video; --screenshot saves the same picture off-screen instead.
int splash(const Args& args) {
#if !defined(__linux__)
    if (args.screenshot.empty())
        throw std::runtime_error("--splash animates the Pi's framebuffer (Linux /dev/fb0), not available here; "
                                 "--splash --screenshot FILE still saves its picture");
#endif
    if (SDL_Init(0) != 0) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    int code = 0;
    if (args.screenshot.empty()) {
        const char* dev = std::getenv("KSPHMI_FBDEV");
        code = run_splash(dev && *dev ? dev : "/dev/fb0");
    } else {
        int w = 1280, h = 800;
        size_t x = args.size.find_first_of("xX");
        if (x != std::string::npos) w = std::stoi(args.size.substr(0, x)), h = std::stoi(args.size.substr(x + 1));
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_Renderer* r = s ? SDL_CreateSoftwareRenderer(s) : nullptr;
        if (!r) throw std::runtime_error(std::string("software renderer: ") + SDL_GetError());
        SDL_DestroyRenderer(r);  // only proves the surface works; SplashPainter owns its renderer
        {
            const BootView view = boot_view(boot_demo_inputs(args.splash_state.empty() ? "system" : args.splash_state,
                                                             w, h));
            SplashPainter painter(s);
            painter.draw(view, SplashAnim{view.progress, view.done ? 0.9 : 0.0, 1.0});
        }
        code = save_png(s, args.screenshot) ? 0 : 1;
        SDL_FreeSurface(s);
    }
    TTF_Quit();
    SDL_Quit();
    return code;
}

int run(int argc, char** argv) {
    Args args;
    try {
        parse(argc, argv, args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ksphmi: %s\n", e.what());
        usage(stderr);
        return 2;
    }
    if (args.splash) return splash(args);
    Config cfg = load_config(args.config);
    if (!args.skin.empty()) cfg.display.skin = args.skin;
    if (!args.monitor.empty()) {
        if (!valid_monitor(args.monitor)) throw std::runtime_error("--monitor must be auto or a display index");
        cfg.display.monitor = args.monitor;
    }
    if (!args.address.empty()) cfg.krpc.address = args.address;  // one place: the source and the waiting screen
    int want_w = 0, want_h = 0;
    if (!args.size.empty()) {
        size_t x = args.size.find_first_of("xX");
        if (x == std::string::npos) throw std::runtime_error("--size must look like 1920x1080");
        want_w = std::stoi(args.size.substr(0, x)), want_h = std::stoi(args.size.substr(x + 1));
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGUSR1
    std::signal(SIGUSR1, on_shot_signal);
#endif
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(0) != 0) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());

    // Boot hand-over: the splash (ksphmi-splash.service) draws until this file exists; it is written after the
    // first presented frame. Only the real screen announces itself, never a window, a screenshot or a benchmark,
    // and only on Linux: the boot splash is a Linux framebuffer service (the Pi), absent on macOS or a PC.
#if defined(__linux__)
    const bool announce = !args.bench && args.screenshot.empty() && !args.offscreen && !args.windowed;
#else
    const bool announce = false;
#endif
    if (announce) remove_ready_file();
    // The boot intro video (ksphmi-splash.service, mpv on DRM) holds DRM master on its last frame: wait for its end
    // and make mpv quit before SDL video opens the display. Bounded; any failure is logged and the HMI goes on.
    if (announce && !args.desktop) finish_boot_intro();
    std::unique_ptr<Source> source;
    if (!args.bench) source = make_source(cfg, args);
    log::info(LOG, fmt::str("ksphmi %s (C++), source %s, settings %s", KSPHMI_VERSION,
                            source ? source->name() : "bench",
                            cfg.loaded.empty() ? "defaults" : cfg.loaded.front().c_str()));
    if (source) source->start();
    int code = 0;
    {
        Output out(cfg, args, want_w, want_h);
        if (!args.size.empty() && out.video() && !args.windowed)
            log::warning(LOG, "--size ignored: a real screen uses its native resolution");
        // Canvas and Display are sized for the screen: rebuilt when the window or display changes size.
        std::unique_ptr<Canvas> canvas;
        std::unique_ptr<Display> display;
        std::string key_skin;  // Ctrl+1 / Ctrl+2 / Ctrl+3 on the keyboard; survives a rebuild for a new screen size
        Keyboard keyboard;     // the HMI keyboard as a remote KSP keyboard (keyboard.hpp)
        bool vessel_heat = args.vessel_heat;  // Ctrl+0: the VESSEL panel shows heat instead of fuel
        auto build_view = [&] {
            display.reset();
            canvas.reset();
            canvas = std::make_unique<Canvas>(out.renderer(), out.w(), out.h(), out.accelerated());
            display = std::make_unique<Display>(cfg, *canvas);
            display->choose_skin(key_skin);
            display->set_vessel_heat(vessel_heat);
            log::info(LOG, fmt::str("screen %dx%d (%s)", out.w(), out.h(), out.kind().c_str()));
            const std::string mode = args.layer_cache.empty() ? cfg.display.layer_cache : args.layer_cache;
            const bool want = mode == "on" || (mode != "off" && out.accelerated());
            const bool on = display->set_layer_cache(want);
            log::info(LOG, fmt::str("layer cache: %s (%s%s)", on ? "on" : "off", mode.c_str(),
                                    want && !on ? ", no render targets" : ""));
        };
        build_view();
        const int fps = std::max(1, args.fps ? args.fps : cfg.display.fps);
        const double frame = 1.0 / fps;
        const double deadline = monotonic() + args.seconds;
        std::shared_ptr<const Snapshot> last;
        double draw_ms = 0, present_ms = 0, present_cpu_ms = 0, perf_since = monotonic();
        double ready_at = -1;  // when the ready file was written; one full repaint follows a second later
        bool repainted = false;
        double full_ms = 0, anim_ms = 0;  // draw time split: full (cached layer) frames, NavBall-only frames
        int frames = 0, full_frames = 0, anim_frames = 0;
        DemoSource bench_source;
        const auto bench_snap = std::make_shared<const Snapshot>(bench_source.sample(0));
        // --bench-anim turns the attitude 1.5 degrees per frame around a tilted axis; the sample never changes.
        auto bench_attitude = [](int i) { return attitude(10 + 20 * std::sin(i * .05), i * 1.5, 15 * std::cos(i * .03)); };
        if (args.bench) {  // warm-up frame: fonts, label atlas and lighting are built once, outside the timing
            if (args.bench_anim) display->navball().hold = bench_attitude(0);
            display->draw(*bench_snap, monotonic());
            if (args.bench_anim) last = bench_snap;
            out.present();
        }
        // Apply what a key asked for: game commands to the source (its own thread sends them), skin, quit.
        auto apply_keys = [&](const KeyResult& r) {
            if (r.quit) quit_signal = 1;
            if (source)
                for (const Command& c : r.commands) source->command(c);
            if (r.vessel_mode) {
                vessel_heat = !vessel_heat;
                display->set_vessel_heat(vessel_heat);
                log::info(LOG, std::string("VESSEL panel: ") + (vessel_heat ? "heat" : "fuel"));
                last = nullptr;  // redraw now
            }
            if (!r.skin.empty()) {
                key_skin = r.skin;
                display->choose_skin(key_skin);
                last = nullptr;  // redraw now in the chosen skin
            }
        };
        for (int i = 0; !quit_signal; ++i) {
            double tick = monotonic();
            if (out.video()) {
                SDL_Event ev;
                bool resized = false, reset = false;
                while (SDL_PollEvent(&ev)) {
                    // Keys (keyboard.hpp): KSP's default flight keys go to the game, Ctrl+1 / Ctrl+2 / Ctrl+3 pick the
                    // skin, Esc quits. Auto-repeat is not a new press.
                    if (ev.type == SDL_QUIT)
                        quit_signal = 1;
                    else if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
                        apply_keys(keyboard.down(key_event(ev.key), tick));
                    else if (ev.type == SDL_KEYUP)
                        apply_keys(keyboard.up(key_event(ev.key), tick));
                    else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        apply_keys(keyboard.release_all());  // no key-up events follow: never leave keys held
                    else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                        resized = true;
                    else if (ev.type == SDL_RENDER_TARGETS_RESET || ev.type == SDL_RENDER_DEVICE_RESET)
                        reset = true;  // cached textures are gone: rebuild them even at the same size
                }
                apply_keys(keyboard.tick(tick));
                if (source) source->hold(keyboard.held(tick));
                const bool changed = (resized || reset) && out.refresh_size();
                if (changed || reset) {
                    build_view();
                    last = nullptr;  // full redraw at the new size
                }
            }
            std::shared_ptr<const Snapshot> snap;
            if (!args.bench) snap = source->latest();
            else if (args.bench_anim) snap = bench_snap, display->navball().hold = bench_attitude(i + 1);
            else snap = std::make_shared<const Snapshot>(bench_source.sample(i + 1));
            // Redraw on new data; animated or ageing screens (waiting, stale counter) every tick. Between samples
            // only the NavBall turns: a NavBall-only frame over the cached layer when there is one.
            const bool shot = shot_signal != 0;
            bool full = shot || snap != last || snap->state != State::Flight || stale(*snap, tick);
            bool anim = !full && snap->state == State::Flight && display->navball().animating();
            if (full || anim) {
                double t0 = monotonic();
                if (!(anim && display->animate())) display->draw(*snap, t0), anim = false;
                double t1 = monotonic(), c1 = thread_cpu();
                if (shot) {  // before present: the back buffer is undefined after a page flip
                    shot_signal = 0;
                    if (out.screenshot(live_shot_path())) log::info(LOG, "live screenshot: " + live_shot_path());
                    else log::warning(LOG, "live screenshot failed: " + std::string(SDL_GetError()));
                }
                out.present();
                double t2 = monotonic(), c2 = thread_cpu();
                draw_ms += (t1 - t0) * 1000, present_ms += (t2 - t1) * 1000, present_cpu_ms += (c2 - c1) * 1000, ++frames;
                if (anim) anim_ms += (t1 - t0) * 1000, ++anim_frames;
                else full_ms += (t1 - t0) * 1000, ++full_frames;
                last = snap;
                if (announce && ready_at < 0) {
                    ready_at = t2;
                    if (write_ready_file()) log::info(LOG, "first frame presented: " + ready_file_path());
                    else log::warning(LOG, "cannot write " + ready_file_path() + ": the boot splash may linger");
                } else if (announce && !repainted && t2 - ready_at > 1.0) {
                    // On the framebuffer the splash may have written one last frame after ours: rewrite every row.
                    out.repaint_all();
                    repainted = true;
                }
            }
            if (args.bench && frames >= args.bench) {
                std::printf("HMI_RENDER_MS=%.1f HMI_PRESENT_MS=%.1f HMI_PRESENT_CPU_MS=%.1f frames=%d full=%d "
                            "navball_only=%d layer_cache=%s size=%dx%d output=%s\n",
                            draw_ms / frames, present_ms / frames, present_cpu_ms / frames, frames, full_frames,
                            anim_frames, display->layer_cache() ? "on" : "off", out.w(), out.h(), out.kind().c_str());
                break;
            }
            if (!args.screenshot.empty() && tick >= deadline) {
                code = out.screenshot(args.screenshot) ? 0 : 1;
                if (!code) log::info(LOG, "saved " + args.screenshot);
                break;
            }
            if (!args.bench && tick - perf_since >= 60 && frames) {
                log::info(LOG, fmt::str("perf: %d frames in %.0f s, draw %.1f ms, present %.1f ms (cpu %.1f ms) per "
                                        "frame; full %d at %.1f ms, navball-only %d at %.1f ms",
                                        frames, tick - perf_since, draw_ms / frames, present_ms / frames,
                                        present_cpu_ms / frames, full_frames, full_frames ? full_ms / full_frames : 0.0,
                                        anim_frames, anim_frames ? anim_ms / anim_frames : 0.0));
                draw_ms = present_ms = present_cpu_ms = full_ms = anim_ms = 0;
                frames = full_frames = anim_frames = 0, perf_since = tick;
            }
            if (!args.bench) {
                double left = tick + frame - monotonic();
                if (left > 0) std::this_thread::sleep_for(std::chrono::duration<double>(left));
            }
        }
    }
    if (source) source->stop();
    if (announce) remove_ready_file();  // a splash started by hand after this can draw again
    TTF_Quit();
    SDL_Quit();
    return code;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SDL_SetMainReady();  // SDL_MAIN_HANDLED (CMake): this main() is the entry point, console logging included
#endif
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        log::error(LOG, e.what());
        return 1;
    }
}
