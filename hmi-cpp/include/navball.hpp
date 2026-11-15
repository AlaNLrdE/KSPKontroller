// Stock-inspired NavBall (port of navball.py): a quaternion-rotated sphere drawn as triangles.
//
// Static work is cached: the volume lighting is one alpha texture per size, the degree / bearing labels are
// pre-rendered once into a sprite atlas and drawn as foreshortened, rotated quads (continuous foreshortening and
// limb fading, never screen-space collision tests). Per frame only vertices are computed.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "model.hpp"

namespace ksphmi {

class Navball {
public:
    static constexpr double MARKER_SIZE = 2.2;  // screen-height units; twice the original marker radius

    Navball();
    ~Navball();
    Navball(const Navball&) = delete;
    Navball& operator=(const Navball&) = delete;

    void draw(Canvas& c, const Rect& rect, const std::optional<Navigation>& nav);

    /// Marker glyphs (prograde, retrograde, normal, anti_normal, radial, anti_radial, target, anti_target, maneuver).
    static void symbol(Canvas& c, const std::string& kind, Color color, float x, float y, float size);

private:
    struct Cell {
        Vec3 v[4];
        Color color;
    };
    struct Line {
        std::vector<Vec3> points;
        int width;
    };
    struct Label {
        std::string text;
        double pitch, heading;
        bool is_heading;
    };
    struct Sprite {
        SDL_Rect src;
    };

    void build_labels(Canvas& c);
    void build_lighting(Canvas& c, int size, double radius);

    std::vector<Cell> cells_;
    std::vector<Line> lines_;
    std::vector<Label> labels_;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* label_atlas_ = nullptr;
    int label_px_ = 0, atlas_w_ = 0, atlas_h_ = 0;
    std::map<std::string, Sprite> sprites_;  // text + style + background
    SDL_Texture* lighting_ = nullptr;
    int lighting_size_ = 0;
    double lighting_radius_ = 0;
};

}  // namespace ksphmi
