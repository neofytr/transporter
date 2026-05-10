// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU software rasterizer for Dear ImGui draw data.
// Output format: WL_SHM_FORMAT_XRGB8888 (LE bytes: B G R X).

#include "platform_internal.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace transporter::gui::platform {

namespace {

// ImGui ImU32: LE bytes R G B A  →  XRGB8888 LE bytes B G R X
static inline uint32_t to_xrgb(ImU32 c) {
    const uint8_t r = c & 0xFFu;
    const uint8_t g = (c >>  8) & 0xFFu;
    const uint8_t b = (c >> 16) & 0xFFu;
    return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}

// Porter-Duff src-over into an XRGB pixel buffer.
static inline uint32_t blend_over(uint32_t dst, uint32_t src, uint32_t alpha) {
    if (alpha == 0u) return dst;
    if (alpha >= 255u) return src;
    const uint32_t inv = 255u - alpha;
    const uint8_t r = static_cast<uint8_t>(
        ((src >> 16 & 0xFFu) * alpha + (dst >> 16 & 0xFFu) * inv) / 255u);
    const uint8_t g = static_cast<uint8_t>(
        ((src >>  8 & 0xFFu) * alpha + (dst >>  8 & 0xFFu) * inv) / 255u);
    const uint8_t b = static_cast<uint8_t>(
        ((src       & 0xFFu) * alpha + (dst       & 0xFFu) * inv) / 255u);
    return (0xFFu << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}

void rasterize_tri(uint32_t* __restrict__ pixels, int pstride, int fw, int fh,
                   const ImDrawVert& va, const ImDrawVert& vb, const ImDrawVert& vc,
                   const ImVec4& clip,
                   const uint8_t* atlas, int aw, int ah)
{
    // Clip rect intersection
    const int cx0 = std::max(0, static_cast<int>(clip.x));
    const int cy0 = std::max(0, static_cast<int>(clip.y));
    const int cx1 = std::min(fw, static_cast<int>(clip.z));
    const int cy1 = std::min(fh, static_cast<int>(clip.w));
    if (cx0 >= cx1 || cy0 >= cy1) return;

    // Triangle AABB clamped to clip rect
    const int x0 = std::max(cx0, static_cast<int>(std::min({va.pos.x, vb.pos.x, vc.pos.x})));
    const int y0 = std::max(cy0, static_cast<int>(std::min({va.pos.y, vb.pos.y, vc.pos.y})));
    const int x1 = std::min(cx1, 1 + static_cast<int>(std::max({va.pos.x, vb.pos.x, vc.pos.x})));
    const int y1 = std::min(cy1, 1 + static_cast<int>(std::max({va.pos.y, vb.pos.y, vc.pos.y})));
    if (x0 >= x1 || y0 >= y1) return;

    // Pre-compute for barycentric interpolation
    const float denom = (vb.pos.y - vc.pos.y) * (va.pos.x - vc.pos.x) +
                        (vc.pos.x - vb.pos.x) * (va.pos.y - vc.pos.y);
    if (std::abs(denom) < 0.5f) return;
    const float inv = 1.0f / denom;

    for (int py = y0; py < y1; ++py) {
        const float fy = static_cast<float>(py) + 0.5f;
        for (int px = x0; px < x1; ++px) {
            const float fx = static_cast<float>(px) + 0.5f;
            const float w0 = ((vb.pos.y - vc.pos.y) * (fx - vc.pos.x) +
                               (vc.pos.x - vb.pos.x) * (fy - vc.pos.y)) * inv;
            const float w1 = ((vc.pos.y - va.pos.y) * (fx - vc.pos.x) +
                               (va.pos.x - vc.pos.x) * (fy - vc.pos.y)) * inv;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            // Interpolate UV
            const float u = w0 * va.uv.x + w1 * vb.uv.x + w2 * vc.uv.x;
            const float v = w0 * va.uv.y + w1 * vb.uv.y + w2 * vc.uv.y;

            // Use vertex a color (ImGui draw lists mostly use uniform color per command)
            const ImU32 col = va.col;
            const uint32_t src = to_xrgb(col);

            // Sample font atlas for alpha (8-bit single channel)
            uint32_t alpha = (col >> 24) & 0xFFu;
            if (atlas && aw > 0 && ah > 0) {
                const int tx = std::clamp(static_cast<int>(u * static_cast<float>(aw)), 0, aw - 1);
                const int ty = std::clamp(static_cast<int>(v * static_cast<float>(ah)), 0, ah - 1);
                const uint32_t ta = atlas[ty * aw + tx];
                alpha = (alpha * ta) / 255u;
            }

            uint32_t& dst = pixels[py * pstride + px];
            dst = blend_over(dst, src, alpha);
        }
    }
}

} // namespace

void render_frame(WindowImpl& w, float r, float g, float b) {
    ShmBuffer& buf = w.shm_bufs[w.shm_back];
    if (!buf.data) return;

    auto* pixels = reinterpret_cast<uint32_t*>(buf.data);
    const int pstride = w.shm_stride / 4;  // stride in pixels

    // Clear to background colour
    const uint32_t bg = to_xrgb(IM_COL32(
        static_cast<int>(r * 255.0f),
        static_cast<int>(g * 255.0f),
        static_cast<int>(b * 255.0f), 255));
    const int npix = w.width * w.height;
    for (int i = 0; i < npix; ++i) pixels[i] = bg;

    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->CmdListsCount == 0) return;

    // Build font atlas on first call
    if (!w.font_pixels) {
        unsigned char* px = nullptr;
        ImGui::GetIO().Fonts->GetTexDataAsAlpha8(&px, &w.font_atlas_w, &w.font_atlas_h);
        w.font_pixels = px;
    }

    for (int ci = 0; ci < dd->CmdListsCount; ++ci) {
        const ImDrawList* cl = dd->CmdLists[ci];
        const ImDrawVert* vtx = cl->VtxBuffer.Data;
        const ImDrawIdx*  idx = cl->IdxBuffer.Data;

        for (int cmd_i = 0; cmd_i < cl->CmdBuffer.Size; ++cmd_i) {
            const ImDrawCmd& cmd = cl->CmdBuffer[cmd_i];
            if (cmd.UserCallback) {
                cmd.UserCallback(cl, &cmd);
                idx += cmd.ElemCount;
                continue;
            }
            const ImVec4 clip = cmd.ClipRect;
            for (unsigned ei = 0; ei < cmd.ElemCount; ei += 3) {
                rasterize_tri(pixels, pstride, w.width, w.height,
                              vtx[idx[ei + 0]], vtx[idx[ei + 1]], vtx[idx[ei + 2]],
                              clip, w.font_pixels, w.font_atlas_w, w.font_atlas_h);
            }
            idx += cmd.ElemCount;
        }
    }
}

} // namespace transporter::gui::platform
