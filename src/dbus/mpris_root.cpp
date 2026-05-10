// SPDX-License-Identifier: GPL-3.0-or-later
//
// org.mpris.MediaPlayer2 — root interface. Identity, capability flags, and
// the Quit / Raise methods. Raise is a no-op (Wayland: window managers, not
// the player, decide stacking).

#include "mpris_root.hpp"

#include <sdbus-c++/sdbus-c++.h>

#include <string>
#include <utility>
#include <vector>

namespace transporter::dbus_svc {

namespace {

constexpr const char* kIface = "org.mpris.MediaPlayer2";

const std::vector<std::string>& supported_uri_schemes() {
    static const std::vector<std::string> v{"file"};
    return v;
}

const std::vector<std::string>& supported_mime_types() {
    static const std::vector<std::string> v{
        "audio/flac",
        "audio/x-flac",
        "audio/mpeg",
        "audio/mp4",
        "audio/x-vorbis+ogg",
        "audio/x-opus+ogg",
        "audio/wav",
        "audio/x-wav",
        "audio/aiff",
        "audio/x-aiff",
    };
    return v;
}

} // namespace

MprisRoot::MprisRoot(sdbus::IObject& obj, std::function<void()> quit_cb)
    : obj_(obj), quit_cb_(std::move(quit_cb)) {
    register_vtable();
}

void MprisRoot::register_vtable() {
    obj_.addVTable(
        // Methods
        sdbus::registerMethod("Raise").implementedAs([] { /* no-op */ }),
        sdbus::registerMethod("Quit").implementedAs([this] {
            if (quit_cb_) {
                quit_cb_();
            }
        }),
        // Properties (all read-only).
        sdbus::registerProperty("CanQuit").withGetter([] { return true; }),
        sdbus::registerProperty("CanRaise").withGetter([] { return false; }),
        sdbus::registerProperty("HasTrackList").withGetter([] { return true; }),
        sdbus::registerProperty("Identity")
            .withGetter([] { return std::string{"transporter"}; }),
        sdbus::registerProperty("DesktopEntry")
            .withGetter([] { return std::string{"transporter"}; }),
        sdbus::registerProperty("SupportedUriSchemes")
            .withGetter([] { return supported_uri_schemes(); }),
        sdbus::registerProperty("SupportedMimeTypes")
            .withGetter([] { return supported_mime_types(); })
    ).forInterface(sdbus::InterfaceName{kIface});
}

} // namespace transporter::dbus_svc
