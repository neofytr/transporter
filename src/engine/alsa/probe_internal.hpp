// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-internal capability probe shared by alsa::probe (used by the
// playback path) and device::probe (used by enumeration). Lives behind
// src/ — not a public header.

#ifndef TRANSPORTER_ENGINE_ALSA_PROBE_INTERNAL_HPP
#define TRANSPORTER_ENGINE_ALSA_PROBE_INTERNAL_HPP

#include <transporter/engine/alsa_output.hpp>
#include <transporter/engine/error.hpp>
#include <transporter/engine/format.hpp>

#include <alsa/asoundlib.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>

namespace transporter::engine::alsa::detail {

// Project's supported sample formats, in canonical order. Probes iterate
// this list with snd_pcm_hw_params_test_format.
inline constexpr std::array<SampleFormat, 5> ALL_FORMATS = {
    SampleFormat::S16_LE, SampleFormat::S24_LE, SampleFormat::S24_3LE,
    SampleFormat::S32_LE, SampleFormat::FLOAT_LE,
};

// Anchor rate list. Tested with snd_pcm_hw_params_test_rate exactly. The
// device's reported min/max via get_rate_min/max may extend this set; the
// device probe layers on those rates when they fall outside the anchor.
inline constexpr std::array<std::uint32_t, 14> ANCHOR_RATES = {
    8000,   11025,  16000,  22050,  32000,  44100,  48000,
    88200,  96000,  176400, 192000, 352800, 384000, 705600,
};

// Translate project format enum to ALSA's; SND_PCM_FORMAT_UNKNOWN on miss
// (no project format currently maps to UNKNOWN, so a miss is internal bug).
snd_pcm_format_t to_alsa_format(SampleFormat f) noexcept;

// Run the format/rate/channel probe against an open snd_pcm_t. The PCM may
// be opened in blocking or nonblocking mode; this routine does not call
// snd_pcm_open or snd_pcm_close itself.
//
// Caller pre-allocates DeviceCapsStorage; we fill it. Errors here are
// "params query failed" — open/EBUSY classification is the caller's job.
std::expected<void, Error> probe_open_pcm(snd_pcm_t* pcm,
                                          DeviceCapsStorage& caps);

} // namespace transporter::engine::alsa::detail

#endif
