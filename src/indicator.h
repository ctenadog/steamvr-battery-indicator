// Draws the battery indicator texture (battery, charge bolt, percent text).
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "image.h"

namespace indicator {

constexpr int kTexW = 640;
constexpr int kTexH = 256;

const draw::Color kOk{78, 214, 120, 255};
const draw::Color kWarn{250, 190, 60, 255};
const draw::Color kLow{240, 74, 74, 255};
const draw::Color kNone{150, 150, 160, 255};
const draw::Color kBolt{255, 226, 84, 255};
const draw::Color kPanelFill{10, 12, 18, 165};
const draw::Color kPanelEdge{255, 255, 255, 38};
const draw::Color kBodyEdge{235, 240, 250, 230};
const draw::Color kText{255, 255, 255, 245};
const draw::Color kTextShadow{0, 0, 0, 170};

// Normalized outline of the lightning bolt.
const float kBoltShape[][2] = {
    {0.60f, 0.00f}, {0.14f, 0.58f}, {0.44f, 0.58f}, {0.32f, 1.00f},
    {0.88f, 0.40f}, {0.57f, 0.40f}, {0.76f, 0.00f},
};

struct BatteryState {
    bool available = false;
    bool hasPercentage = false;
    float percentage = 0.0f;  // 0..1
    bool charging = false;
};

inline draw::Color levelColor(const BatteryState &s) {
    if (!s.hasPercentage) return kNone;
    if (s.percentage >= 0.5f) return kOk;
    if (s.percentage >= 0.2f) return kWarn;
    return kLow;
}

inline std::vector<std::pair<float, float>> boltPoints(float x0, float y0, float x1, float y1,
                                                       float grow) {
    const float w = x1 - x0, h = y1 - y0;
    std::vector<std::pair<float, float>> pts;
    for (const auto &p : kBoltShape) {
        pts.emplace_back(x0 - grow + p[0] * (w + 2 * grow),
                         y0 - grow + p[1] * (h + 2 * grow));
    }
    return pts;
}

inline void drawBolt(draw::Image &img, bool charging, float t) {
    const float x0 = 424, y0 = 52, x1 = 604, y1 = 204;
    if (!charging) {
        const int scale = 8;
        const std::string dash = "-";
        const float tx = x0 + ((x1 - x0) - draw::Image::textWidth(dash, scale)) / 2;
        const float ty = y0 + ((y1 - y0) - 8 * scale) / 2;
        img.text(tx, ty, dash, scale, draw::Color{120, 126, 140, 190});
        return;
    }
    const float pulse = 0.5f + 0.5f * std::sin(t * 4.0f);
    for (int i = 6; i > 0; --i) {
        const float grow = i * 5.0f;
        const int alpha = static_cast<int>((28 - i * 3) * (0.45f + 0.55f * pulse));
        if (alpha <= 0) continue;
        img.fillPolygon(boltPoints(x0, y0, x1, y1, grow),
                        draw::Color{255, 226, 84, static_cast<uint8_t>(alpha)});
    }
    img.fillPolygon(boltPoints(x0, y0, x1, y1, 0.0f), kBolt);
}

inline void render(draw::Image &img, const BatteryState &state, const std::string &sourceLabel,
                   float t) {
    img.resize(kTexW, kTexH);
    img.fillRoundedRect(4, 4, kTexW - 4, kTexH - 4, 30, kPanelFill);

    const draw::Color color = levelColor(state);

    // Battery body and cap.
    img.fillRoundedRect(36, 60, 376, 196, 14, kBodyEdge, /*outline=*/true, 9, kBodyEdge);
    img.fillRoundedRect(376, 100, 400, 156, 6, kBodyEdge);

    // Charge level fill.
    if (state.hasPercentage) {
        const float innerX0 = 48, innerX1 = 364, innerY0 = 72, innerY1 = 184;
        float fillW = (innerX1 - innerX0) * state.percentage;
        if (fillW > 0) {
            fillW = std::max(fillW, 10.0f);
            img.fillRoundedRect(innerX0, innerY0, innerX0 + fillW, innerY1, 6, color);
        }
    }

    // Percent text, centered in the body.
    const std::string text = state.hasPercentage
        ? std::to_string(static_cast<int>(std::lround(state.percentage * 100))) + "%"
        : "--";
    const int scale = 8;
    const float tw = draw::Image::textWidth(text, scale);
    const float th = 8.0f * scale;
    const float tx = 36 + ((376 - 36) - tw) / 2;
    const float ty = 60 + ((196 - 60) - th) / 2;
    img.text(tx + 2, ty + 2, text, scale, kTextShadow);
    img.text(tx, ty, text, scale, kText);

    drawBolt(img, state.charging, t);

    // Source label in the bottom-left corner.
    img.text(30, kTexH - 40, sourceLabel, 2, draw::Color{200, 208, 224, 200});
}

// Converts the RGBA canvas to the BGRA byte order that SetOverlayRaw expects.
inline std::vector<uint8_t> toBgra(const draw::Image &img) {
    std::vector<uint8_t> out(img.pixels.size());
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        out[i + 0] = img.pixels[i + 2];
        out[i + 1] = img.pixels[i + 1];
        out[i + 2] = img.pixels[i + 0];
        out[i + 3] = img.pixels[i + 3];
    }
    return out;
}

}  // namespace indicator
