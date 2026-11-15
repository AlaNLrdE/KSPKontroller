// Linux framebuffer output (/dev/fb0), as display.py's Framebuffer: geometry from sysfs, RGB565 or XRGB8888.
//
// Frames are drawn by SDL's software renderer into an ARGB8888 surface and written here row by row. A shadow copy
// of what the device shows skips unchanged rows, so a mostly static screen costs little more than the
// comparison. The device can be left blanked (powered down: /sys/class/graphics/fb0/blank = 4, seen after an SDL
// kmsdrm run), which hides every frame: it is unblanked at start and every 30 s (FBIOBLANK needs only write access).
// Each unblank also rewrites every row once, so pixels somebody else left (the boot splash, console text) never
// survive behind rows the shadow copy believes unchanged.
#pragma once

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ksphmi {

class Framebuffer {
public:
    explicit Framebuffer(const std::string& device);
    ~Framebuffer();
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    int bits_per_pixel() const { return bpp_; }
    /// Copy an ARGB8888 surface of the device's size; returns the rows actually written.
    int write(const SDL_Surface* frame);
    /// Re-apply the current video mode (FBIOPUT_VSCREENINFO, FB_ACTIVATE_FORCE): with DRM's fbdev emulation the
    /// mode is only set when somebody calls set_par, normally the text console; the splash must not rely on it.
    void activate();
    /// The next write rewrites every row (somebody else may have drawn on the device meanwhile).
    void repaint_all() { first_ = true; }
    /// Driver name from sysfs ("vc4drmfb", "simple"); empty when unknown.
    const std::string& name() const { return name_; }
    /// True when the device node is gone or is no longer the one opened (another driver took fb0 over, as
    /// vc4drmfb replaces the firmware's simplefb a few seconds into the Pi's boot): reopen it.
    bool replaced() const;

    static constexpr double UNBLANK_EVERY = 30.0;

private:
    void unblank();
    std::string device_, sysfs_, name_;
    unsigned long inode_ = 0;
    int fd_ = -1, width_ = 0, height_ = 0, bpp_ = 0, stride_ = 0;
    uint8_t* map_ = nullptr;
    size_t map_size_ = 0;
    std::vector<uint8_t> shadow_, row_;
    bool first_ = true;
    double unblanked_at_ = -1e9;
};

}  // namespace ksphmi
