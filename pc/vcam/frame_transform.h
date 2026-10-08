#pragma once
// Pure NV12 image operations used by the virtual camera. No Windows dependencies, so they can be
// unit-tested (pc/tests).

#include <stddef.h>
#include <stdint.h>

namespace mycam {

// Fills an NV12 image with black (Y=16, UV=128). The UV plane starts at dst + pitch * height.
void FillBlackNV12(uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height);

// Draws a tightly packed NV12 source (srcWidth x srcHeight, both even) into the destination:
// rotates it clockwise by `rotation` (0/90/180/270), optionally mirrors it horizontally, then scales
// it to fit and centres it (letterbox). Pixels outside the fitted area are left untouched, so call
// FillBlackNV12 first. Nearest-neighbour sampling.
void DrawFittedNV12(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, uint32_t rotation, bool mirror,
                    uint8_t* dst, ptrdiff_t pitch, uint32_t width, uint32_t height);

} // namespace mycam
