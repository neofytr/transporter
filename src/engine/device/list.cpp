// SPDX-License-Identifier: GPL-3.0-or-later
//
// Enumerate ALSA playback devices, fingerprint them, and probe capabilities.
// Public API in include/transporter/engine/device.hpp.

#include <transporter/engine/device.hpp>

#include "device_internal.hpp"

#include <alsa/asoundlib.h>

#include <cerrno>
#include <cstdio>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace transporter::engine {

namespace {

struct CtlCloser {
    void operator()(snd_ctl_t* p) const noexcept {
        if (p) {
            snd_ctl_close(p);
        }
    }
};
using CtlHandle = std::unique_ptr<snd_ctl_t, CtlCloser>;

std::unexpected<Error> alsa_err(ErrorCode code, const std::string& what, int err) {
    std::string msg = what;
    msg += ": ";
    msg += snd_strerror(err);
    return std::unexpected(Error{code, std::move(msg)});
}

std::string make_hw_string(const char* card_name, int device_index) {
    std::string s = "hw:CARD=";
    s += (card_name != nullptr ? card_name : "");
    s += ",DEV=";
    s += std::to_string(device_index);
    return s;
}

std::string make_ctl_name(int card_index) {
    std::string s = "hw:";
    s += std::to_string(card_index);
    return s;
}

// Walk PCM playback devices on one card. Skips devices that aren't playback.
std::expected<std::vector<DeviceInfo>, Error>
enumerate_card(int card_index) {
    const std::string ctl_name = make_ctl_name(card_index);
    snd_ctl_t* ctl_raw = nullptr;
    int rc = snd_ctl_open(&ctl_raw, ctl_name.c_str(), 0);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceOpenFailed,
                        "snd_ctl_open(" + ctl_name + ")", rc);
    }
    CtlHandle ctl{ctl_raw};

    snd_ctl_card_info_t* card_info = nullptr;
    snd_ctl_card_info_alloca(&card_info);
    rc = snd_ctl_card_info(ctl.get(), card_info);
    if (rc < 0) {
        return alsa_err(ErrorCode::DeviceOpenFailed, "snd_ctl_card_info", rc);
    }

    const char* card_name = snd_ctl_card_info_get_name(card_info);
    const char* card_longname = snd_ctl_card_info_get_longname(card_info);
    const char* card_id = snd_ctl_card_info_get_id(card_info);

    DeviceFingerprint base_fp;
    base_fp.alsa_card_name = card_name != nullptr ? card_name : "";
    base_fp.alsa_card_longname = card_longname != nullptr ? card_longname : "";
    detail::populate_usb_fingerprint(card_index, base_fp);

    snd_pcm_info_t* pcm_info = nullptr;
    snd_pcm_info_alloca(&pcm_info);

    std::vector<DeviceInfo> out;

    int device_index = -1;
    for (;;) {
        rc = snd_ctl_pcm_next_device(ctl.get(), &device_index);
        if (rc < 0) {
            return alsa_err(ErrorCode::DeviceOpenFailed,
                            "snd_ctl_pcm_next_device", rc);
        }
        if (device_index < 0) {
            break;
        }

        snd_pcm_info_set_device(pcm_info, static_cast<unsigned int>(device_index));
        snd_pcm_info_set_subdevice(pcm_info, 0);
        snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_PLAYBACK);
        rc = snd_ctl_pcm_info(ctl.get(), pcm_info);
        if (rc < 0) {
            // -ENOENT: device exists but not for playback. Skip silently.
            if (rc == -ENOENT) {
                continue;
            }
            return alsa_err(ErrorCode::DeviceOpenFailed, "snd_ctl_pcm_info", rc);
        }

        DeviceInfo di;
        di.alsa_card_index = card_index;
        di.alsa_device_index = device_index;
        // Prefer the stable card id (snd_ctl_card_info_get_id) over the
        // pretty name when building hw strings: the id is the kernel's
        // CARD= token and is always ASCII-safe.
        di.alsa_hw_string = make_hw_string(card_id, device_index);
        di.fingerprint = base_fp;
        di.id = make_stable_id(di.fingerprint);

        auto caps = detail::probe_device(di.alsa_hw_string);
        di.caps = std::move(caps);

        out.push_back(std::move(di));
    }

    return out;
}

} // namespace

std::expected<std::vector<DeviceInfo>, Error> list_playback_devices() {
    std::vector<DeviceInfo> all;
    int card = -1;
    for (;;) {
        const int rc = snd_card_next(&card);
        if (rc < 0) {
            return alsa_err(ErrorCode::DeviceOpenFailed, "snd_card_next", rc);
        }
        if (card < 0) {
            break;
        }
        auto card_devs = enumerate_card(card);
        if (!card_devs) {
            // Couldn't open the control device for this card. Skip it rather
            // than failing the whole enumeration; surfaced via the absent
            // entry. (The error path below is reserved for snd_card_next
            // itself failing.)
            continue;
        }
        for (auto& d : *card_devs) {
            all.push_back(std::move(d));
        }
    }
    return all;
}

std::expected<DeviceInfo, Error> describe_device(const std::string& alsa_hw_string) {
    auto parsed = detail::parse_hw_string(alsa_hw_string);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    auto all = list_playback_devices();
    if (!all) {
        return std::unexpected(all.error());
    }
    for (auto& d : *all) {
        if (d.alsa_card_index == parsed->card_index &&
            d.alsa_device_index == parsed->device_index) {
            return d;
        }
    }
    return std::unexpected(Error{ErrorCode::DeviceOpenFailed,
                                 "describe_device: " + alsa_hw_string + " not found"});
}

} // namespace transporter::engine
