// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-device capability probe. Uses the shared format/rate/channel probe
// in src/engine/alsa/output.cpp (alsa::detail::probe_open_pcm). Adds:
//   - rates outside the anchor set that the device reports via
//     get_rate_min / get_rate_max
//   - HW volume control via the simple-mixer API
//   - -EBUSY classification (caps_probe_failed=true, GUI shows "in use")

#include <transporter/engine/device.hpp>

#include "../alsa/probe_internal.hpp"
#include "device_internal.hpp"

#include <transporter/engine/alsa_output.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace transporter::engine::detail {

namespace {

struct PcmCloser {
    void operator()(snd_pcm_t* p) const noexcept {
        if (p) {
            snd_pcm_close(p);
        }
    }
};
using PcmHandle = std::unique_ptr<snd_pcm_t, PcmCloser>;

struct HwParamsFreer {
    void operator()(snd_pcm_hw_params_t* p) const noexcept {
        if (p) {
            snd_pcm_hw_params_free(p);
        }
    }
};
using HwParams = std::unique_ptr<snd_pcm_hw_params_t, HwParamsFreer>;

struct MixerCloser {
    void operator()(snd_mixer_t* m) const noexcept {
        if (m) {
            snd_mixer_close(m);
        }
    }
};
using MixerHandle = std::unique_ptr<snd_mixer_t, MixerCloser>;

HwParams make_hw_params() {
    snd_pcm_hw_params_t* raw = nullptr;
    if (snd_pcm_hw_params_malloc(&raw) < 0) {
        return HwParams{};
    }
    return HwParams{raw};
}

// Volume-element preference. Matches what most audiophile setups expect:
// HW playback element on USB DACs is typically "PCM" or "Master"; HDA
// codecs expose both Master and PCM, in which case Master is the canonical
// post-mix tap.
constexpr std::array<const char*, 6> VOLUME_ELEMENT_PREFERENCE = {
    "Master", "Speaker", "PCM", "Headphone", "Front", "Digital",
};

// Returns the simple-mixer element matching one of our preferred names.
// Loops VOLUME_ELEMENT_PREFERENCE in order; first one with playback volume
// wins.
snd_mixer_elem_t* find_volume_element(snd_mixer_t* mix) noexcept {
    for (const char* wanted : VOLUME_ELEMENT_PREFERENCE) {
        snd_mixer_selem_id_t* sid = nullptr;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, wanted);
        snd_mixer_elem_t* elem = snd_mixer_find_selem(mix, sid);
        if (elem != nullptr && snd_mixer_selem_has_playback_volume(elem) != 0) {
            return elem;
        }
    }
    return nullptr;
}

// ALSA card index -> "hw:N" for the simple-mixer attach. The simple mixer
// requires the card-level control device, not the PCM hw string.
std::string ctl_string_for_card(int card_index) {
    std::string s = "hw:";
    s += std::to_string(card_index);
    return s;
}

void probe_hw_volume(int card_index, VolumeControl& vc) {
    snd_mixer_t* raw = nullptr;
    if (snd_mixer_open(&raw, 0) < 0) {
        return;
    }
    MixerHandle mix{raw};

    const std::string ctl = ctl_string_for_card(card_index);
    if (snd_mixer_attach(mix.get(), ctl.c_str()) < 0) {
        return;
    }
    if (snd_mixer_selem_register(mix.get(), nullptr, nullptr) < 0) {
        return;
    }
    if (snd_mixer_load(mix.get()) < 0) {
        return;
    }

    snd_mixer_elem_t* elem = find_volume_element(mix.get());
    if (elem == nullptr) {
        return;
    }

    vc.present = true;
    snd_mixer_selem_id_t* sid = nullptr;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_get_id(elem, sid);
    const char* nm = snd_mixer_selem_id_get_name(sid);
    if (nm != nullptr) {
        vc.element_name = nm;
    }

    long min_db = 0;
    long max_db = 0;
    if (snd_mixer_selem_get_playback_dB_range(elem, &min_db, &max_db) == 0) {
        vc.min_db_x100 = min_db;
        vc.max_db_x100 = max_db;
        vc.has_db_scale = true;
    }
}

// Probe rate min/max independently of the anchor set. Some DACs accept
// "weird" rates (e.g., 64000) that aren't in the anchor list; if min or max
// falls outside our anchors and snd_pcm_hw_params_test_rate accepts it,
// add it. We never fall back to _near or trust get_rate_max blindly.
void merge_extra_rates(snd_pcm_t* pcm, snd_pcm_hw_params_t* hwp,
                       std::vector<std::uint32_t>& rates) {
    unsigned int rate_min = 0;
    unsigned int rate_max = 0;
    int dir = 0;
    if (snd_pcm_hw_params_get_rate_min(hwp, &rate_min, &dir) < 0) {
        return;
    }
    dir = 0;
    if (snd_pcm_hw_params_get_rate_max(hwp, &rate_max, &dir) < 0) {
        return;
    }

    auto try_add = [&](unsigned int r) {
        if (r == 0) {
            return;
        }
        if (std::find(rates.begin(), rates.end(),
                      static_cast<std::uint32_t>(r)) != rates.end()) {
            return;
        }
        if (snd_pcm_hw_params_test_rate(pcm, hwp, r, 0) == 0) {
            rates.push_back(static_cast<std::uint32_t>(r));
        }
    };
    try_add(rate_min);
    try_add(rate_max);

    std::sort(rates.begin(), rates.end());
}

} // namespace

std::expected<ParsedHwString, Error> parse_hw_string(const std::string& hw_string) {
    // Accept "hw:CARD=<id>,DEV=<n>" or "hw:<n>,<m>".
    if (hw_string.rfind("hw:", 0) != 0) {
        return std::unexpected(Error{ErrorCode::DeviceOpenFailed,
                                     "parse_hw_string: not a hw: string"});
    }
    const std::string body = hw_string.substr(3);

    auto comma = body.find(',');
    std::string left = comma == std::string::npos ? body : body.substr(0, comma);
    std::string right = comma == std::string::npos ? std::string{} : body.substr(comma + 1);

    int card_index = -1;
    int device_index = 0;

    const std::string card_prefix = "CARD=";
    if (left.rfind(card_prefix, 0) == 0) {
        const std::string card_id = left.substr(card_prefix.size());
        const int rc = snd_card_get_index(card_id.c_str());
        if (rc < 0) {
            return std::unexpected(Error{ErrorCode::DeviceOpenFailed,
                                         "snd_card_get_index(" + card_id + ") failed"});
        }
        card_index = rc;
    } else {
        try {
            card_index = std::stoi(left);
        } catch (...) {
            return std::unexpected(Error{ErrorCode::DeviceOpenFailed,
                                         "parse_hw_string: bad card index"});
        }
    }

    const std::string dev_prefix = "DEV=";
    if (right.rfind(dev_prefix, 0) == 0) {
        try {
            device_index = std::stoi(right.substr(dev_prefix.size()));
        } catch (...) {
            return std::unexpected(Error{ErrorCode::DeviceOpenFailed,
                                         "parse_hw_string: bad device index"});
        }
    } else if (!right.empty()) {
        try {
            device_index = std::stoi(right);
        } catch (...) {
            return std::unexpected(Error{ErrorCode::DeviceOpenFailed,
                                         "parse_hw_string: bad device index"});
        }
    }

    return ParsedHwString{card_index, device_index};
}

DeviceCapabilities probe_device(const std::string& alsa_hw_string) {
    DeviceCapabilities caps;

    snd_pcm_t* raw = nullptr;
    // SND_PCM_NONBLOCK avoids the open call hanging on busy USB DACs that
    // are slow to respond. We never call snd_pcm_writei against this handle;
    // it's purely for hw_params query.
    const int open_err = snd_pcm_open(&raw, alsa_hw_string.c_str(),
                                      SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (open_err < 0) {
        caps.caps_probe_failed = true;
        if (open_err == -EBUSY) {
            caps.probe_failure_reason = "device busy (in use by another app)";
        } else {
            caps.probe_failure_reason = std::string{"snd_pcm_open: "} +
                                        snd_strerror(open_err);
        }
        return caps;
    }
    PcmHandle pcm{raw};

    alsa::DeviceCapsStorage storage;
    auto rc = alsa::detail::probe_open_pcm(pcm.get(), storage);
    if (!rc) {
        caps.caps_probe_failed = true;
        caps.probe_failure_reason = rc.error().message;
        return caps;
    }

    caps.formats = std::move(storage.formats);
    caps.sample_rates = std::move(storage.rates);
    caps.channels_min = storage.min_channels;
    caps.channels_max = storage.max_channels;

    // Re-open hw_params for the merge-extra-rates pass — we need a fresh
    // hw_params populated with snd_pcm_hw_params_any.
    HwParams hwp = make_hw_params();
    if (hwp) {
        if (snd_pcm_hw_params_any(pcm.get(), hwp.get()) >= 0) {
            merge_extra_rates(pcm.get(), hwp.get(), caps.sample_rates);
        }
    }

    // Close the PCM before opening the mixer; some kernels serialize
    // control access per card. Our PcmHandle dtor handles this when we
    // leave scope, but explicit reset makes the order obvious.
    pcm.reset();

    // HW volume probe. Best-effort; absence is normal (HDMI playback paths,
    // USB DACs without a HW mixer, etc.).
    auto parsed = parse_hw_string(alsa_hw_string);
    if (parsed) {
        probe_hw_volume(parsed->card_index, caps.hw_volume);
    }

    return caps;
}

} // namespace transporter::engine::detail
