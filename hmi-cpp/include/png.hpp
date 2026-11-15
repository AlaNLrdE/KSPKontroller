// Minimal PNG writer for --screenshot (stored deflate blocks, no zlib dependency).
#pragma once

#include <SDL.h>

#include <string>

namespace ksphmi {

/// Save an ARGB8888 surface as an RGB PNG. Returns false and logs on failure.
bool save_png(const SDL_Surface* surface, const std::string& path);

}  // namespace ksphmi
