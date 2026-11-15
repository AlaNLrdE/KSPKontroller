// Entry point: ksphmi [--demo] [--windowed] [--address HOST] [--screenshot FILE] [--bench N] ...
// Settings: config.toml + config.local.toml + KSPHMI_* environment (config.hpp); see --help.
#include <SDL.h>

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
#include "log.hpp"
#include "png.hpp"
#include "splash.hpp"
#include "telemetry.hpp"

using namespace ksphmi;

namespace {

const char* LOG = "ksphmi";
volatile std::sig_atomic_t quit_signal = 0;
void on_signal(int) { quit_signal = 1; }

struct Args {
    std::string config, address, size, screenshot, skin, layer_cache, splash_state;
    bool demo = false, windowed = false, offscreen = false, splash = false, bench_anim = false;
    double seconds = 1.5;
    int fps = 0, bench = 0;
};

void usage(FILE* out) {
    std::fprintf(out,
                 "usage: ksphmi [--config FILE] [--demo] [--address HOST] [--windowed] [--size WxH]\n"
                 "              [--skin classic|apollo]\n"
                 "              [--screenshot FILE] [--seconds S] [--fps N] [--bench N [--bench-anim]] [--offscreen]\n"
                 "              [--layer-cache auto|on|off]\n"
                 "       ksphmi --splash [--screenshot FILE --size WxH [--splash-state STAGE]]\n\n"
                 "KSP flight data display (kRPC client), for the Raspberry Pi.\n\n"
                 "  --config FILE      settings file instead of config.toml + config.local.toml\n"
                 "  --demo             simulated orbit, no game needed\n"
                 "  --address HOST     kRPC server address (overrides the settings, disables discovery)\n"
                 "  --skin NAME        classic or apollo (overrides [display] skin)\n"
                 "  --windowed         window instead of full screen (development on a PC)\n"
                 "  --size WxH         window or screen size, for example 1920x1080\n"
                 "  --screenshot FILE  draw for --seconds, save the frame as PNG and exit (off-screen)\n"
                 "  --seconds S        how long --screenshot waits for data (default 1.5)\n"
                 "  --fps N            frames per second (default: [display] fps)\n"
                 "  --bench N          draw N demo frames, print HMI_RENDER_MS / HMI_PRESENT_MS and exit\n"
                 "  --bench-anim       with --bench: one sample, only the NavBall attitude changes (NavBall-only\n"
                 "                     frames over the cached layer; full frames when the layer cache is off)\n"
                 "  --layer-cache M    auto (GPU renderer only), on or off (overrides [display] layer_cache)\n"
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
        else if (k == "--offscreen") a.offscreen = true;
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
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
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
        if (!offscreen && output == "auto" && !args.windowed) {
            std::string why;
            if (open_sdl(cfg, args, want_w, want_h, true, why)) return;
            log::warning(LOG, "GPU output unavailable (" + why + "), using the framebuffer");
            close_sdl();
            output = "fbdev";
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
        else if (fb_) fb_->write(surface_);
    }

    bool screenshot(const std::string& path) {
        if (surface_) return save_png(surface_, path);
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w_, h_, 32, SDL_PIXELFORMAT_ARGB8888);
        bool ok = s && SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0 &&
                  save_png(s, path);
        if (s) SDL_FreeSurface(s);
        return ok;
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
        bool fullscreen = cfg.display.fullscreen && !args.windowed;
        Uint32 flags = 0;
        if (fullscreen) flags |= (want_w > 0 && want_h > 0) ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (want_w <= 0 || want_h <= 0) {
            SDL_DisplayMode mode;
            if (fullscreen && SDL_GetDesktopDisplayMode(0, &mode) == 0) want_w = mode.w, want_h = mode.h;
            else want_w = 1280, want_h = 800;
        }
        window_ = SDL_CreateWindow("KSP HMI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, want_w, want_h, flags);
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

std::unique_ptr<Source> make_source(const Config& cfg, const Args& args) {
    if (args.demo || cfg.source == "demo") return std::make_unique<DemoSource>(cfg.demo.speed, cfg.krpc.rate_hz);
    KrpcSource::Options o;
    o.address = cfg.krpc.address;
    o.rpc_port = cfg.krpc.rpc_port;
    o.stream_port = cfg.krpc.stream_port;
    o.rate_hz = cfg.krpc.rate_hz;
    o.retry_seconds = cfg.krpc.retry_seconds;
    o.discovery = cfg.krpc.discovery && args.address.empty();
    return std::make_unique<KrpcSource>(o);
}

/// --splash: before any settings, network or SDL video; --screenshot saves the same picture off-screen instead.
int splash(const Args& args) {
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
    if (!args.address.empty()) cfg.krpc.address = args.address;  // one place: the source and the waiting screen
    int want_w = cfg.display.width, want_h = cfg.display.height;
    if (!args.size.empty()) {
        size_t x = args.size.find_first_of("xX");
        if (x == std::string::npos) throw std::runtime_error("--size must look like 1920x1080");
        want_w = std::stoi(args.size.substr(0, x)), want_h = std::stoi(args.size.substr(x + 1));
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(0) != 0) throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());

    // Boot hand-over: the splash (ksphmi-splash.service) draws until this file exists; it is written after the
    // first presented frame. Only the real screen announces itself, never a window, a screenshot or a benchmark.
    const bool announce = !args.bench && args.screenshot.empty() && !args.offscreen && !args.windowed;
    if (announce) remove_ready_file();
    std::unique_ptr<Source> source;
    if (!args.bench) source = make_source(cfg, args);
    log::info(LOG, fmt::str("ksphmi %s (C++), source %s, settings %s", KSPHMI_VERSION,
                            source ? source->name() : "bench",
                            cfg.loaded.empty() ? "defaults" : cfg.loaded.front().c_str()));
    if (source) source->start();
    int code = 0;
    {
        Output out(cfg, args, want_w, want_h);
        Canvas canvas(out.renderer(), out.w(), out.h(), out.accelerated());
        Display display(cfg, canvas);
        log::info(LOG, fmt::str("screen %dx%d (%s)", out.w(), out.h(), out.kind().c_str()));
        {
            const std::string mode = args.layer_cache.empty() ? cfg.display.layer_cache : args.layer_cache;
            const bool want = mode == "on" || (mode != "off" && out.accelerated());
            const bool on = display.set_layer_cache(want);
            log::info(LOG, fmt::str("layer cache: %s (%s%s)", on ? "on" : "off", mode.c_str(),
                                    want && !on ? ", no render targets" : ""));
        }
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
            if (args.bench_anim) display.navball().hold = bench_attitude(0);
            display.draw(*bench_snap, monotonic());
            if (args.bench_anim) last = bench_snap;
            out.present();
        }
        for (int i = 0; !quit_signal; ++i) {
            double tick = monotonic();
            if (out.video()) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev))
                    if (ev.type == SDL_QUIT ||
                        (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)))
                        quit_signal = 1;
            }
            std::shared_ptr<const Snapshot> snap;
            if (!args.bench) snap = source->latest();
            else if (args.bench_anim) snap = bench_snap, display.navball().hold = bench_attitude(i + 1);
            else snap = std::make_shared<const Snapshot>(bench_source.sample(i + 1));
            // Redraw on new data; animated or ageing screens (waiting, stale counter) every tick. Between samples
            // only the NavBall turns: a NavBall-only frame over the cached layer when there is one.
            bool full = snap != last || snap->state != State::Flight || stale(*snap, tick);
            bool anim = !full && snap->state == State::Flight && display.navball().animating();
            if (full || anim) {
                double t0 = monotonic();
                if (!(anim && display.animate())) display.draw(*snap, t0), anim = false;
                double t1 = monotonic(), c1 = thread_cpu();
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
                            anim_frames, display.layer_cache() ? "on" : "off", out.w(), out.h(), out.kind().c_str());
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
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        log::error(LOG, e.what());
        return 1;
    }
}
