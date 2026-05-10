// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRANSPORTER_SRC_DBUS_TRANSPORTER_IFACE_HPP
#define TRANSPORTER_SRC_DBUS_TRANSPORTER_IFACE_HPP

#include <transporter/engine/engine.hpp>
#include <transporter/library/library.hpp>

#include <sdbus-c++/sdbus-c++.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace transporter::dbus_svc {

// Adaptor for org.mpris.MediaPlayer2.transporter at
// /org/mpris/MediaPlayer2/transporter. Exposes the bit-perfect verdict, the
// device fingerprint + capabilities, the RT mode, the ring fill, plus the
// flattened pipeline snapshot via GetSnapshot(). Methods ReloadConfig,
// RescanLibrary, SelectDevice are also published.
class TransporterIface {
public:
    using ReloadHook = std::function<bool()>;          // returns true on apply
    using RescanHook = std::function<void()>;

    TransporterIface(sdbus::IObject& obj,
                     transporter::engine::Engine* engine,
                     transporter::library::Library* library,
                     ReloadHook reload,
                     RescanHook rescan);

    TransporterIface(const TransporterIface&) = delete;
    TransporterIface& operator=(const TransporterIface&) = delete;

    // Emit BitPerfectChanged when the level transitioned.
    void emit_bitperfect_changed(const std::string& level,
                                 const std::vector<std::string>& qualifications);

private:
    void register_vtable();

    sdbus::IObject& obj_;
    transporter::engine::Engine* engine_;
    transporter::library::Library* library_;
    ReloadHook reload_;
    RescanHook rescan_;
};

} // namespace transporter::dbus_svc

#endif
