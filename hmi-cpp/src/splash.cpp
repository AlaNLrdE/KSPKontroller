#include "splash.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "fbdev.hpp"
#include "fmt.hpp"
#include "log.hpp"
#include "model.hpp"
#include "netinfo.hpp"
#include "theme.hpp"

namespace ksphmi {

namespace {
const char* LOG = "ksphmi.splash";

// Layout in units u (1 % of the height), as the approved 150 x 100 u mockup. Left-hand elements keep their
// position; the card, clock and footer are anchored to the real right edge, and the planet is centred in the
// space left of the card, so other aspect ratios keep the same composition.
constexpr double ORBIT = 25, SURFACE = 13, AIR = 16, CY = 49, CARD_W = 62, MARGIN = 4;
constexpr double CARD_TOP = 19, CARD_BOTTOM = 83, ROW0 = 27.5, ROW_STEP = 10.4, BAR_Y = 91.2, BAR_H = 1.2;

struct Layout {
    double u, w;
    double right, card_l, cx, cy;
    explicit Layout(const Canvas& c) : u(c.u), w(c.w) {
        right = w - MARGIN * u;
        card_l = right - CARD_W * u;
        cx = card_l * 44.0 / 84.0;  // 44 u at 3:2, where the card starts at 84 u
        cy = CY * u;
    }
    float x(double units) const { return static_cast<float>(units * u); }
    Rect box(double l, double t, double r, double b) const { return Rect::f(l, t * u, r - l, (b - t) * u); }
    /// Regions redrawn every frame (pixels); draw_dynamic must stay inside them (test_render checks).
    std::vector<Rect> dynamic() const {
        const double reach = (ORBIT + 2.5) * u;
        return {box(right - 48 * u, 0, w, 14.0),                                       // clock
                box(cx - reach, CY - ORBIT - 2.5, cx + reach, CY + ORBIT + 2.5),       // trajectory and ship
                box(u, 77.0, card_l - u, 90.7),                                         // percentage and message
                box(card_l + 0.6 * u, ROW0 - 0.4, right - 0.6 * u, CARD_BOTTOM - 0.6),  // rows
                box(0, BAR_Y - 0.4, w, BAR_Y + BAR_H + 0.4)};                           // segment bar
    }
};

Color mix(Color a, Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

/// Text shrunk (never grown) until it is at most `max_px` wide.
Rect fit_text(Canvas& c, const std::string& s, float size, Color col, double x, double y, Anchor a, double max_px) {
    while (size > 1.2f && c.text_width(c.font(size), s) > max_px) size *= 0.94f;
    return c.text(s, size, col, x, y, a);
}

double theta(double s) { return -PI / 2 - 0.35 + 2.1 * std::pow(s, 1.5); }
double radius(double s) { return SURFACE + (ORBIT - SURFACE) * (1 - std::pow(1 - s, 2.4)); }

void draw_static(Canvas& c, const Layout& L, const BootView& v) {
    using namespace theme;
    const double u = L.u;
    const float px = std::max(1.0f, L.x(0.18));
    c.clear(BG);
    // Header
    c.text("KSPKontroller", 4.6f, VALUE, MARGIN * u, 6.0 * u, Anchor::MidLeft);
    c.text("FLIGHT DISPLAY  \xC2\xB7  RASPBERRY PI", 2.5f, LABEL, MARGIN * u, 11.0 * u, Anchor::MidLeft);
    c.text("SINCE POWER-ON", 2.2f, LABEL, L.right, 4.3 * u, Anchor::MidRight);
    c.fill_rect(static_cast<float>(MARGIN * u), L.x(14.2), static_cast<float>(L.w - 2 * MARGIN * u), px, GRID);
    // Planet, atmosphere and the dashed target orbit
    const float cx = static_cast<float>(L.cx), cy = static_cast<float>(L.cy);
    c.circle(cx, cy, L.x(AIR), ATMOSPHERE);
    c.circle(cx, cy, L.x(SURFACE), BODY);
    c.text("BOOT", 2.0f, mix(BODY, VALUE, 0.55), cx, cy, Anchor::Center);
    const int dashes = 72;
    for (int i = 0; i < dashes; ++i) {
        double a = 2 * PI * i / dashes;
        c.arc(cx, cy, L.x(ORBIT + 0.2), a, a + 2 * PI / dashes * 0.55, std::max(1.0f, L.x(0.35)), GRID);
    }
    // BOOT SEQUENCE card: title, row labels and separators
    theme::panel(c, L.box(L.card_l, CARD_TOP, L.right, CARD_BOTTOM));
    const double pad = L.card_l + 3 * u;
    c.text("BOOT SEQUENCE", 2.5f, ACCENT, pad, 23.0 * u, Anchor::MidLeft);
    for (size_t i = 0; i < v.rows.size(); ++i) {
        const double top = (ROW0 + ROW_STEP * i) * u;
        c.fill_rect(static_cast<float>(pad), static_cast<float>(top), static_cast<float>(L.right - 3 * u - pad), px,
                    GRID);
        c.text(v.rows[i].label, 2.3f, LABEL, pad, top + 3.0 * u, Anchor::MidLeft);
    }
    // Footer
    const double half = (L.w - 2 * MARGIN * u) / 2 - 2 * u;
    fit_text(c, v.footer, 2.3f, MUTED, MARGIN * u, 96.2 * u, Anchor::MidLeft, half);
    fit_text(c, "Arduino panel works without this display", 2.3f, MUTED, L.right, 96.2 * u, Anchor::MidRight, half);
}

void draw_pill(Canvas& c, const Layout& L, Step step, double pulse, double right, double cy) {
    using namespace theme;
    const char* text = "--";
    Color col = MUTED;
    switch (step) {
        case Step::Done: text = "OK", col = GOOD; break;
        case Step::Active: text = "\xC2\xB7\xC2\xB7\xC2\xB7", col = mix(GRID, ACCENT, 0.35 + 0.65 * pulse); break;
        case Step::Pending: break;
        case Step::Warn: text = "OFF", col = WARN; break;
        case Step::Failed: text = "FAIL", col = BAD; break;
    }
    const double u = L.u, h = 3.6 * u;
    const float size = step == Step::Active || step == Step::Pending ? 2.8f : 2.0f;  // symbols read small
    Font& f = c.font(size);
    const double w = std::max(8.0 * u, c.text_width(f, text) + 2.6 * u);
    const Rect outer = Rect::f(right - w, cy - h / 2, w, h);
    const float stroke = std::max(1.0f, L.x(0.22));
    c.rounded_rect(outer, static_cast<float>(h / 2), col);
    c.rounded_rect(outer.inflate(-2 * stroke, -2 * stroke), static_cast<float>(h / 2 - stroke), PANEL);
    c.text(text, size, col, outer.centerx(), outer.centery(), Anchor::Center);
}

void draw_dynamic(Canvas& c, const Layout& L, const BootView& v, const SplashAnim& a) {
    using namespace theme;
    const double u = L.u;
    c.text(v.clock, 4.2f, VALUE, L.right, 9.6 * u, Anchor::MidRight);
    // Ascent trajectory from the surface to the target orbit; the ship keeps circling once it is there.
    const float cx = static_cast<float>(L.cx), cy = static_cast<float>(L.cy);
    const double shown = std::clamp(a.shown, 0.0, 1.0);
    const float wide = std::max(1.5f, L.x(0.5));
    if (v.done && shown >= 0.995) c.ring(cx, cy, L.x(ORBIT + 0.25), std::max(1.0f, L.x(0.45)), ACCENT);
    const int n = std::max(2, static_cast<int>(std::ceil(shown * 90)));
    std::vector<Pt> path;
    for (int i = 0; i <= n; ++i) {
        double s = shown * i / n, r = radius(s) * u, t = theta(s);
        path.push_back({cx + static_cast<float>(r * std::cos(t)), cy + static_cast<float>(r * std::sin(t))});
    }
    c.polyline(path, false, wide, ACCENT);
    for (const Pt& p : path) c.circle(p.x, p.y, wide / 2, ACCENT);  // round joints
    Pt ship = path.back();
    if (shown >= 0.995) {
        double t = theta(1) + a.orbit;
        ship = {cx + L.x(ORBIT) * static_cast<float>(std::cos(t)), cy + L.x(ORBIT) * static_cast<float>(std::sin(t))};
    }
    c.circle(ship.x, ship.y, L.x(1.2), ACCENT);
    c.circle(ship.x, ship.y, L.x(0.5), VALUE);
    // Percentage and message
    const double room = 2 * std::min(L.cx - 2 * u, L.card_l - 2 * u - L.cx);
    std::string percent = v.failed ? "HMI FAILED" : fmt::str("%d%%", static_cast<int>(std::lround(shown * 100)));
    fit_text(c, percent, 7.5f, v.failed ? BAD : VALUE, L.cx, 77.6 * u, Anchor::MidTop, room);
    fit_text(c, v.message, 2.7f, v.failed ? BAD : LABEL, L.cx, 86.7 * u, Anchor::MidTop, room);
    // Rows: detail and status pill
    const double pad = L.card_l + 3 * u, pill_r = L.right - 3 * u;
    for (size_t i = 0; i < v.rows.size(); ++i) {
        const BootRow& r = v.rows[i];
        const double top = (ROW0 + ROW_STEP * i) * u;
        Color col = r.step == Step::Pending ? MUTED : r.step == Step::Failed ? BAD : r.step == Step::Warn ? WARN : VALUE;
        fit_text(c, r.detail, 3.1f, col, pad, top + 7.0 * u, Anchor::MidLeft, pill_r - 10.5 * u - pad);
        draw_pill(c, L, r.step, a.pulse, pill_r, top + 5.2 * u);
    }
    // Five-segment bar, one per row
    const double gap = 0.8 * u, x0 = MARGIN * u, span = L.w - 2 * MARGIN * u;
    const double seg = (span - gap * (v.rows.size() - 1)) / v.rows.size();
    for (size_t i = 0; i < v.rows.size(); ++i) {
        Step s = v.rows[i].step;
        Color col = s == Step::Done ? ACCENT : s == Step::Active ? GRID : s == Step::Failed ? BAD
                  : s == Step::Warn ? WARN : PANEL;
        c.rounded_rect(Rect::f(x0 + i * (seg + gap), BAR_Y * u, seg, BAR_H * u), L.x(BAR_H / 2), col);
    }
}

std::string static_key(const BootView& v) { return v.footer + "|" + v.rows[1].detail; }

volatile std::sig_atomic_t stop_signal = 0;
void on_stop(int) { stop_signal = 1; }

/// Probe results, refreshed at 2 Hz on a detached worker (systemctl may take a while early in the boot).
struct Probes {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool system_up = false, hmi_running = false;
    NetInterface net;
    std::string hostname;
};

std::shared_ptr<Probes> start_probes() {
    auto p = std::make_shared<Probes>();
    p->hostname = host_name();
    std::thread([p] {
        bool up = false;
        for (;;) {
            if (!up) up = unit_active("multi-user.target");  // never goes back during a boot
            NetInterface net = primary_interface();
            bool hmi = hmi_process_running();
            std::string host = host_name();
            std::unique_lock<std::mutex> lock(p->mutex);
            if (p->stopping) return;
            p->system_up = up, p->net = net, p->hmi_running = hmi;
            if (!host.empty()) p->hostname = host;
            if (p->wake.wait_for(lock, std::chrono::milliseconds(500), [&] { return p->stopping; })) return;
        }
    }).detach();
    return p;
}
}  // namespace

void draw_splash(Canvas& c, const BootView& view, const SplashAnim& anim) {
    Layout L(c);
    draw_static(c, L, view);
    draw_dynamic(c, L, view, anim);
    c.flush();
}

SplashPainter::SplashPainter(SDL_Surface* target) : target_(target) {
    renderer_ = SDL_CreateSoftwareRenderer(target_);
    if (!renderer_) throw std::runtime_error(std::string("software renderer: ") + SDL_GetError());
    canvas_ = std::make_unique<Canvas>(renderer_, target_->w, target_->h, false);
}

SplashPainter::~SplashPainter() {
    canvas_.reset();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (static_) SDL_FreeSurface(static_);
}

void SplashPainter::draw(const BootView& view, const SplashAnim& anim) {
    Layout L(*canvas_);
    const std::string key = static_key(view);
    if (!static_ || key != static_key_) {
        draw_static(*canvas_, L, view);
        canvas_->flush();
        SDL_RenderFlush(renderer_);
        if (!static_) {
            static_ = SDL_CreateRGBSurfaceWithFormat(0, target_->w, target_->h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (!static_) throw std::runtime_error(std::string("SDL surface: ") + SDL_GetError());
            SDL_SetSurfaceBlendMode(static_, SDL_BLENDMODE_NONE);
            SDL_SetSurfaceBlendMode(target_, SDL_BLENDMODE_NONE);
        }
        SDL_BlitSurface(target_, nullptr, static_, nullptr);
        static_key_ = key;
    } else {
        for (Rect r : L.dynamic()) {
            SDL_Rect src{r.x, r.y, r.w, r.h}, dst = src;
            SDL_BlitSurface(static_, &src, target_, &dst);
        }
    }
    draw_dynamic(*canvas_, L, view, anim);
    canvas_->flush();
    SDL_RenderFlush(renderer_);
}

int run_splash(const std::string& device, double max_seconds) {
    std::signal(SIGTERM, on_stop);
    std::signal(SIGINT, on_stop);
    const std::string ready = ready_file_path();
    if (file_exists(ready)) {
        log::info(LOG, "the HMI is already drawing (" + ready + "): no splash");
        return 0;
    }
    auto probes = start_probes();
    std::unique_ptr<Framebuffer> fb;
    SDL_Surface* surface = nullptr;
    std::unique_ptr<SplashPainter> painter;
    auto close_fb = [&] {
        painter.reset();
        if (surface) SDL_FreeSurface(surface), surface = nullptr;
        fb.reset();
    };
    const double start = monotonic(), frame = 1.0 / 8;
    double last = start, checked = start, shown = 0, orbit = 0, draw_ms = 0, write_ms = 0;
    int frames = 0, opened = 0, code = 0;
    bool plain = false, warned_open = false, failed_logged = false;
    std::string why = "time limit";
    std::string net_logged = "?";
    while (!stop_signal) {
        const double now = monotonic();
        if (now - start > max_seconds) break;
        if (file_exists(ready)) {
            why = "HMI ready";
            break;
        }
        if (fb && now - checked >= 0.5) {
            checked = now;
            if (fb->replaced()) {
                log::info(LOG, device + " was replaced (" + fb->name() + " gone), reopening");
                close_fb();
            }
        }
        if (!fb) {
            try {
                fb = std::make_unique<Framebuffer>(device);
                fb->activate();  // nothing may have set the display mode yet when the console stays silent
                surface = SDL_CreateRGBSurfaceWithFormat(0, fb->width(), fb->height(), 32, SDL_PIXELFORMAT_ARGB8888);
                if (!surface) throw std::runtime_error(std::string("SDL surface: ") + SDL_GetError());
                ++opened, checked = now, plain = false;
                log::info(LOG, fmt::str("splash on %s (%s, %dx%d, %d bpp) %.1f s after power-on", device.c_str(),
                                        fb->name().c_str(), fb->width(), fb->height(), fb->bits_per_pixel(),
                                        boot_seconds()));
                try {
                    painter = std::make_unique<SplashPainter>(surface);
                } catch (const std::exception& e) {
                    log::warning(LOG, std::string("splash without text: ") + e.what());
                    plain = true;
                }
            } catch (const std::exception& e) {
                if (!warned_open) log::warning(LOG, std::string(e.what()) + "; retrying");
                warned_open = true;
                close_fb();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }
        BootInputs in;
        {
            std::lock_guard<std::mutex> lock(probes->mutex);
            in.system_up = probes->system_up, in.hmi_running = probes->hmi_running;
            in.iface = probes->net.name, in.address = probes->net.address, in.hostname = probes->hostname;
        }
        in.seconds = boot_seconds(), in.width = fb->width(), in.height = fb->height(), in.device = device;
        const BootView view = boot_view(in);
        const std::string net = in.iface + " " + in.address;
        if (net != net_logged) log::info(LOG, "network: " + (in.address.empty() ? in.iface + " no address" : net));
        net_logged = net;
        if (view.failed && !failed_logged) log::warning(LOG, "the HMI has not drawn a frame yet: " + view.message);
        failed_logged = view.failed;
        const double dt = now - last;
        last = now;
        shown += (view.progress - shown) * (1 - std::exp(-dt * 1.8));
        if (std::fabs(view.progress - shown) < 0.002) shown = view.progress;
        if (view.done && shown >= 0.995) orbit += dt * 0.5;
        SplashAnim anim{shown, orbit, 0.5 + 0.5 * std::sin(now * 2 * PI * 0.8)};
        const double t0 = monotonic();
        if (plain) {
            SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, theme::BG.r, theme::BG.g, theme::BG.b));
        } else {
            try {
                painter->draw(view, anim);
            } catch (const std::exception& e) {
                log::warning(LOG, std::string("splash without text: ") + e.what());
                plain = true;
                continue;
            }
        }
        const double t1 = monotonic();
        // Checked again right before the write: the HMI's first frame is never painted over.
        if (file_exists(ready)) {
            why = "HMI ready";
            break;
        }
        try {
            fb->write(surface);
        } catch (const std::exception& e) {
            log::warning(LOG, e.what());
            close_fb();
            continue;
        }
        const double t2 = monotonic();
        draw_ms += (t1 - t0) * 1000, write_ms += (t2 - t1) * 1000, ++frames;
        const double left = now + frame - monotonic();
        if (left > 0) std::this_thread::sleep_for(std::chrono::duration<double>(left));
    }
    if (stop_signal) why = "stopped";
    {
        std::lock_guard<std::mutex> lock(probes->mutex);
        probes->stopping = true;
        probes->wake.notify_all();
    }
    if (!opened) code = 1, log::error(LOG, "could not open " + device);
    log::info(LOG, fmt::str("splash done (%s) %.1f s after power-on: %d frames, draw %.1f ms, write %.1f ms per frame",
                            why.c_str(), boot_seconds(), frames, frames ? draw_ms / frames : 0.0,
                            frames ? write_ms / frames : 0.0));
    close_fb();
    return code;
}

}  // namespace ksphmi
