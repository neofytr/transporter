// SPDX-License-Identifier: GPL-3.0-or-later
//
// Library browse view. Two-pane: artists left, albums + tracks right.
// Search box at top binds to Library::search.

#include "app.hpp"

#include <transporter/library/library.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace transporter::gui {

namespace {

constexpr ImVec4 kMuted{0.65f, 0.65f, 0.70f, 1.0f};

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
    if (st.library_ == nullptr) {
        ImGui::TextDisabled("library not configured");
        ImGui::TextColored(kMuted,
                           "Add a [library].roots entry in your config and restart.");
        return;
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
        // Search-mode: single full-width result list
        library::SearchFilter f;
        f.query = st.library_query;
        f.limit = 200;
        if (auto r = st.library_->search(f); r) {
            ImGui::TextColored(kMuted, "%zu match(es)", r->size());
            ImGui::BeginChild("results", ImVec2(0, avail.y - 60),
                              ImGuiChildFlags_Border);
            for (const auto& t : *r) {
                std::string label = t.artist + " - " + t.title +
                                    " [" + t.album + "]";
                if (ImGui::Selectable(label.c_str())) {
                    play_track(st, t);
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem("Play now")) {
                        play_track(st, t);
                    }
                    if (ImGui::MenuItem("Add to queue")) {
                        st.queue_append(t.path);
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
        } else {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "search failed: %s",
                               r.error().message.c_str());
        }
    } else {
        // Browse-mode: artists pane on the left, albums + tracks on the right
        ImGui::BeginChild("artists", ImVec2(left_w, avail.y - 60),
                          ImGuiChildFlags_Border);
        if (auto a = st.library_->artists(); a) {
            for (const auto& art : *a) {
                const bool sel = (art.name == st.selected_artist);
                if (ImGui::Selectable(art.name.c_str(), sel)) {
                    st.selected_artist = art.name;
                    st.selected_album_id = 0;
                }
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "(%lld)",
                                   static_cast<long long>(art.track_count));
            }
        } else {
            ImGui::TextColored(kMuted, "(no artists)");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("right", ImVec2(0, avail.y - 60));
        if (!st.selected_artist.empty()) {
            if (auto albs = st.library_->albums(st.selected_artist); albs) {
                for (const auto& al : *albs) {
                    const bool sel = (al.id == st.selected_album_id);
                    char hdr[128];
                    std::snprintf(hdr, sizeof(hdr), "%s  (%lld track%s)%s",
                                  al.title.c_str(),
                                  static_cast<long long>(al.track_count),
                                  al.track_count == 1 ? "" : "s",
                                  al.date.empty() ? "" :
                                      (std::string{"  "} + al.date).c_str());
                    ImGui::SetNextItemOpen(sel, ImGuiCond_Always);
                    if (ImGui::CollapsingHeader(hdr)) {
                        if (al.id != st.selected_album_id) {
                            st.selected_album_id = al.id;
                        }
                        if (auto tracks = st.library_->tracks_in_album(al.id); tracks) {
                            for (std::size_t ti = 0; ti < tracks->size(); ++ti) {
                                const auto& t = (*tracks)[ti];
                                char row[256];
                                std::snprintf(row, sizeof(row), "%s. %s  [%u Hz / %u-bit]",
                                              t.track_no.empty() ? "-" : t.track_no.c_str(),
                                              t.title.c_str(),
                                              t.sample_rate_hz, t.bit_depth);
                                if (ImGui::Selectable(row, false,
                                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                                    if (ImGui::IsMouseDoubleClicked(0)) {
                                        play_album(st, *tracks, ti);
                                    }
                                }
                                if (ImGui::BeginPopupContextItem()) {
                                    if (ImGui::MenuItem("Play now")) {
                                        play_album(st, *tracks, ti);
                                    }
                                    if (ImGui::MenuItem("Add to queue")) {
                                        st.queue_append((*tracks)[ti].path);
                                    }
                                    ImGui::EndPopup();
                                }
                            }
                        }
                    }
                    if (ImGui::BeginPopupContextItem(hdr)) {
                        if (ImGui::MenuItem("Play album")) {
                            if (auto tracks = st.library_->tracks_in_album(al.id); tracks && !tracks->empty()) {
                                play_album(st, *tracks, 0);
                            }
                        }
                        if (ImGui::MenuItem("Add album to queue")) {
                            if (auto tracks = st.library_->tracks_in_album(al.id); tracks) {
                                for (const auto& t : *tracks) {
                                    st.queue_append(t.path);
                                }
                            }
                        }
                        ImGui::EndPopup();
                    }
                }
            }
        } else {
            ImGui::TextColored(kMuted, "Select an artist on the left.");
        }
        ImGui::EndChild();
    }

    // Status line
    ImGui::Separator();
    const auto p = st.library_->progress();
    if (p.state == library::ScanState::Idle && st.library_status.empty()) {
        ImGui::TextColored(kMuted,
                           "library: idle  -  add roots to "
                           "~/.config/transporter/config.toml and restart");
    } else {
        char buf2[128];
        std::snprintf(buf2, sizeof(buf2),
                      "%s... seen=%zu indexed=%zu deleted=%zu",
                      scan_state_name(p.state), p.files_seen,
                      p.files_indexed, p.files_deleted);
        ImGui::TextColored(kMuted, "%s", buf2);
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) {
        st.library_->rescan_async();
    }
}

} // namespace transporter::gui
