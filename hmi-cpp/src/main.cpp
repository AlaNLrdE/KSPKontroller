// Entry point: ksphmi [--demo] [--windowed] [--address HOST] [--screenshot FILE] [--bench N] ...
// Settings: config.toml + config.local.toml + KSPHMI_* environment (config.hpp); see --help.
#include <SDL.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

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
    std::string config, address, size, screenshot, skin;
    bool demo = false, windowed = false, offscreen = false, splash = false;
    double seconds = 1.5;
    int fps = 0, bench = 0;
};

void usage(FILE* out) {
    std::fprintf(out,
                 "usage: ksphmi [--config FILE] [--demo] [--address HOST] [--windowed] [--size WxH]\n"
                 "              [--skin classic|apollo]\n"
                 "              [--screenshot FILE] [--seconds S] [--fps N] [--bench N] [--offscreen]\n"
                 "       ksphmi --splash [--screenshot FILE --size WxH]\n\n"
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
                 "  --offscreen        draw in memory only (never touches the screen; for --bench)\n"
                 "  --splash           draw the boot loading screen on $KSPHMI_FBDEV (default /dev/fb0) and exit;\n"
                 "                     reads no settings (ksphmi-splash.service, early boot)\n");
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
        else if (k == "--splash") a.splash = true;
        else throw std::runtime_error("unknown option " + k);
    }
    return true;
}

/// Where frames go: an SDL window (GPU renderer when available; kmsdrm on the Pi console), the Linux
/// framebuffer, or memory only. The last two use SDL's software renderer on an ARGB8888 surface.
class Output {
public:
    Output(const Config& cfg, const Args& args, int want_w, int want_h) {
        const char* env = std::getenv("KSPHMI_OUTPUT");
        std::string output = env && *env ? env : cfg.display.output;
        bool offscreen = args.offscreen || !args.screenshot.empty();
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
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) throw std::runtime_error(std::string("SDL video: ") + SDL_GetError());
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
        if (!window_) throw std::runtime_error(std::string("SDL window: ") + SDL_GetError());
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            log::warning(LOG, std::string("no GPU renderer (") + SDL_GetError() + "), using software");
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) throw std::runtime_error(std::string("SDL renderer: ") + SDL_GetError());
        SDL_RendererInfo info;
        SDL_GetRendererInfo(renderer_, &info);
        accelerated_ = (info.flags & SDL_RENDERER_ACCELERATED) != 0;
        SDL_GetRendererOutputSize(renderer_, &w_, &h_);
        if (fullscreen) SDL_ShowCursor(SDL_DISABLE);
        kind_ = std::string("SDL ") + SDL_GetCurrentVideoDriver() + " / " + info.name;
    }

    ~Output() {
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        if (surface_) SDL_FreeSurface(surface_);
        if (video_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
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
        {
            Canvas canvas(r, w, h, false);
            draw_splash(canvas);
        }
        code = save_png(s, args.screenshot) ? 0 : 1;
        SDL_DestroyRenderer(r);
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
        const int fps = std::max(1, args.fps ? args.fps : cfg.display.fps);
        const double frame = 1.0 / fps;
        const double deadline = monotonic() + args.seconds;
        std::shared_ptr<const Snapshot> last;
        double draw_ms = 0, present_ms = 0, perf_since = monotonic();
        int frames = 0;
        DemoSource bench_source;
        if (args.bench) {  // warm-up frame: fonts, label atlas and lighting are built once, outside the timing
            display.draw(bench_source.sample(0), monotonic());
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
            std::shared_ptr<const Snapshot> snap =
                args.bench ? std::make_shared<const Snapshot>(bench_source.sample(i + 1)) : source->latest();
            // Redraw on new data; animated or ageing screens (waiting, stale counter) every tick.
            bool needed = snap != last || snap->state != State::Flight || stale(*snap, tick);
            if (needed) {
                double t0 = monotonic();
                display.draw(*snap, t0);
                double t1 = monotonic();
                out.present();
                double t2 = monotonic();
                draw_ms += (t1 - t0) * 1000, present_ms += (t2 - t1) * 1000, ++frames;
                last = snap;
            }
            if (args.bench && frames >= args.bench) {
                std::printf("HMI_RENDER_MS=%.1f HMI_PRESENT_MS=%.1f frames=%d size=%dx%d output=%s\n", draw_ms / frames,
                            present_ms / frames, frames, out.w(), out.h(), out.kind().c_str());
                break;
            }
            if (!args.screenshot.empty() && tick >= deadline) {
                code = out.screenshot(args.screenshot) ? 0 : 1;
                if (!code) log::info(LOG, "saved " + args.screenshot);
                break;
            }
            if (!args.bench && tick - perf_since >= 60 && frames) {
                log::info(LOG, fmt::str("perf: %d frames in %.0f s, draw %.1f ms, present %.1f ms per frame", frames,
                                        tick - perf_since, draw_ms / frames, present_ms / frames));
                draw_ms = present_ms = 0, frames = 0, perf_since = tick;
            }
            if (!args.bench) {
                double left = tick + frame - monotonic();
                if (left > 0) std::this_thread::sleep_for(std::chrono::duration<double>(left));
            }
        }
    }
    if (source) source->stop();
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
