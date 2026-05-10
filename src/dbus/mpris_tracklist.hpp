// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_MPRIS_TRACKLIST_HPP
#define TRANSPORTER_SRC_DBUS_MPRIS_TRACKLIST_HPP

#include <sdbus-c++/sdbus-c++.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace transporter::dbus_svc {

// Subset of org.mpris.MediaPlayer2.TrackList.
//   Tracks (property)               - object-path array of currently known tracks
//   GetTracksMetadata (method)      - returns a metadata map per requested id
//   TrackListReplaced (signal)      - emitted on queue replacement
//
// The tracklist is sourced from an in-process queue (gui::AppState). Phase 10
// keeps the queue simple: a single track when a file is on the command line
// or loaded via OpenUri, and the GUI pushes additional entries when the user
// queues from the library view in a later phase. Always at least the
// currently-playing track is reported; an empty queue yields an empty array.
class MprisTrackList {
public:
    using IdProvider = std::function<std::vector<sdbus::ObjectPath>()>;
    using MetaProvider =
        std::function<std::map<std::string, sdbus::Variant>(const sdbus::ObjectPath&)>;

    MprisTrackList(sdbus::IObject& obj,
                   IdProvider ids,
                   MetaProvider meta);

    MprisTrackList(const MprisTrackList&) = delete;
    MprisTrackList& operator=(const MprisTrackList&) = delete;

    // Emit TrackListReplaced({ids}, current). The signal arguments are the
    // new track ids and the id that should be considered "current". A
    // subsequent property-changed for Tracks is emitted as well.
    void emit_replaced(const std::vector<sdbus::ObjectPath>& ids,
                       const sdbus::ObjectPath& current);

private:
    void register_vtable();

    sdbus::IObject& obj_;
    IdProvider ids_;
    MetaProvider meta_;
};

} // namespace transporter::dbus_svc

#endif
