// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_MPRIS_ROOT_HPP
#define TRANSPORTER_SRC_DBUS_MPRIS_ROOT_HPP

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <functional>
#include <memory>

namespace transporter::dbus_svc {

// Adaptor for org.mpris.MediaPlayer2 at /org/mpris/MediaPlayer2.
// Owns a vtable on the shared IObject; the parent service shares one IObject
// across all MPRIS interfaces (root, Player, TrackList) since they all sit at
// the same object path.
class MprisRoot {
public:
    MprisRoot(sdbus::IObject& obj, std::function<void()> quit_cb);

    // Non-copy, non-move. Lifetime tied to the owning IObject.
    MprisRoot(const MprisRoot&) = delete;
    MprisRoot& operator=(const MprisRoot&) = delete;

private:
    void register_vtable();

    sdbus::IObject& obj_;
    std::function<void()> quit_cb_;
};

} // namespace transporter::dbus_svc

#endif
