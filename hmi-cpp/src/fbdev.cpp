#include "fbdev.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "log.hpp"
#include "model.hpp"

namespace ksphmi {

namespace {
std::string attr(const std::string& sysfs, const char* name) {
    std::ifstream f(sysfs + "/" + name);
    std::string v;
    if (!std::getline(f, v)) throw std::runtime_error("cannot read " + sysfs + "/" + name);
    return v;
}
}  // namespace

Framebuffer::Framebuffer(const std::string& device) : device_(device) {
    std::string base = device.substr(device.find_last_of('/') + 1);
    std::string sysfs = "/sys/class/graphics/" + base;
    sysfs_ = sysfs;
    try {
        name_ = attr(sysfs, "name");
    } catch (const std::exception&) {
        name_.clear();
    }
    std::string size = attr(sysfs, "virtual_size");
    size_t comma = size.find(',');
    width_ = std::stoi(size.substr(0, comma));
    height_ = std::stoi(size.substr(comma + 1));
    bpp_ = std::stoi(attr(sysfs, "bits_per_pixel"));
    stride_ = std::stoi(attr(sysfs, "stride"));
    if (bpp_ != 16 && bpp_ != 32)
        throw std::runtime_error(device + ": unsupported " + std::to_string(bpp_) + " bits per pixel");
    fd_ = open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error(device + ": " + std::strerror(errno) + " (user in group video?)");
    struct stat st {};
    if (fstat(fd_, &st) == 0) inode_ = static_cast<unsigned long>(st.st_ino);
    map_size_ = static_cast<size_t>(stride_) * height_;
    void* m = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) {
        log::warning("ksphmi.display", device + ": mmap failed (" + std::strerror(errno) + "), using pwrite");
        map_ = nullptr;
    } else {
        map_ = static_cast<uint8_t*>(m);
    }
    size_t row = static_cast<size_t>(width_) * (bpp_ / 8);
    shadow_.assign(row * height_, 0);
    row_.resize(row);
    unblank();
}

Framebuffer::~Framebuffer() {
    if (map_) munmap(map_, map_size_);
    if (fd_ >= 0) close(fd_);
}

void Framebuffer::unblank() {
    if (ioctl(fd_, FBIOBLANK, FB_BLANK_UNBLANK) != 0)  // keep drawing; the log says why the screen may stay dark
        log::warning("ksphmi.display", std::string("cannot unblank the framebuffer: ") + std::strerror(errno));
    unblanked_at_ = monotonic();
    first_ = true;  // next write repaints every row
}

void Framebuffer::activate() {
    fb_var_screeninfo var{};
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var) != 0) {
        log::warning("ksphmi.display", std::string("cannot read the video mode: ") + std::strerror(errno));
        return;
    }
    var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
    if (ioctl(fd_, FBIOPUT_VSCREENINFO, &var) != 0)
        log::warning("ksphmi.display", std::string("cannot set the video mode: ") + std::strerror(errno));
}

bool Framebuffer::replaced() const {
    struct stat st {};
    if (stat(device_.c_str(), &st) != 0 || static_cast<unsigned long>(st.st_ino) != inode_) return true;
    std::ifstream f(sysfs_ + "/name");
    std::string now;
    return std::getline(f, now) && now != name_;
}

int Framebuffer::write(const SDL_Surface* frame) {
    if (monotonic() - unblanked_at_ > UNBLANK_EVERY) unblank();
    if (frame->w != width_ || frame->h != height_ || frame->format->format != SDL_PIXELFORMAT_ARGB8888)
        throw std::runtime_error("frame does not match the framebuffer");
    const size_t row_bytes = row_.size();
    int written = 0;
    for (int y = 0; y < height_; ++y) {
        const auto* src = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(frame->pixels) + y * frame->pitch);
        const uint8_t* out;
        if (bpp_ == 32) {
            out = reinterpret_cast<const uint8_t*>(src);  // XRGB8888 = ARGB8888 with the alpha ignored
        } else {
            auto* dst = reinterpret_cast<uint16_t*>(row_.data());
            for (int x = 0; x < width_; ++x) {
                uint32_t p = src[x];
                dst[x] = static_cast<uint16_t>(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
            }
            out = row_.data();
        }
        uint8_t* shadow = shadow_.data() + static_cast<size_t>(y) * row_bytes;
        if (!first_ && std::memcmp(shadow, out, row_bytes) == 0) continue;
        std::memcpy(shadow, out, row_bytes);
        if (map_) {
            std::memcpy(map_ + static_cast<size_t>(y) * stride_, out, row_bytes);
        } else if (pwrite(fd_, out, row_bytes, static_cast<off_t>(y) * stride_) < 0) {
            throw std::runtime_error(device_ + ": " + std::strerror(errno));
        }
        ++written;
    }
    first_ = false;
    return written;
}

}  // namespace ksphmi
#else
#include <stdexcept>

namespace ksphmi {
Framebuffer::Framebuffer(const std::string& device) : device_(device) {
    throw std::runtime_error("fbdev output requires Linux; use --windowed or --offscreen on this platform");
}
Framebuffer::~Framebuffer() = default;
int Framebuffer::write(const SDL_Surface*) {
    throw std::runtime_error("fbdev output requires Linux");
}
void Framebuffer::activate() {
    throw std::runtime_error("fbdev output requires Linux");
}
bool Framebuffer::replaced() const { return true; }
}  // namespace ksphmi
#endif
