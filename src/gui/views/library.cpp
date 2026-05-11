// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library browse view. Two-pane: artists left, albums + tracks right.
// Search box at top binds to Library::search.

#include "app.hpp"
#include "albumart.hpp"

#include <transporter/library/library.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace transporter::gui {

namespace {

constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};

constexpr float kThumbSize = 64.0f;

// Per-album art cache. Key is album id; value is the decoded art (or an
// empty AlbumArt if no cover was found, so we don't re-probe every frame).
// Lives for the process lifetime — albums don't change without restart.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unordered_map<std::int64_t, AlbumArt> g_art_cache;

// Library query cache. Re-querying SQLite every frame is wasteful; these
// caches are invalidated when a background scan completes. All fields are
// accessed from the render thread only — no locking needed.
struct LibraryCache {
    std::vector<library::Artist>  artists;
    bool                          artists_valid = false;

    std::string                   albums_artist;
    std::vector<library::Album>   albums;

    std::unordered_map<std::int64_t, std::vector<library::Track>> tracks;

    std::string                   search_query;
    std::vector<library::Track>   search_results;
    bool                          search_valid  = false;

    library::ScanState            last_state    = library::ScanState::Idle;

    void invalidate() {
        artists_valid = false;
        albums_artist.clear();
        tracks.clear();
        search_valid  = false;
    }
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static LibraryCache g_lib_cache;

// Returns a stable colour for a given album id, used for the placeholder swatch.
static inline ImU32 album_placeholder_color(std::int64_t id) {
    // Spread the id through a simple hash to get varied hues.
    const auto h = static_cast<uint32_t>(static_cast<uint64_t>(id) * 2654435761ULL);
    const uint8_t r = 80u  + static_cast<uint8_t>((h & 0x3Fu) * 2u);
    const uint8_t g = 60u  + static_cast<uint8_t>(((h >> 8u)  & 0x3Fu) * 2u);
    const uint8_t b = 100u + static_cast<uint8_t>(((h >> 16u) & 0x3Fu) * 2u);
    return IM_COL32(r, g, b, 255);
}

// Draw a thumbnail for the album at the current cursor position.
// Advances the cursor by kThumbSize + a small gap on the Y axis.
void draw_album_thumb(std::int64_t album_id, const std::string& album_title,
                      const std::filesystem::path& first_track_dir) {
    // Probe/load on first encounter.
    if (g_art_cache.find(album_id) == g_art_cache.end()) {
        g_art_cache[album_id] = load_album_art(first_track_dir);
    }

    const AlbumArt& art = g_art_cache[album_id];
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (art) {
        // Image draw: UV (0,0)→(1,1) maps full image to the square.
        dl->AddImage(art.texture_id(), pos,
                     ImVec2(pos.x + kThumbSize, pos.y + kThumbSize),
                     ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    } else {
        // Placeholder: muted colour square with the first letter of the title.
        const ImU32 col = album_placeholder_color(album_id);
        dl->AddRectFilled(pos, ImVec2(pos.x + kThumbSize, pos.y + kThumbSize), col, 4.0f);
        if (!album_title.empty()) {
            char initial[2] = { album_title[0], '\0' };
            const ImVec2 tsz = ImGui::CalcTextSize(initial);
            dl->AddText(ImVec2(pos.x + (kThumbSize - tsz.x) * 0.5f,
                               pos.y + (kThumbSize - tsz.y) * 0.5f),
                        IM_COL32(220, 220, 230, 255), initial);
        }
    }

    // Reserve vertical space so ImGui layout flows below the thumbnail.
    ImGui::Dummy(ImVec2(kThumbSize, kThumbSize));
    ImGui::Spacing();
}

const char* scan_state_name(library::ScanState s) {
    switch (s) {
    case library::ScanState::Idle:     return "idle";
    case library::ScanState::Scanning: return "scanning";
    case library::ScanState::Indexing: return "indexing";
    case library::ScanState::Cleaning: return "cleaning";
    }
    return "?";
}

void play_track(AppState& st, const library::Track& t) {
    if (st.engine_ == nullptr) {
        st.push_log("cannot play: no engine");
        return;
    }
    st.queue_set_single(t.path);
    auto r = st.engine_->load(t.path);
    if (!r) {
        st.push_log(std::string{"load failed: "} + r.error().message);
        return;
    }
    auto p = st.engine_->play();
    if (!p) {
        st.push_log(std::string{"play failed: "} + p.error().message);
    } else {
        st.push_log(std::string{"playing: "} + t.path.string());
    }
}

void play_album(AppState& st, const std::vector<library::Track>& album_tracks,
                std::size_t idx) {
    if (st.engine_ == nullptr) {
        st.push_log("cannot play: no engine");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(st.queue_mtx);
        st.queue.clear();
        for (const auto& t : album_tracks) {
            st.queue.push_back(t.path);
        }
        st.queue_index = static_cast<std::int32_t>(idx);
    }
    st.recompute_queue_duration();
    auto r = st.engine_->load(album_tracks[idx].path);
    if (!r) {
        st.push_log(std::string{"load failed: "} + r.error().message);
        return;
    }
    auto p = st.engine_->play();
    if (!p) {
        st.push_log(std::string{"play failed: "} + p.error().message);
    } else {
        st.push_log(std::string{"playing: "} + album_tracks[idx].path.string());
    }
}

} // namespace

void draw_library_view(AppState& st) {
    if (st.library_ == nullptr || st.cfg.library.roots.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("no library configured");
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "Add a [library].roots path to your config:");
        ImGui::Spacing();
        ImGui::Indent(16.0f);
        ImGui::TextColored(kMuted, "~/.config/transporter/config.toml");
        ImGui::Spacing();
        ImGui::TextColored(kMuted, "  [library]");
        ImGui::TextColored(kMuted, "  roots = [\"/home/you/Music\"]");
        ImGui::Unindent(16.0f);
        return;
    }

    // Invalidate caches when a background scan completes (non-Idle → Idle transition).
    {
        const auto prog = st.library_->progress();
        if (g_lib_cache.last_state != library::ScanState::Idle &&
            prog.state == library::ScanState::Idle) {
            g_lib_cache.invalidate();
        }
        g_lib_cache.last_state = prog.state;
    }

    // Search bar
    char buf[256]{};
    if (st.library_query.size() < sizeof(buf)) {
        std::memcpy(buf, st.library_query.c_str(), st.library_query.size());
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##search", "search title / artist / album",
                                 buf, sizeof(buf))) {
        st.library_query = buf;
    }

    // Two-pane child layout
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float left_w = std::max(220.0f, avail.x * 0.28f);

    if (!st.library_query.empty()) {
        // Search-mode: single full-width result list.
        // Re-query only when the search string changes.
        if (st.library_query != g_lib_cache.search_query || !g_lib_cache.search_valid) {
            library::SearchFilter f;
            f.query = st.library_query;
            f.limit = 200;
            if (auto r = st.library_->search(f); r) {
                g_lib_cache.search_results = std::move(*r);
                g_lib_cache.search_valid   = true;
            } else {
                g_lib_cache.search_valid = false;
                ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "search failed: %s",
                                   r.error().message.c_str());
            }
            g_lib_cache.search_query = st.library_query;
        }
        if (g_lib_cache.search_valid) {
            const auto& results = g_lib_cache.search_results;
            ImGui::TextColored(kMuted, "%zu match(es)", results.size());
            ImGui::BeginChild("results", ImVec2(0, avail.y - 60),
                              ImGuiChildFlags_Border);
            for (const auto& t : results) {
                ImGui::PushID(static_cast<int>(t.id));
                std::string label = t.artist + " - " + t.title +
                                    " [" + t.album + "]";
                if (ImGui::Selectable(label.c_str())) {
                    play_track(st, t);
                }
                if (t.duration.count() > 0) {
                    const std::int64_t s = t.duration.count() / 1000;
                    char dur[20];
                    std::snprintf(dur, sizeof(dur), "%lld:%02lld",
                                  static_cast<long long>(s / 60),
                                  static_cast<long long>(s % 60));
                    const float dur_w = ImGui::CalcTextSize(dur).x + 4.0f;
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - dur_w);
                    ImGui::TextColored(kMuted, "%s", dur);
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Play now")) {
                        play_track(st, t);
                    }
                    if (ImGui::MenuItem("Play next")) {
                        st.queue_insert_next(t.path);
                    }
                    if (ImGui::MenuItem("Add to queue")) {
                        st.queue_append(t.path);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    } else {
        // Browse-mode: artists pane on the left, albums + tracks on the right.

        // Artists: fetch once per session (or after rescan).
        if (!g_lib_cache.artists_valid) {
            if (auto a = st.library_->artists(); a) {
                g_lib_cache.artists       = std::move(*a);
                g_lib_cache.artists_valid = true;
            }
        }

        ImGui::BeginChild("artists", ImVec2(left_w, avail.y - 60),
                          ImGuiChildFlags_Border);
        if (g_lib_cache.artists_valid && !g_lib_cache.artists.empty()) {
            for (const auto& art : g_lib_cache.artists) {
                ImGui::PushID(static_cast<int>(art.id));
                const bool sel = (art.name == st.selected_artist);
                if (ImGui::Selectable(art.name.c_str(), sel)) {
                    st.selected_artist = art.name;
                    st.selected_album_id = 0;
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Play all")) {
                        library::SearchFilter f;
                        f.artist = art.name;
                        f.sort_by = library::SortBy::Album;
                        if (auto tracks = st.library_->search(f); tracks && !tracks->empty()) {
                            play_album(st, *tracks, 0);
                        }
                    }
                    if (ImGui::MenuItem("Add all to queue")) {
                        library::SearchFilter f;
                        f.artist = art.name;
                        f.sort_by = library::SortBy::Album;
                        if (auto tracks = st.library_->search(f); tracks) {
                            for (const auto& t : *tracks) {
                                st.queue_append(t.path);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "(%lld alb · %lld)",
                                   static_cast<long long>(art.album_count),
                                   static_cast<long long>(art.track_count));
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored(kMuted, "(no artists)");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Albums: fetch when selected artist changes.
        if (st.selected_artist != g_lib_cache.albums_artist) {
            if (auto albs = st.library_->albums(st.selected_artist); albs) {
                g_lib_cache.albums        = std::move(*albs);
            } else {
                g_lib_cache.albums.clear();
            }
            g_lib_cache.albums_artist = st.selected_artist;
        }

        ImGui::BeginChild("right", ImVec2(0, avail.y - 60));
        if (!st.selected_artist.empty()) {
            for (const auto& al : g_lib_cache.albums) {
                ImGui::PushID(static_cast<int>(al.id));
                const bool sel = (al.id == st.selected_album_id);
                char hdr[128];
                std::snprintf(hdr, sizeof(hdr), "%s  (%lld track%s)%s",
                              al.title.c_str(),
                              static_cast<long long>(al.track_count),
                              al.track_count == 1 ? "" : "s",
                              al.date.empty() ? "" :
                                  (std::string{"  "} + al.date).c_str());

                // Tracks fetched once per album_id, cached until next rescan.
                auto it_tr = g_lib_cache.tracks.find(al.id);
                if (it_tr == g_lib_cache.tracks.end()) {
                    if (auto r = st.library_->tracks_in_album(al.id); r) {
                        it_tr = g_lib_cache.tracks.emplace(al.id, std::move(*r)).first;
                    }
                }
                const std::vector<library::Track>* tracks =
                    (it_tr != g_lib_cache.tracks.end()) ? &it_tr->second : nullptr;

                char alb_dur_str[32] = {};
                if (tracks && !tracks->empty()) {
                    std::chrono::milliseconds album_dur{0};
                    for (const auto& tr : *tracks) album_dur += tr.duration;
                    if (album_dur.count() > 0) {
                        const std::int64_t s = album_dur.count() / 1000;
                        const std::int64_t m = s / 60;
                        if (m >= 60) std::snprintf(alb_dur_str, sizeof(alb_dur_str), "  %lld:%02lld:%02lld", static_cast<long long>(m/60), static_cast<long long>(m%60), static_cast<long long>(s%60));
                        else         std::snprintf(alb_dur_str, sizeof(alb_dur_str), "  %lld:%02lld", static_cast<long long>(m), static_cast<long long>(s%60));
                    }
                }

                char hdr2[192];
                std::snprintf(hdr2, sizeof(hdr2), "%s%s", hdr, alb_dur_str);

                ImGui::SetNextItemOpen(sel, ImGuiCond_Always);
                if (ImGui::CollapsingHeader(hdr2)) {
                    if (al.id != st.selected_album_id) {
                        st.selected_album_id = al.id;
                    }
                    if (tracks) {
                        if (!tracks->empty()) {
                            draw_album_thumb(al.id, al.title,
                                             (*tracks)[0].path.parent_path());
                        }
                        for (std::size_t ti = 0; ti < tracks->size(); ++ti) {
                            const auto& tr = (*tracks)[ti];
                            char row[256];
                            std::snprintf(row, sizeof(row), "%s. %s",
                                          tr.track_no.empty() ? "-" : tr.track_no.c_str(),
                                          tr.title.c_str());
                            if (ImGui::Selectable(row, false,
                                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                                if (ImGui::IsMouseDoubleClicked(0)) {
                                    play_album(st, *tracks, ti);
                                }
                            }
                            if (ImGui::IsItemHovered() && !tr.codec.empty()) {
                                ImGui::SetTooltip("%s  %u Hz  %u-bit",
                                                  tr.codec.c_str(),
                                                  tr.sample_rate_hz,
                                                  static_cast<unsigned>(tr.bit_depth));
                            }
                            if (tr.duration.count() > 0) {
                                const std::int64_t s = tr.duration.count() / 1000;
                                char dur[20];
                                std::snprintf(dur, sizeof(dur), "%lld:%02lld",
                                              static_cast<long long>(s / 60),
                                              static_cast<long long>(s % 60));
                                const float dur_w = ImGui::CalcTextSize(dur).x + 4.0f;
                                ImGui::SameLine();
                                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - dur_w);
                                ImGui::TextColored(kMuted, "%s", dur);
                            }
                            if (ImGui::BeginPopupContextItem()) {
                                if (ImGui::MenuItem("Play now")) {
                                    play_album(st, *tracks, ti);
                                }
                                if (ImGui::MenuItem("Play next")) {
                                    st.queue_insert_next((*tracks)[ti].path);
                                }
                                if (ImGui::MenuItem("Add to queue")) {
                                    st.queue_append((*tracks)[ti].path);
                                }
                                ImGui::EndPopup();
                            }
                        }
                    }
                }
                if (ImGui::BeginPopupContextItem("##album_ctx")) {
                    if (ImGui::MenuItem("Play album")) {
                        if (tracks && !tracks->empty()) {
                            play_album(st, *tracks, 0);
                        }
                    }
                    if (ImGui::MenuItem("Add album to queue")) {
                        if (tracks) {
                            for (const auto& t : *tracks) {
                                st.queue_append(t.path);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored(kMuted, "Select an artist on the left.");
        }
        ImGui::EndChild();
    }

    // Status line
    ImGui::Separator();
    {
        const auto p = st.library_->progress();
        if (p.state == library::ScanState::Idle) {
            ImGui::TextColored(kMuted, "idle  %zu tracks", p.files_indexed);
        } else {
            char buf2[256];
            const char* fn = p.current_path.empty()
                ? ""
                : p.current_path.filename().c_str();
            std::snprintf(buf2, sizeof(buf2), "%s  %zu seen  %zu indexed  %s",
                          scan_state_name(p.state),
                          p.files_seen, p.files_indexed, fn);
            ImGui::TextColored(kMuted, "%s", buf2);
        }
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) {
        st.library_->rescan_async();
    }
}

} // namespace transporter::gui
