// SPDX-License-Identifier: GPL-3.0-or-later
//
// Main view. Cover hero, type hierarchy (title / artist · album), custom
// seekbar with waveform underlay, icon-only transport row, status footer
// with bit-perfect badge + volume.

#include "app.hpp"
#include "albumart.hpp"
#include "../platform/platform.hpp"
#include "../theme.hpp"
#include "../util/dominant_color.hpp"

#include <transporter/engine/device.hpp>
#include <transporter/engine/engine.hpp>
#include <transporter/engine/telemetry.hpp>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace transporter::gui {

namespace {

constexpr ImVec4 kPass{0.40f, 0.85f, 0.40f, 1.0f};
constexpr ImVec4 kFail{0.95f, 0.40f, 0.35f, 1.0f};
constexpr ImVec4 kWarn{0.95f, 0.75f, 0.30f, 1.0f};

// Glyphs picked from JetBrainsMono / Misc-Technical, Geometric Shapes:
//   prev U+23EE, next U+23ED, pause U+23F8, play U+25B6, shuffle U+1F500
//   fallback "S"/"R" for shuffle/repeat if private-use isn't in our ranges.
constexpr const char* kGlyphPrev  = "\xe2\x8f\xae";  // ⏮  U+23EE
constexpr const char* kGlyphNext  = "\xe2\x8f\xad";  // ⏭  U+23ED
constexpr const char* kGlyphPause = "\xe2\x8f\xb8";  // ⏸  U+23F8
constexpr const char* kGlyphPlay  = "\xe2\x96\xb6";  // ▶  U+25B6

void format_time(std::int64_t ms, char* out, std::size_t n) {
    if (ms < 0) ms = 0;
    const std::int64_t s = ms / 1000;
    const std::int64_t m = s / 60;
    const std::int64_t h = m / 60;
    if (h > 0) {
        std::snprintf(out, n, "%lld:%02lld:%02lld",
                      static_cast<long long>(h),
                      static_cast<long long>(m % 60),
                      static_cast<long long>(s % 60));
    } else {
        std::snprintf(out, n, "%lld:%02lld",
                      static_cast<long long>(m),
                      static_cast<long long>(s % 60));
    }
}

// Album-art cache keyed by track-directory path. AlbumArt owns its pixel
// buffer; references stay valid for the lifetime of the process. Lookups
// happen on the GUI thread only — no locking required.
struct ArtCache {
    std::unordered_map<std::string, AlbumArt> map;
    std::string last_loaded;  // last directory we attempted
};

ArtCache& art_cache() {
    static ArtCache c;
    return c;
}

const AlbumArt& art_for_track(const std::filesystem::path& file_path) {
    static const AlbumArt empty;
    if (file_path.empty()) return empty;
    auto& c = art_cache();
    const std::string dir = file_path.parent_path().string();
    if (auto it = c.map.find(dir); it != c.map.end()) return it->second;
    AlbumArt a = load_album_art(file_path.parent_path());
    auto [it, _] = c.map.emplace(dir, std::move(a));
    return it->second;
}

// Push a named font if available; otherwise fall back to the default. The
// matching pop is unconditional — PushFont(nullptr) is well-defined.
struct ScopedFont {
    ScopedFont(ImFont* f) { ImGui::PushFont(f); }
    ~ScopedFont() { ImGui::PopFont(); }
    ScopedFont(const ScopedFont&) = delete;
    ScopedFont& operator=(const ScopedFont&) = delete;
};

ImFont* f_title(AppState& st) { return st.window ? st.window->font_title() : nullptr; }
ImFont* f_h2   (AppState& st) { return st.window ? st.window->font_h2()    : nullptr; }
ImFont* f_small(AppState& st) { return st.window ? st.window->font_small() : nullptr; }
ImFont* f_icon (AppState& st) { return st.window ? st.window->font_icon()  : nullptr; }

// Centre-align the text cursor for a single line of width tw within available width.
void center_x(float content_w, float tw) {
    const float x = ImGui::GetCursorPosX() + (content_w - tw) * 0.5f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
}

// ── transport icon button (manual-draw) ─────────────────────────────────────
// 48×48 hit-target; on hover, render a soft translucent disc behind the glyph.
// Returns true on click.
bool icon_button(const char* id, const char* glyph, ImFont* glyph_font,
                 float size, const ImVec4& glyph_col, bool emphasize) {
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##hit", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 centre(origin.x + size * 0.5f, origin.y + size * 0.5f);
    if (emphasize) {
        dl->AddCircleFilled(centre, size * 0.45f, IM_COL32(255, 255, 255, 22));
    }
    if (hovered) {
        dl->AddCircleFilled(centre, size * 0.48f, IM_COL32(255, 255, 255, 36));
    }
    if (glyph_font) {
        const float fs = glyph_font->FontSize;
        const ImVec2 ts = glyph_font->CalcTextSizeA(fs, FLT_MAX, 0.0f, glyph);
        const ImVec2 pos(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f);
        dl->AddText(glyph_font, fs, pos,
                    ImGui::ColorConvertFloat4ToU32(glyph_col), glyph);
    } else {
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        const ImVec2 pos(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f);
        dl->AddText(pos, ImGui::ColorConvertFloat4ToU32(glyph_col), glyph);
    }
    ImGui::PopID();
    return clicked;
}

// ── verdict dot ─────────────────────────────────────────────────────────────
void verdict_badge(const engine::PipelineSnapshot& s) {
    using L = engine::BitPerfectVerdict::Level;
    ImVec4 col = kFail;
    const char* label = "Not bit-perfect";
    if (s.bit_perfect.level == L::Yes)        { col = kPass; label = "Bit-Perfect"; }
    else if (s.bit_perfect.level == L::Qualified) { col = kWarn; label = "Bit-Perfect (qualified)"; }

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float cy = cursor.y + ImGui::GetTextLineHeight() * 0.5f;
    constexpr float r = 5.0f;
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(cursor.x + r, cy), r, ImGui::ColorConvertFloat4ToU32(col));
    ImGui::Dummy(ImVec2(r * 2.0f + 8.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(label);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("digital path bit-perfect: %s",
                    s.bit_perfect.digital_path_bitperfect ? "yes" : "no");
        ImGui::Text("no resampling in flight: %s",
                    s.bit_perfect.no_resampling_in_flight ? "yes" : "no");
        ImGui::Text("RT enabled: %s",
                    s.bit_perfect.rt_enabled ? "yes" : "no");
        ImGui::Text("no recent xrun: %s",
                    s.bit_perfect.no_recent_xrun ? "yes" : "no");
        ImGui::Text("digital path off: %s",
                    s.bit_perfect.digital_path_off ? "yes" : "no");
        ImGui::Text("no mismatch in flight: %s",
                    s.bit_perfect.no_mismatch_in_flight ? "yes" : "no");
        for (const auto& q : s.bit_perfect.qualifications) {
            ImGui::BulletText("%s", q.c_str());
        }
        ImGui::EndTooltip();
    }
}

// ── cover art block with subtle drop shadow ─────────────────────────────────
void draw_cover(const AlbumArt& art, const DominantColor& dc, ImVec2 pos, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Shadow: two offset rounded rects fading down.
    const ImU32 sh1 = IM_COL32(0, 0, 0, 80);
    const ImU32 sh2 = IM_COL32(0, 0, 0, 30);
    dl->AddRectFilled(ImVec2(pos.x + 2, pos.y + 8),
                      ImVec2(pos.x + size + 2, pos.y + size + 8), sh2, 10.0f);
    dl->AddRectFilled(ImVec2(pos.x + 1, pos.y + 4),
                      ImVec2(pos.x + size + 1, pos.y + size + 4), sh1, 10.0f);

    const ImVec2 a = pos;
    const ImVec2 b = ImVec2(pos.x + size, pos.y + size);
    if (art) {
        // Square image. Avoid AddImageRounded: the CPU renderer's textured
        // triangle path samples the font atlas, which would mis-render the
        // rounded corner triangles. The drop shadow softens the edge enough.
        dl->AddImage(art.texture_id(), a, b);
    } else {
        // Placeholder: accent-tinted square with a centred note glyph.
        const ImU32 fill = ImGui::ColorConvertFloat4ToU32(
            ImVec4(dc.accent.x * 0.40f, dc.accent.y * 0.40f, dc.accent.z * 0.40f, 1.0f));
        dl->AddRectFilled(a, b, fill, 10.0f);
        const char* note = "\xe2\x99\xaa";  // ♪
        const ImVec2 ts  = ImGui::CalcTextSize(note);
        dl->AddText(ImVec2(pos.x + (size - ts.x) * 0.5f,
                           pos.y + (size - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 140), note);
    }
}

// ── custom seekbar with optional waveform underlay ──────────────────────────
// Returns true on commit; out_target_frac is the desired position [0,1].
bool draw_seekbar(AppState& st,
                  const engine::PipelineSnapshot& snap,
                  float frac,
                  float width,
                  const DominantColor& dc,
                  float& out_frac) {
    out_frac = frac;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float kHitH   = 22.0f;
    constexpr float kTrackH = 6.0f;
    ImGui::InvisibleButton("##seekhit", ImVec2(width, kHitH));
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    const float track_y = origin.y + (kHitH - kTrackH) * 0.5f;
    const ImVec2 tmin(origin.x, track_y);
    const ImVec2 tmax(origin.x + width, track_y + kTrackH);
    const float radius = kTrackH * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Subtle waveform underlay (drawn behind the track so it tints the inactive part).
    if (st.waveform_ready.load(std::memory_order_acquire)) {
        std::lock_guard lk(st.waveform_mtx);
        if (!st.waveform.empty()) {
            const float wh   = kHitH;
            const float cy   = origin.y + wh * 0.5f;
            const auto  N    = static_cast<float>(st.waveform.size());
            const float bw   = width / N;
            const ImU32 wcol = IM_COL32(255, 255, 255, 32);
            for (std::size_t i = 0; i < st.waveform.size(); ++i) {
                const float v  = st.waveform[i];
                const float x0 = origin.x + static_cast<float>(i) * bw;
                const float hh = v * wh * 0.5f;
                dl->AddRectFilled({x0, cy - hh}, {x0 + bw, cy + hh}, wcol);
            }
        }
    }

    // Inactive track.
    const ImU32 track_col = IM_COL32(255, 255, 255, 32);
    dl->AddRectFilled(tmin, tmax, track_col, radius);

    // Filled portion.
    const float clamped = std::clamp(frac, 0.0f, 1.0f);
    const float fw = clamped * width;
    if (fw > 0.0f) {
        const ImU32 acc = ImGui::ColorConvertFloat4ToU32(dc.accent);
        dl->AddRectFilled(tmin, ImVec2(tmin.x + fw, tmax.y), acc, radius);
    }

    // Cursor: filled circle. Slightly larger when active.
    const float cursor_x = origin.x + fw;
    const float cursor_y = track_y + radius;
    const float crad = active ? 8.0f : (hovered ? 7.0f : 6.0f);
    dl->AddCircleFilled(ImVec2(cursor_x, cursor_y), crad,
                        IM_COL32(245, 245, 250, 255));

    // Hover tooltip with target time.
    const std::int64_t total_ms = snap.source.duration.count();
    if (hovered && total_ms > 0) {
        const float hf = (ImGui::GetMousePos().x - origin.x) / width;
        const std::int64_t hms = static_cast<std::int64_t>(
            std::clamp(hf, 0.0f, 1.0f) * static_cast<float>(total_ms));
        char tb[24];
        format_time(hms, tb, sizeof(tb));
        ImGui::SetTooltip("%s", tb);
    }

    // Drag-to-scrub.
    if (active) {
        const float nf = (ImGui::GetMousePos().x - origin.x) / width;
        out_frac = std::clamp(nf, 0.0f, 1.0f);
    }
    return active && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// ── DAC popup (gear/cog menu) ──────────────────────────────────────────────
void dac_popup(AppState& st, const engine::PipelineSnapshot& snap) {
    if (!ImGui::BeginPopup("##dac_popup")) return;

    if (st.devices.empty()) {
        ImGui::TextDisabled("no DAC found");
    } else {
        ImGui::TextDisabled("output device");
        ImGui::Separator();
        for (const auto& d : st.devices) {
            const bool selected = (d.alsa_hw_string == st.preferred_device);
            const bool busy = d.caps.caps_probe_failed;
            std::string label = d.fingerprint.alsa_card_name;
            if (busy) label += "  (busy)";
            if (ImGui::MenuItem(label.c_str(), nullptr, selected,
                                !busy || selected)) {
                if (d.alsa_hw_string != st.preferred_device) {
                    st.pending_device_switch = d.alsa_hw_string;
                }
            }
            if (busy && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", d.caps.probe_failure_reason.c_str());
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reclaim DAC")) {
            if (!st.preferred_device.empty()) {
                st.pending_device_switch = st.preferred_device;
            }
        }
        if (ImGui::MenuItem("Release DAC")) {
            st.release_dac_requested = true;
        }
    }
    (void)snap;
    ImGui::EndPopup();
}

// ── empty state ─────────────────────────────────────────────────────────────
void draw_empty_state(AppState& st) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float content_w = avail.x;

    // Vertically centre.
    const float vpad = std::max(40.0f, (avail.y - 240.0f) * 0.4f);
    ImGui::Dummy(ImVec2(0, vpad));

    // Centred icon.
    if (auto* fi = f_icon(st)) {
        ScopedFont scoped(fi);
        const char* glyph = "\xe2\x99\xab";  // ♫
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        center_x(content_w, ts.x);
        ImGui::TextDisabled("%s", glyph);
    }
    ImGui::Dummy(ImVec2(0, 8));

    // Headline.
    if (auto* ft = f_title(st)) {
        ScopedFont scoped(ft);
        const char* head = "No track loaded";
        const ImVec2 ts = ImGui::CalcTextSize(head);
        center_x(content_w, ts.x);
        ImGui::TextUnformatted(head);
    } else {
        const char* head = "No track loaded";
        const ImVec2 ts = ImGui::CalcTextSize(head);
        center_x(content_w, ts.x);
        ImGui::TextUnformatted(head);
    }
    ImGui::Dummy(ImVec2(0, 4));

    // Sub-line — varies with what's missing.
    {
        std::string sub;
        if (st.engine_ == nullptr) {
            if (st.devices.empty())                sub = "No DAC detected. Plug one in, then restart.";
            else if (st.preferred_device.empty())  sub = "Select a DAC to begin.";
            else                                   sub = "Initialising audio engine…";
        } else if (st.library_ == nullptr ||
                   st.cfg.library.roots.empty()) {
            sub = "Add a library root in ~/.config/transporter/config.toml.";
        } else {
            sub = "Open the Library tab and pick a track.";
        }
        if (auto* fh = f_h2(st)) {
            ScopedFont scoped(fh);
            const ImVec2 ts = ImGui::CalcTextSize(sub.c_str());
            center_x(content_w, ts.x);
            ImGui::TextDisabled("%s", sub.c_str());
        } else {
            const ImVec2 ts = ImGui::CalcTextSize(sub.c_str());
            center_x(content_w, ts.x);
            ImGui::TextDisabled("%s", sub.c_str());
        }
    }
    ImGui::Dummy(ImVec2(0, 24));

    // Inline DAC selector when one exists but none is chosen.
    if (st.engine_ == nullptr && !st.devices.empty()) {
        const float bw = 220.0f;
        center_x(content_w, bw);
        ImGui::SetNextItemWidth(bw);
        std::string preview = st.preferred_device.empty() ? "(select)" : st.preferred_device;
        for (const auto& d : st.devices) {
            if (d.alsa_hw_string == st.preferred_device) {
                preview = d.fingerprint.alsa_card_name;
                break;
            }
        }
        if (ImGui::BeginCombo("##empty_dac", preview.c_str())) {
            for (const auto& d : st.devices) {
                const bool selected = (d.alsa_hw_string == st.preferred_device);
                const bool busy = d.caps.caps_probe_failed;
                std::string label = d.fingerprint.alsa_card_name;
                if (busy) label += "  (busy)";
                if (ImGui::Selectable(label.c_str(), selected,
                                      busy ? ImGuiSelectableFlags_Disabled : 0)) {
                    if (d.alsa_hw_string != st.preferred_device) {
                        st.pending_device_switch = d.alsa_hw_string;
                    }
                }
            }
            ImGui::EndCombo();
        }
    }
}

// ── status footer (format · bit-perfect · volume) ───────────────────────────
void draw_status_footer(AppState& st, const engine::PipelineSnapshot& snap) {
    if (auto* fs = f_small(st)) ImGui::PushFont(fs);

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    // Left: format summary + bit-perfect verdict.
    // Right: volume slider + HW/SW pill.
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float vol_w   = 260.0f;
    const float left_w  = total_w - vol_w - 24.0f;

    ImGui::BeginGroup();
    {
        // Format summary.
        char fmt[96];
        if (snap.source.sample_rate_hz > 0) {
            const double khz = static_cast<double>(snap.source.sample_rate_hz) / 1000.0;
            const unsigned bd = snap.source.bit_depth_file;
            const char* codec = snap.source.codec_name.empty()
                ? "PCM" : snap.source.codec_name.c_str();
            if (bd > 0) {
                std::snprintf(fmt, sizeof(fmt), "%.1f kHz  %u-bit  %s", khz, bd, codec);
            } else {
                std::snprintf(fmt, sizeof(fmt), "%.1f kHz  %s", khz, codec);
            }
        } else {
            std::snprintf(fmt, sizeof(fmt), "—");
        }
        ImGui::TextDisabled("%s", fmt);
        ImGui::SameLine();
        ImGui::TextDisabled(" \xc2\xb7 ");  // middle dot
        ImGui::SameLine();
        verdict_badge(snap);
        ImGui::SameLine();
        ImGui::TextDisabled(" \xc2\xb7 ");
        ImGui::SameLine();

        // Compact DAC label, click for menu.
        std::string dac = "no DAC";
        for (const auto& d : st.devices) {
            if (d.alsa_hw_string == st.preferred_device) {
                dac = d.fingerprint.alsa_card_name;
                break;
            }
        }
        // Truncate over-long names.
        if (dac.size() > 28) dac = dac.substr(0, 27) + "\xe2\x80\xa6";
        if (ImGui::SmallButton(dac.c_str())) {
            ImGui::OpenPopup("##dac_popup");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("change output device");
        dac_popup(st, snap);

        // Shuffle / repeat indicators (clickable, dimmed when off).
        ImGui::SameLine();
        ImGui::TextDisabled(" \xc2\xb7 ");
        ImGui::SameLine();
        const ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        const ImVec4 lit = ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab);
        ImGui::PushStyleColor(ImGuiCol_Text, st.shuffle ? lit : dim);
        if (ImGui::SmallButton("shuffle")) {
            st.queue_shuffle_toggle();
            if (st.dbus_) st.dbus_->notify_shuffle_changed();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const char* rep_l = "repeat";
        if (st.repeat_mode == AppState::RepeatMode::One) rep_l = "repeat 1";
        else if (st.repeat_mode == AppState::RepeatMode::All) rep_l = "repeat all";
        const bool rep_on = (st.repeat_mode != AppState::RepeatMode::None);
        ImGui::PushStyleColor(ImGuiCol_Text, rep_on ? lit : dim);
        if (ImGui::SmallButton(rep_l)) {
            {
                std::lock_guard lk(st.queue_mtx);
                switch (st.repeat_mode) {
                case AppState::RepeatMode::None: st.repeat_mode = AppState::RepeatMode::One;  break;
                case AppState::RepeatMode::One:  st.repeat_mode = AppState::RepeatMode::All;  break;
                case AppState::RepeatMode::All:  st.repeat_mode = AppState::RepeatMode::None; break;
                }
            }
            st.refresh_preload();
            if (st.dbus_) st.dbus_->notify_loop_status_changed();
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    (void)left_w;

    // Right-aligned volume.
    ImGui::SameLine();
    {
        const float gx = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x
                       - vol_w - ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorScreenPos(ImVec2(gx,
            ImGui::GetCursorScreenPos().y - ImGui::GetTextLineHeight() * 1.2f));
    }
    ImGui::BeginGroup();
    {
        const auto& hw_string = snap.device.current_hw_string;
        const auto& vol = snap.device.capabilities.hw_volume;
        const bool have_hw = vol.present && !hw_string.empty();
        // Speaker glyph + pill + slider.
        if (have_hw) {
            const auto now = std::chrono::steady_clock::now();
            if (st.hw_volume_pct < 0 ||
                now - st.hw_volume_last_poll > std::chrono::milliseconds(250)) {
                st.hw_volume_pct = engine::get_hw_volume_pct(hw_string);
                if (st.hw_volume_pct < 0) st.hw_volume_pct = 0;
                st.hw_volume_muted = engine::get_hw_mute_state(hw_string);
                st.hw_volume_last_poll = now;
            }
            // Plain ASCII label; the Nerd Font speaker glyphs at U+1F507/9 sit
            // in supplementary planes that the atlas glyph ranges don't cover.
            if (st.hw_volume_muted) ImGui::TextDisabled("MUTE");
            else                    ImGui::TextDisabled("VOL");
            ImGui::SameLine();
            int pct = st.hw_volume_pct;
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderInt("##vol", &pct, 0, 100, "%d%%")) {
                st.hw_volume_pct = pct;
                engine::set_hw_volume_pct(hw_string, pct);
                if (st.dbus_) st.dbus_->notify_volume_changed();
            }
            ImGui::SameLine();
            // HW pill — small filled rect with the literal text "HW".
            {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 ts = ImGui::CalcTextSize("HW");
                const ImVec2 pill = ImVec2(ts.x + 12.0f, ts.y + 4.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(
                    ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab));
                dl->AddRectFilled(p0, ImVec2(p0.x + pill.x, p0.y + pill.y),
                                  col, 6.0f);
                dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 2.0f),
                            IM_COL32(20, 20, 24, 255), "HW");
                ImGui::Dummy(pill);
            }
        } else if (!hw_string.empty()) {
            ImGui::TextDisabled("Vol — no HW control");
        } else {
            ImGui::TextDisabled("Vol —");
        }
    }
    ImGui::EndGroup();

    if (auto* fs = f_small(st); fs) ImGui::PopFont();
}

// ── per-frame: pick album art and apply theme tint if changed ───────────────
void update_theme_for_track(AppState& st, const engine::PipelineSnapshot& snap) {
    (void)st;
    static std::string last_key;
    const std::string key = snap.source.file_path;
    if (key == last_key) return;
    last_key = key;

    if (key.empty()) {
        apply_theme(default_dominant_color());
        return;
    }
    const AlbumArt& art = art_for_track(std::filesystem::path{key});
    const DominantColor dc = compute_dominant(key, art);
    apply_theme(dc);
}

} // namespace

void draw_main_view(AppState& st) {
    engine::PipelineSnapshot snap{};
    if (st.engine_) snap = st.engine_->pipeline_snapshot();

    // Recompute dominant colour when the active track path changes.
    update_theme_for_track(st, snap);

    // Mini-mode toggle — small text in top-right of the view.
    {
        const float lw = ImGui::CalcTextSize("mini").x +
                         ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - lw);
        if (ImGui::SmallButton(st.mini_mode ? "full" : "mini")) {
            st.mini_mode = !st.mini_mode;
        }
        ImGui::NewLine();
    }

    // Toast (transient error).
    {
        std::lock_guard lk(st.toast_mtx);
        if (st.toast.has_value()) {
            if (std::chrono::steady_clock::now() < st.toast->expires) {
                constexpr ImVec4 kToastBg{0.45f, 0.12f, 0.12f, 1.0f};
                ImGui::PushStyleColor(ImGuiCol_ChildBg, kToastBg);
                ImGui::BeginChild("##toast", ImVec2(-FLT_MIN, 32),
                                  ImGuiChildFlags_None);
                ImGui::SetCursorPos(ImVec2(16, 6));
                ImGui::Text("%s", st.toast->msg.c_str());
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0, 4));
            } else {
                st.toast.reset();
            }
        }
    }

    // Persistent banner for transient non-playing states.
    {
        const auto eng_state = st.last_engine_state.load(std::memory_order_relaxed);
        const char* msg = nullptr;
        ImVec4 bg{};
        if (eng_state == engine::State::Disconnected) {
            msg = "DAC disconnected — reconnect to resume";
            bg = ImVec4(0.40f, 0.26f, 0.06f, 1.0f);
        } else if (eng_state == engine::State::Error) {
            msg = "engine error — see event log";
            bg = ImVec4(0.40f, 0.08f, 0.08f, 1.0f);
        }
        if (msg) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::BeginChild("##banner", ImVec2(-FLT_MIN, 26),
                              ImGuiChildFlags_None);
            ImGui::SetCursorPos(ImVec2(16, 4));
            ImGui::TextUnformatted(msg);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, 4));
        }
    }

    // Empty state: no engine OR no track loaded.
    const bool has_track = !snap.source.file_path.empty() ||
                           !snap.source.tags.title.empty();
    if (st.engine_ == nullptr || !has_track) {
        draw_empty_state(st);
        return;
    }

    // ── hero (cover) ────────────────────────────────────────────────────────
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float content_w = region.x;
    const float cover_size = std::clamp(content_w * 0.42f, 220.0f, 320.0f);
    ImGui::Dummy(ImVec2(0, 4));
    {
        const float gx = ImGui::GetCursorPosX() + (content_w - cover_size) * 0.5f;
        ImGui::SetCursorPosX(gx);
        const ImVec2 screen_pos = ImGui::GetCursorScreenPos();
        const AlbumArt& art = art_for_track(std::filesystem::path{snap.source.file_path});
        // We need the active DominantColor for the placeholder tint; cheap to recompute.
        DominantColor dc = compute_dominant(snap.source.file_path, art);
        draw_cover(art, dc, screen_pos, cover_size);
        ImGui::Dummy(ImVec2(cover_size, cover_size + 4));
    }

    // ── title (28pt) ────────────────────────────────────────────────────────
    {
        std::string title = !snap.source.tags.title.empty()
            ? snap.source.tags.title
            : std::filesystem::path{snap.source.file_path}.stem().string();
        ImGui::Dummy(ImVec2(0, 18));
        if (auto* ft = f_title(st)) ImGui::PushFont(ft);
        const ImVec2 ts = ImGui::CalcTextSize(title.c_str());
        center_x(content_w, ts.x);
        ImGui::TextUnformatted(title.c_str());
        if (f_title(st)) ImGui::PopFont();
        if (ImGui::IsItemHovered() && !snap.source.file_path.empty()) {
            ImGui::SetTooltip("%s", snap.source.file_path.c_str());
        }
    }

    // ── artist · album (18pt, dim) ─────────────────────────────────────────
    {
        std::string line;
        if (!snap.source.tags.artist.empty()) line = snap.source.tags.artist;
        if (!snap.source.tags.album.empty()) {
            if (!line.empty()) line += "  \xc2\xb7  ";
            line += snap.source.tags.album;
        }
        if (!line.empty()) {
            ImGui::Dummy(ImVec2(0, 2));
            if (auto* fh = f_h2(st)) ImGui::PushFont(fh);
            const ImVec2 ts = ImGui::CalcTextSize(line.c_str());
            center_x(content_w, ts.x);
            ImGui::TextDisabled("%s", line.c_str());
            if (f_h2(st)) ImGui::PopFont();
        }
    }

    // Queue position (small, below artist line).
    {
        std::lock_guard lk(st.queue_mtx);
        if (st.queue.size() > 1 && st.queue_index >= 0) {
            char qb[32];
            std::snprintf(qb, sizeof(qb), "%d of %d",
                          st.queue_index + 1,
                          static_cast<int>(st.queue.size()));
            if (auto* fs = f_small(st)) ImGui::PushFont(fs);
            const ImVec2 ts = ImGui::CalcTextSize(qb);
            center_x(content_w, ts.x);
            ImGui::TextDisabled("%s", qb);
            if (f_small(st)) ImGui::PopFont();
        }
    }

    ImGui::Dummy(ImVec2(0, 14));

    // ── seekbar ─────────────────────────────────────────────────────────────
    const std::int64_t total_ms = snap.source.duration.count();
    const std::int64_t track_frames =
        static_cast<std::int64_t>(snap.output.frames_written) -
        static_cast<std::int64_t>(snap.output.frames_written_at_track_start);
    const std::int64_t elapsed_ms = snap.source.sample_rate_hz > 0
        ? (track_frames * 1000LL) / static_cast<std::int64_t>(snap.source.sample_rate_hz)
        : 0;
    const float frac = total_ms > 0
        ? std::clamp(static_cast<float>(elapsed_ms) / static_cast<float>(total_ms), 0.0f, 1.0f)
        : 0.0f;

    {
        const float seek_w = std::min(content_w - 80.0f, 720.0f);
        const float seek_x = ImGui::GetCursorPosX() + (content_w - seek_w) * 0.5f;
        ImGui::SetCursorPosX(seek_x);
        // Get the DominantColor at this point; it was cached by compute_dominant.
        const AlbumArt& art = art_for_track(std::filesystem::path{snap.source.file_path});
        DominantColor dc = compute_dominant(snap.source.file_path, art);
        float target = frac;
        const bool committed = draw_seekbar(st, snap, frac, seek_w, dc, target);
        if (committed && snap.source.total_frames > 0) {
            const auto tgt = static_cast<std::uint64_t>(
                target * static_cast<float>(snap.source.total_frames));
            (void)st.engine_->seek(tgt);
            if (st.dbus_ && snap.source.sample_rate_hz > 0) {
                const auto pos_us = static_cast<std::int64_t>(
                    (tgt * 1'000'000ULL) / snap.source.sample_rate_hz);
                st.dbus_->notify_seeked(pos_us);
            }
        }

        // Time labels: current (left) / duration (right), flanking the bar.
        char tbuf[24], dbuf[24];
        format_time(elapsed_ms, tbuf, sizeof(tbuf));
        format_time(total_ms,   dbuf, sizeof(dbuf));
        if (auto* fs = f_small(st)) ImGui::PushFont(fs);
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetCursorPosX(seek_x);
        ImGui::TextDisabled("%s", tbuf);
        ImGui::SameLine();
        const ImVec2 dts = ImGui::CalcTextSize(dbuf);
        ImGui::SetCursorPosX(seek_x + seek_w - dts.x);
        ImGui::TextDisabled("%s", dbuf);
        if (f_small(st)) ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0, 14));

    // ── transport row (icon-only) ──────────────────────────────────────────
    {
        constexpr float kBtn = 48.0f;
        constexpr float kPlayBtn = 60.0f;  // emphasised centre button
        constexpr float kGap = 18.0f;
        const float row_w = kBtn + kGap + kPlayBtn + kGap + kBtn;
        const float row_x = ImGui::GetCursorPosX() + (content_w - row_w) * 0.5f;
        const float y0 = ImGui::GetCursorPosY();
        const ImVec4 col = ImGui::GetStyle().Colors[ImGuiCol_Text];

        // prev
        ImGui::SetCursorPos(ImVec2(row_x, y0 + (kPlayBtn - kBtn) * 0.5f));
        if (icon_button("prev", kGlyphPrev, f_icon(st), kBtn, col, false)) {
            { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
            if (auto p = st.queue_previous(); p) {
                (void)st.engine_->load(*p);
                (void)st.engine_->play();
                if (st.dbus_) st.dbus_->notify_track_loaded();
            }
        }

        // play / pause (emphasised)
        const bool playing = (snap.engine_state == engine::State::Playing);
        ImGui::SetCursorPos(ImVec2(row_x + kBtn + kGap, y0));
        if (icon_button("playpause", playing ? kGlyphPause : kGlyphPlay,
                        f_icon(st), kPlayBtn, col, true)) {
            if (playing) (void)st.engine_->pause();
            else         (void)st.engine_->play();
        }

        // next
        ImGui::SetCursorPos(ImVec2(row_x + kBtn + kGap + kPlayBtn + kGap,
                                   y0 + (kPlayBtn - kBtn) * 0.5f));
        if (icon_button("next", kGlyphNext, f_icon(st), kBtn, col, false)) {
            { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
            if (auto n = st.queue_next(); n) {
                (void)st.engine_->load(*n);
                (void)st.engine_->play();
                if (st.dbus_) st.dbus_->notify_track_loaded();
            }
        }
        // Leave a gap below the transport row.
        ImGui::SetCursorPosY(y0 + kPlayBtn + 12.0f);
    }

    // ── status footer ──────────────────────────────────────────────────────
    draw_status_footer(st, snap);
}

// ── mini view (compact horizontal bar) ──────────────────────────────────────
void draw_mini_view(AppState& st) {
    engine::PipelineSnapshot snap{};
    if (st.engine_) snap = st.engine_->pipeline_snapshot();

    if (ImGui::SmallButton("full")) { st.mini_mode = false; }
    ImGui::SameLine();

    std::string display;
    if (!snap.source.tags.title.empty()) {
        display = snap.source.tags.title;
        if (!snap.source.tags.artist.empty())
            display += "  \xc2\xb7  " + snap.source.tags.artist;
    } else if (!snap.source.file_path.empty()) {
        display = std::filesystem::path{snap.source.file_path}.stem().string();
    } else {
        display = "\xe2\x80\x94";
    }
    ImGui::TextUnformatted(display.c_str());
    ImGui::SameLine();

    const std::int64_t total_ms = snap.source.duration.count();
    const std::int64_t fw = static_cast<std::int64_t>(snap.output.frames_written) -
                            static_cast<std::int64_t>(snap.output.frames_written_at_track_start);
    const std::int64_t elapsed_ms = snap.source.sample_rate_hz > 0
        ? (fw * 1000LL) / snap.source.sample_rate_hz : 0;
    char tbuf[24], dbuf[24];
    format_time(elapsed_ms < 0 ? 0 : elapsed_ms, tbuf, sizeof(tbuf));
    format_time(total_ms, dbuf, sizeof(dbuf));
    ImGui::TextDisabled("%s / %s", tbuf, dbuf);
    ImGui::SameLine();

    const bool playing = (snap.engine_state == engine::State::Playing);
    if (st.engine_) {
        if (ImGui::SmallButton(playing ? kGlyphPause : kGlyphPlay)) {
            playing ? (void)st.engine_->pause() : (void)st.engine_->play();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(kGlyphPrev)) {
            { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
            if (auto p = st.queue_previous(); p) {
                (void)st.engine_->load(*p);
                (void)st.engine_->play();
                if (st.dbus_) st.dbus_->notify_track_loaded();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(kGlyphNext)) {
            { std::lock_guard lk(st.queue_mtx); st.pending_preload_path.clear(); }
            if (auto n = st.queue_next(); n) {
                (void)st.engine_->load(*n);
                (void)st.engine_->play();
                if (st.dbus_) st.dbus_->notify_track_loaded();
            }
        }
        ImGui::SameLine();
        verdict_badge(snap);
    }

    if (total_ms > 0) {
        const float fr = std::clamp(
            static_cast<float>(elapsed_ms < 0 ? 0 : elapsed_ms) /
            static_cast<float>(total_ms), 0.0f, 1.0f);
        ImGui::ProgressBar(fr, ImVec2(-FLT_MIN, 3.0f), "");
    }
}

} // namespace transporter::gui
