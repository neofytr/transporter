// SPDX-License-Identifier: GPL-3.0-or-later

// Opus is internally always 48 kHz; libopusfile's op_pcm_total /
// op_read_float operate in 48 kHz units regardless of the original encoder
// input rate. We surface 48 kHz as the decoder's native rate and let the
// engine's format-match step refuse if the DAC will not take it. We do not
// resample, ever — exposure of 48 kHz here is the only honest answer.

#include "decoders.hpp"

#include <opus/opusfile.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace transporter::engine {

namespace {

class OpusDecoder final : public IDecoder {
public:
    static std::expected<std::unique_ptr<OpusDecoder>, Error>
    open(const std::filesystem::path& path);

    ~OpusDecoder() override {
        if (of_) {
            op_free(of_);
        }
    }

    PcmFormat format() const noexcept override { return fmt_; }
    std::uint64_t total_frames() const noexcept override { return total_frames_; }
    const Tags& tags() const noexcept override { return tags_; }

    std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) override;

    std::expected<void, Error> seek_frame(std::uint64_t frame) override;

private:
    OpusDecoder() = default;

    OggOpusFile* of_ = nullptr;
    PcmFormat fmt_{};
    Tags tags_{};
    std::uint64_t total_frames_ = 0;
};

void parse_opus_tags(const OpusTags* tags, Tags& out) {
    if (!tags) {
        return;
    }
    for (int i = 0; i < tags->comments; ++i) {
        const std::string_view s{tags->user_comments[i],
                                 static_cast<std::size_t>(tags->comment_lengths[i])};
        const auto eq = s.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        std::string keylc;
        keylc.reserve(eq);
        for (std::size_t j = 0; j < eq; ++j) {
            const char c = s[j];
            keylc.push_back(static_cast<char>(
                c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));
        }
        const std::string_view val = s.substr(eq + 1);
        if (keylc == "artist") {
            out.artist.assign(val);
        } else if (keylc == "albumartist" || keylc == "album_artist") {
            out.album_artist.assign(val);
        } else if (keylc == "album") {
            out.album.assign(val);
        } else if (keylc == "title") {
            out.title.assign(val);
        } else if (keylc == "tracknumber" || keylc == "track") {
            out.track_no.assign(val);
        } else if (keylc == "discnumber" || keylc == "disc") {
            out.disc_no.assign(val);
        } else if (keylc == "date" || keylc == "year") {
            out.date.assign(val);
        } else if (keylc == "replaygain_track_gain") {
            try { out.rg_track_gain = std::stof(std::string(val)); }
            catch (const std::exception&) {}
        } else if (keylc == "replaygain_track_peak") {
            try { out.rg_track_peak = std::stof(std::string(val)); }
            catch (const std::exception&) {}
        } else if (keylc == "replaygain_album_gain") {
            try { out.rg_album_gain = std::stof(std::string(val)); }
            catch (const std::exception&) {}
        } else if (keylc == "replaygain_album_peak") {
            try { out.rg_album_peak = std::stof(std::string(val)); }
            catch (const std::exception&) {}
        }
    }
}

std::expected<std::unique_ptr<OpusDecoder>, Error>
OpusDecoder::open(const std::filesystem::path& path) {
    int err = 0;
    OggOpusFile* of = op_open_file(path.c_str(), &err);
    if (!of) {
        std::string msg = "op_open_file: ";
        msg += std::to_string(err);
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }
    std::unique_ptr<OpusDecoder> self{new OpusDecoder()};
    self->of_ = of;

    const OpusHead* head = op_head(of, -1);
    if (!head) {
        return std::unexpected(
            Error{ErrorCode::DecoderInitFailed, "op_head returned null"});
    }
    self->fmt_.sample_rate_hz = 48000;
    self->fmt_.channels = static_cast<std::uint16_t>(head->channel_count);
    self->fmt_.sample_format = SampleFormat::FLOAT_LE;

    const ogg_int64_t total = op_pcm_total(of, -1);
    self->total_frames_ = total > 0 ? static_cast<std::uint64_t>(total) : 0;

    parse_opus_tags(op_tags(of, -1), self->tags_);
    return self;
}

std::expected<std::size_t, Error>
OpusDecoder::read(std::span<std::byte> dst, std::size_t max_frames) {
    if (max_frames == 0) {
        return 0;
    }
    const unsigned channels = fmt_.channels;
    const std::size_t want_floats = max_frames * channels;
    const std::size_t want_bytes = want_floats * sizeof(float);
    if (dst.size() < want_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "destination buffer too small"});
    }
    auto* out = reinterpret_cast<float*>(dst.data());
    const int got = op_read_float(of_, out, static_cast<int>(want_floats), nullptr);
    if (got < 0) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "op_read_float negative return"});
    }
    return static_cast<std::size_t>(got);
}

std::expected<void, Error> OpusDecoder::seek_frame(std::uint64_t frame) {
    if (op_pcm_seek(of_, static_cast<ogg_int64_t>(frame)) != 0) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, "op_pcm_seek failed"});
    }
    return {};
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_opus_decoder(const std::filesystem::path& path) {
    auto d = OpusDecoder::open(path);
    if (!d) {
        return std::unexpected(d.error());
    }
    return std::unique_ptr<IDecoder>(std::move(*d));
}

} // namespace transporter::engine
