// Linux framebuffer output (/dev/fb0), as display.py's Framebuffer: geometry from sysfs, RGB565 or XRGB8888.
//
// Frames are drawn by SDL's software renderer into an ARGB8888 surface and written here row by row. A shadow copy
// of what the device shows skips unchanged rows, so a mostly static screen costs little more than the
// comparison. The device can be left blanked (powered down: /sys/class/graphics/fb0/blank = 4, seen after an SDL
// kmsdrm run), which hides every frame: it is unblanked at start and every 30 s (FBIOBLANK needs only write access).
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

    static constexpr double UNBLANK_EVERY = 30.0;

private:
    void unblank();
    std::string device_;
    int fd_ = -1, width_ = 0, height_ = 0, bpp_ = 0, stride_ = 0;
    uint8_t* map_ = nullptr;
    size_t map_size_ = 0;
    std::vector<uint8_t> shadow_, row_;
    bool first_ = true;
    double unblanked_at_ = -1e9;
};

}  // namespace ksphmi
