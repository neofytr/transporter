// SPDX-License-Identifier: GPL-3.0-or-later
//
// org.mpris.MediaPlayer2.TrackList — subset implementation. AddTrack /
// RemoveTrack / GoTo are intentionally omitted (queue management is the
// caller's job). CanEditTracks is exposed and reads false.

#include "mpris_tracklist.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace transporter::dbus_svc {

namespace {

constexpr const char* kIface = "org.mpris.MediaPlayer2.TrackList";

} // namespace

MprisTrackList::MprisTrackList(sdbus::IObject& obj,
                               IdProvider ids,
                               MetaProvider meta)
    : obj_(obj), ids_(std::move(ids)), meta_(std::move(meta)) {
    register_vtable();
}

void MprisTrackList::emit_replaced(const std::vector<sdbus::ObjectPath>& ids,
                                   const sdbus::ObjectPath& current) {
    try {
        obj_.emitSignal("TrackListReplaced")
            .onInterface(kIface)
            .withArguments(ids, current);
        obj_.emitPropertiesChangedSignal(
            kIface, {sdbus::PropertyName{"Tracks"}});
    } catch (const sdbus::Error&) {
        // ignore: connection going away
    }
}

void MprisTrackList::register_vtable() {
    obj_.addVTable(
        sdbus::registerMethod("GetTracksMetadata")
            .withInputParamNames("TrackIds")
            .withOutputParamNames("Metadata")
            .implementedAs(
                [this](const std::vector<sdbus::ObjectPath>& ids)
                    -> std::vector<std::map<std::string, sdbus::Variant>> {
                    std::vector<std::map<std::string, sdbus::Variant>> out;
                    out.reserve(ids.size());
                    for (const auto& id : ids) {
                        if (meta_) {
                            out.emplace_back(meta_(id));
                        } else {
                            out.emplace_back();
                        }
                    }
                    return out;
                }),
        sdbus::registerSignal("TrackListReplaced")
            .withParameters<std::vector<sdbus::ObjectPath>, sdbus::ObjectPath>(
                {"Tracks", "CurrentTrack"}),
        sdbus::registerProperty("Tracks").withGetter(
            [this] { return ids_ ? ids_() : std::vector<sdbus::ObjectPath>{}; }),
        sdbus::registerProperty("CanEditTracks").withGetter(
            [] { return false; })
    ).forInterface(sdbus::InterfaceName{kIface});
}

} // namespace transporter::dbus_svc
