// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU software rasterizer for Dear ImGui draw data.
// Output format: WL_SHM_FORMAT_XRGB8888 (LE bytes: B G R X).

#include "platform_internal.hpp"

// AlbumArt is a plain struct with a pixel pointer; include the header so the
// renderer can cast ImTextureID back to it for image blits.
#include "../views/albumart.hpp"

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

            // Determine whether this draw command uses a user texture (AlbumArt)
            // or the font atlas. The font atlas TextureId is set by ImGui to
            // the value we stored; anything else is cast back to AlbumArt*.
            const ImTextureID font_id = ImGui::GetIO().Fonts->TexID;
            const bool is_user_tex = (cmd.TextureId != font_id) && (cmd.TextureId != ImTextureID{});
            const transporter::gui::AlbumArt* art = nullptr;
            if (is_user_tex) {
                art = reinterpret_cast<const transporter::gui::AlbumArt*>(cmd.TextureId);
                // Sanity: if pixels are null, fall back to font-atlas path.
                if (!art || !art->pixels) { art = nullptr; }
            }

            const ImVec4 clip = cmd.ClipRect;
            unsigned ei = 0;
            while (ei < cmd.ElemCount) {
                // Fast path: 6-index axis-aligned filled quad (two triangles, uniform colour).
                // ImGui standard pattern: {a,b,c, a,c,d}
                if (ei + 6 <= cmd.ElemCount) {
                    const ImDrawIdx i0 = idx[ei+0], i1 = idx[ei+1], i2 = idx[ei+2];
                    const ImDrawIdx i3 = idx[ei+3], i4 = idx[ei+4], i5 = idx[ei+5];
                    if (i0 == i3 && i2 == i4) {
                        const ImDrawVert& va = vtx[i0];
                        const ImDrawVert& vb = vtx[i1];
                        const ImDrawVert& vc = vtx[i2];
                        const ImDrawVert& vd = vtx[i5];

                        const float x0 = std::min({va.pos.x, vb.pos.x, vc.pos.x, vd.pos.x});
                        const float y0 = std::min({va.pos.y, vb.pos.y, vc.pos.y, vd.pos.y});
                        const float x1 = std::max({va.pos.x, vb.pos.x, vc.pos.x, vd.pos.x});
                        const float y1 = std::max({va.pos.y, vb.pos.y, vc.pos.y, vd.pos.y});

                        const int rx0 = std::max({static_cast<int>(x0), static_cast<int>(clip.x), 0});
                        const int ry0 = std::max({static_cast<int>(y0), static_cast<int>(clip.y), 0});
                        const int rx1 = std::min({static_cast<int>(x1), static_cast<int>(clip.z), w.width});
                        const int ry1 = std::min({static_cast<int>(y1), static_cast<int>(clip.w), w.height});

                        if (art && art->width > 0 && art->height > 0) {
                            // Image blit: nearest-neighbour sample from AlbumArt pixel buffer.
                            // UV coordinates come from the four vertices; compute per-pixel.
                            // va=top-left, vb=top-right, vc=bottom-right, vd=bottom-left
                            // (ImGui AddImage emits {tl,tr,br, tl,br,bl}).
                            const float uv_x0 = std::min({va.uv.x, vb.uv.x, vc.uv.x, vd.uv.x});
                            const float uv_y0 = std::min({va.uv.y, vb.uv.y, vc.uv.y, vd.uv.y});
                            const float uv_x1 = std::max({va.uv.x, vb.uv.x, vc.uv.x, vd.uv.x});
                            const float uv_y1 = std::max({va.uv.y, vb.uv.y, vc.uv.y, vd.uv.y});
                            const float quad_w = x1 - x0;
                            const float quad_h = y1 - y0;
                            if (rx0 < rx1 && ry0 < ry1 && quad_w > 0.5f && quad_h > 0.5f) {
                                const uint32_t src_a = (va.col >> 24) & 0xFFu;
                                for (int py = ry0; py < ry1; ++py) {
                                    const float t = (static_cast<float>(py) + 0.5f - y0) / quad_h;
                                    const float v = uv_y0 + t * (uv_y1 - uv_y0);
                                    const int ty = std::clamp(static_cast<int>(v * static_cast<float>(art->height)),
                                                              0, art->height - 1);
                                    uint32_t* out = pixels + py * pstride + rx0;
                                    for (int px = rx0; px < rx1; ++px) {
                                        const float s = (static_cast<float>(px) + 0.5f - x0) / quad_w;
                                        const float u = uv_x0 + s * (uv_x1 - uv_x0);
                                        const int tx = std::clamp(static_cast<int>(u * static_cast<float>(art->width)),
                                                                  0, art->width - 1);
                                        const uint32_t texel = art->pixels[static_cast<std::size_t>(ty * art->width + tx)];
                                        const uint32_t written = (src_a >= 255u) ? texel : blend_over(*out, texel, src_a);
                                        *out++ = written;
                                    }
                                }
                            }
                            ei += 6;
                            continue;
                        }

                        if (va.col == vb.col && va.col == vc.col && va.col == vd.col) {
                            const uint32_t src_a = (va.col >> 24) & 0xFFu;
                            const uint32_t src   = to_xrgb(va.col);
                            // Solid fills (button backgrounds, rects) have all four UV coords
                            // pointing to the atlas white pixel — UV extent ≈ 0.  Sample once.
                            // Glyph quads span a real atlas region; sampling the centre and
                            // applying it uniformly paints every pixel identically → solid box.
                            const float uv_dx = std::abs(va.uv.x - vc.uv.x);
                            const float uv_dy = std::abs(va.uv.y - vc.uv.y);
                            if (uv_dx < 1e-4f && uv_dy < 1e-4f) {
                                // Solid fill — one atlas sample.
                                uint32_t tex_alpha = 255u;
                                if (w.font_pixels && w.font_atlas_w > 0 && w.font_atlas_h > 0) {
                                    const float cu = (va.uv.x + vb.uv.x + vc.uv.x + vd.uv.x) * 0.25f;
                                    const float cv = (va.uv.y + vb.uv.y + vc.uv.y + vd.uv.y) * 0.25f;
                                    const int tx = std::clamp(static_cast<int>(cu * static_cast<float>(w.font_atlas_w)), 0, w.font_atlas_w - 1);
                                    const int ty = std::clamp(static_cast<int>(cv * static_cast<float>(w.font_atlas_h)), 0, w.font_atlas_h - 1);
                                    tex_alpha = w.font_pixels[ty * w.font_atlas_w + tx];
                                }
                                const uint32_t alpha = (src_a * tex_alpha) / 255u;
                                if (rx0 < rx1 && ry0 < ry1) {
                                    if (alpha >= 255u) {
                                        for (int py = ry0; py < ry1; ++py) {
                                            uint32_t* row = pixels + py * pstride + rx0;
                                            for (int px = rx0; px < rx1; ++px) *row++ = src;
                                        }
                                    } else if (alpha > 0u) {
                                        for (int py = ry0; py < ry1; ++py) {
                                            uint32_t* row = pixels + py * pstride + rx0;
                                            for (int px = rx0; px < rx1; ++px) {
                                                *row = blend_over(*row, src, alpha);
                                                ++row;
                                            }
                                        }
                                    }
                                }
                            } else if (src_a > 0u && rx0 < rx1 && ry0 < ry1 &&
                                       w.font_pixels && w.font_atlas_w > 0 && w.font_atlas_h > 0) {
                                // Glyph quad — per-pixel UV sampling.
                                const float g_u0 = std::min({va.uv.x, vb.uv.x, vc.uv.x, vd.uv.x});
                                const float g_v0 = std::min({va.uv.y, vb.uv.y, vc.uv.y, vd.uv.y});
                                const float g_u1 = std::max({va.uv.x, vb.uv.x, vc.uv.x, vd.uv.x});
                                const float g_v1 = std::max({va.uv.y, vb.uv.y, vc.uv.y, vd.uv.y});
                                const float gqw = x1 - x0;
                                const float gqh = y1 - y0;
                                for (int py = ry0; py < ry1; ++py) {
                                    const float t = gqh > 0.5f ? (static_cast<float>(py) + 0.5f - y0) / gqh : 0.0f;
                                    const float v = g_v0 + t * (g_v1 - g_v0);
                                    const int ty = std::clamp(static_cast<int>(v * static_cast<float>(w.font_atlas_h)), 0, w.font_atlas_h - 1);
                                    uint32_t* row = pixels + py * pstride + rx0;
                                    for (int px = rx0; px < rx1; ++px) {
                                        const float s = gqw > 0.5f ? (static_cast<float>(px) + 0.5f - x0) / gqw : 0.0f;
                                        const float u = g_u0 + s * (g_u1 - g_u0);
                                        const int tx = std::clamp(static_cast<int>(u * static_cast<float>(w.font_atlas_w)), 0, w.font_atlas_w - 1);
                                        const uint32_t ta = w.font_pixels[ty * w.font_atlas_w + tx];
                                        const uint32_t alpha = (src_a * ta) / 255u;
                                        if (alpha >= 255u) {
                                            *row++ = src;
                                        } else if (alpha > 0u) {
                                            *row = blend_over(*row, src, alpha);
                                            ++row;
                                        } else {
                                            ++row;
                                        }
                                    }
                                }
                            }
                            ei += 6;
                            continue;
                        }
                    }
                }
                // General case: single triangle
                rasterize_tri(pixels, pstride, w.width, w.height,
                              vtx[idx[ei+0]], vtx[idx[ei+1]], vtx[idx[ei+2]],
                              clip, w.font_pixels, w.font_atlas_w, w.font_atlas_h);
                ei += 3;
            }
            idx += cmd.ElemCount;
        }
    }
}

} // namespace transporter::gui::platform
