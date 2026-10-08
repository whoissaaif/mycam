#include "frame_transform.h"

#include <string.h>

#include <algorithm>
#include <vector>

namespace mycam {

namespace {

// Maps pixel (u, v) of the rotated image back to (sx, sy) in the original w x h image.
inline void Inverse(uint32_t rot, int u, int v, int w, int h, int* sx, int* sy) {
    switch (rot) {
    case 90:  *sx = v;         *sy = h - 1 - u; break;
    case 180: *sx = w - 1 - u; *sy = h - 1 - v; break;
    case 270: *sx = w - 1 - v; *sy = u;         break;
    default:  *sx = u;         *sy = v;         break;
    }
}

} // namespace

void FillBlackNV12(uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height) {
    for (uint32_t y = 0; y < height; ++y) memset(dst + y * pitch, 16, width);
    uint8_t* uv = dst + pitch * height;
    for (uint32_t y = 0; y < height / 2; ++y) memset(uv + y * pitch, 128, width);
}

void DrawFittedNV12(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, uint32_t rotation, bool mirror,
                    uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height) {
    if (rotation != 90 && rotation != 180 && rotation != 270) rotation = 0;
    const int w = int(srcWidth), h = int(srcHeight);
    const bool swap = rotation == 90 || rotation == 270;
    const int ew = swap ? h : w, eh = swap ? w : h; // Size after rotation.

    // Fit (letterbox) the rotated image into the output, keeping even offsets/sizes for NV12.
    double scale = std::min(double(width) / ew, double(height) / eh);
    int dw = std::max(2, int(ew * scale) & ~1), dh = std::max(2, int(eh * scale) & ~1);
    int x0 = ((int(width) - dw) / 2) & ~1, y0 = ((int(height) - dh) / 2) & ~1;

    // Luma: precompute rotated-image coordinates for each output column/row.
    std::vector<int> xmap(dw), ymap(dh);
    for (int x = 0; x < dw; ++x) {
        int u = std::min(ew - 1, int(x * double(ew) / dw));
        xmap[x] = mirror ? ew - 1 - u : u;
    }
    for (int y = 0; y < dh; ++y) ymap[y] = std::min(eh - 1, int(y * double(eh) / dh));

    for (int y = 0; y < dh; ++y) {
        uint8_t* row = dst + (y0 + y) * pitch + x0;
        int v = ymap[y];
        for (int x = 0; x < dw; ++x) {
            int sx, sy;
            Inverse(rotation, xmap[x], v, w, h, &sx, &sy);
            row[x] = src[sy * w + sx];
        }
    }

    // Chroma at half resolution, in UV-sample units of the source (w/2 x h/2).
    const uint8_t* srcUV = src + size_t(w) * h;
    const int cw = w / 2, ch = h / 2, cew = ew / 2, ceh = eh / 2;
    uint8_t* dstUV = dst + pitch * height;
    for (int y = 0; y < dh / 2; ++y) {
        uint8_t* row = dstUV + (y0 / 2 + y) * pitch + x0;
        int v = std::min(ceh - 1, int(y * double(ceh) / (dh / 2)));
        for (int x = 0; x < dw / 2; ++x) {
            int u = std::min(cew - 1, int(x * double(cew) / (dw / 2)));
            if (mirror) u = cew - 1 - u;
            int sx, sy;
            Inverse(rotation, u, v, cw, ch, &sx, &sy);
            const uint8_t* s = srcUV + sy * w + sx * 2;
            row[x * 2] = s[0];
            row[x * 2 + 1] = s[1];
        }
    }
}

} // namespace mycam
