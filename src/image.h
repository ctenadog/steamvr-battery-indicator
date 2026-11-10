// Small CPU raster helper: an RGBA canvas with a few drawing primitives and a
// dependency-free PNG writer (used for the --preview mode).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "font8x8.h"

namespace draw {

struct Color {
    uint8_t r, g, b, a;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // RGBA, row-major, non-premultiplied

    Image() = default;
    Image(int w, int h) { resize(w, h); }

    void resize(int w, int h) {
        width = w;
        height = h;
        pixels.assign(static_cast<size_t>(w) * h * 4, 0);
    }

    // Source-over alpha blend of a single pixel.
    void blend(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= width || y >= height || c.a == 0) return;
        uint8_t *p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
        if (c.a == 255) {
            p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
            return;
        }
        const float sa = c.a / 255.0f;
        const float da = p[3] / 255.0f;
        const float outA = sa + da * (1.0f - sa);
        if (outA <= 0.0f) {
            p[0] = p[1] = p[2] = p[3] = 0;
            return;
        }
        p[0] = static_cast<uint8_t>((c.r * sa + p[0] * da * (1.0f - sa)) / outA);
        p[1] = static_cast<uint8_t>((c.g * sa + p[1] * da * (1.0f - sa)) / outA);
        p[2] = static_cast<uint8_t>((c.b * sa + p[2] * da * (1.0f - sa)) / outA);
        p[3] = static_cast<uint8_t>(outA * 255.0f + 0.5f);
    }

    void fillRoundedRect(float x0, float y0, float x1, float y1, float radius, Color fill,
                         bool outline = false, float thickness = 0.0f, Color stroke = {}) {
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        radius = std::min(radius, std::min(x1 - x0, y1 - y0) / 2.0f);
        const int ix0 = static_cast<int>(std::floor(x0 - 1));
        const int iy0 = static_cast<int>(std::floor(y0 - 1));
        const int ix1 = static_cast<int>(std::ceil(x1 + 1));
        const int iy1 = static_cast<int>(std::ceil(y1 + 1));
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                const float cx = x + 0.5f, cy = y + 0.5f;
                const float d = roundedRectDistance(cx, cy, x0, y0, x1, y1, radius);
                if (outline) {
                    const float w = std::max(thickness, 0.5f);
                    // Draw only near the boundary, not the whole interior.
                    if (d > w * 0.5f || d < -w * 0.5f) continue;
                    blend(x, y, stroke.a ? stroke : fill);
                } else if (d <= 0.0f) {
                    blend(x, y, fill);
                }
            }
        }
    }

    void fillPolygon(const std::vector<std::pair<float, float>> &pts, Color c) {
        if (pts.size() < 3) return;
        float minX = pts[0].first, maxX = minX, minY = pts[0].second, maxY = minY;
        for (auto &p : pts) {
            minX = std::min(minX, p.first);
            maxX = std::max(maxX, p.first);
            minY = std::min(minY, p.second);
            maxY = std::max(maxY, p.second);
        }
        const int ix0 = static_cast<int>(std::floor(minX));
        const int ix1 = static_cast<int>(std::ceil(maxX));
        const int iy0 = static_cast<int>(std::floor(minY));
        const int iy1 = static_cast<int>(std::ceil(maxY));
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                if (pointInPolygon(x + 0.5f, y + 0.5f, pts)) blend(x, y, c);
            }
        }
    }

    // Draws text using the 8x8 bitmap font, scaled by an integer factor.
    void text(float x, float y, const std::string &s, int scale, Color c) {
        float cx = x;
        for (char ch : s) {
            unsigned char uc = static_cast<unsigned char>(ch);
            if (uc >= 128) uc = '?';
            const uint8_t *glyph = kFont8x8[uc];
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    if (glyph[gy] & (1u << gx)) {
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx)
                                blend(static_cast<int>(cx) + gx * scale + sx,
                                      static_cast<int>(y) + gy * scale + sy, c);
                    }
                }
            }
            cx += 8 * scale;
        }
    }

    static float textWidth(const std::string &s, int scale) {
        return static_cast<float>(s.size()) * 8 * scale;
    }

    bool savePng(const std::string &path) const {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (width * 4 + 1));
        for (int y = 0; y < height; ++y) {
            raw.push_back(0);  // filter type: none
            const uint8_t *row = &pixels[static_cast<size_t>(y) * width * 4];
            raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
        }
        std::vector<uint8_t> out;
        const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        out.insert(out.end(), sig, sig + 8);
        appendChunk(out, "IHDR", ihdr());
        appendChunk(out, "IDAT", zlibStore(raw));
        appendChunk(out, "IEND", {});
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        return true;
    }

private:
    static float roundedRectDistance(float px, float py, float x0, float y0,
                                     float x1, float y1, float radius) {
        const float cx = (x0 + x1) * 0.5f;
        const float cy = (y0 + y1) * 0.5f;
        const float hx = (x1 - x0) * 0.5f - radius;
        const float hy = (y1 - y0) * 0.5f - radius;
        const float dx = std::fabs(px - cx) - hx;
        const float dy = std::fabs(py - cy) - hy;
        const float outside = std::sqrt(std::max(dx, 0.0f) * std::max(dx, 0.0f) +
                                        std::max(dy, 0.0f) * std::max(dy, 0.0f));
        const float inside = std::min(std::max(dx, dy), 0.0f);
        return outside + inside - radius;
    }

    static bool pointInPolygon(float px, float py,
                               const std::vector<std::pair<float, float>> &pts) {
        bool inside = false;
        for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
            const float xi = pts[i].first, yi = pts[i].second;
            const float xj = pts[j].first, yj = pts[j].second;
            if (((yi > py) != (yj > py)) &&
                (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
                inside = !inside;
            }
        }
        return inside;
    }

    std::vector<uint8_t> ihdr() const {
        std::vector<uint8_t> d(13);
        auto put32 = [&](int off, uint32_t v) {
            d[off] = v >> 24; d[off + 1] = v >> 16; d[off + 2] = v >> 8; d[off + 3] = v;
        };
        put32(0, width);
        put32(4, height);
        d[8] = 8;   // bit depth
        d[9] = 6;   // color type: RGBA
        d[10] = 0; d[11] = 0; d[12] = 0;
        return d;
    }

    static void appendChunk(std::vector<uint8_t> &out, const char *type,
                            const std::vector<uint8_t> &data) {
        auto put32 = [&](uint32_t v) {
            out.push_back(v >> 24); out.push_back(v >> 16);
            out.push_back(v >> 8); out.push_back(v);
        };
        put32(static_cast<uint32_t>(data.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        put32(crc32(&out[start], 4 + data.size()));
    }

    static std::vector<uint8_t> zlibStore(const std::vector<uint8_t> &data) {
        std::vector<uint8_t> z;
        z.push_back(0x78);  // CMF: deflate, 32K window
        z.push_back(0x01);  // FLG
        size_t pos = 0;
        while (pos < data.size()) {
            const size_t len = std::min<size_t>(65535, data.size() - pos);
            const bool last = (pos + len == data.size());
            z.push_back(last ? 1 : 0);
            z.push_back(len & 0xFF);
            z.push_back((len >> 8) & 0xFF);
            z.push_back(~len & 0xFF);
            z.push_back((~len >> 8) & 0xFF);
            z.insert(z.end(), data.begin() + pos, data.begin() + pos + len);
            pos += len;
        }
        uint32_t a = 1, b = 0;
        for (uint8_t byte : data) {
            a = (a + byte) % 65521;
            b = (b + a) % 65521;
        }
        const uint32_t adler = (b << 16) | a;
        z.push_back(adler >> 24); z.push_back(adler >> 16);
        z.push_back(adler >> 8); z.push_back(adler);
        return z;
    }

    static uint32_t crc32(const uint8_t *data, size_t len) {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready) {
            for (uint32_t n = 0; n < 256; ++n) {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[n] = c;
            }
            ready = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
};

}  // namespace draw
