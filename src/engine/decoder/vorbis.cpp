// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoders.hpp"

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace transporter::engine {

namespace {

class VorbisDecoder final : public IDecoder {
public:
    static std::expected<std::unique_ptr<VorbisDecoder>, Error>
    open(const std::filesystem::path& path);

    ~VorbisDecoder() override {
        if (open_) {
            ov_clear(&vf_);
        }
    }

    PcmFormat format() const noexcept override { return fmt_; }
    std::uint64_t total_frames() const noexcept override { return total_frames_; }
    const Tags& tags() const noexcept override { return tags_; }

    std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) override;

    std::expected<void, Error> seek_frame(std::uint64_t frame) override;

private:
    VorbisDecoder() = default;

    OggVorbis_File vf_{};
    bool open_ = false;
    PcmFormat fmt_{};
    Tags tags_{};
    std::uint64_t total_frames_ = 0;
};

void parse_vorbis_comment(vorbis_comment* vc, Tags& out) {
    if (!vc) {
        return;
    }
    for (int i = 0; i < vc->comments; ++i) {
        const std::string_view s{vc->user_comments[i],
                                 static_cast<std::size_t>(vc->comment_lengths[i])};
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
        } else if (keylc == "album") {
            out.album.assign(val);
        } else if (keylc == "title") {
            out.title.assign(val);
        } else if (keylc == "tracknumber" || keylc == "track") {
            out.track_no.assign(val);
        } else if (keylc == "date" || keylc == "year") {
            out.date.assign(val);
        }
    }
}

std::expected<std::unique_ptr<VorbisDecoder>, Error>
VorbisDecoder::open(const std::filesystem::path& path) {
    std::unique_ptr<VorbisDecoder> self{new VorbisDecoder()};
    const int rc = ov_fopen(path.c_str(), &self->vf_);
    if (rc < 0) {
        std::string msg = "ov_fopen failed: ";
        msg += std::to_string(rc);
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }
    self->open_ = true;

    vorbis_info* info = ov_info(&self->vf_, -1);
    if (!info) {
        return std::unexpected(
            Error{ErrorCode::DecoderInitFailed, "ov_info returned null"});
    }
    self->fmt_.sample_rate_hz = static_cast<std::uint32_t>(info->rate);
    self->fmt_.channels = static_cast<std::uint16_t>(info->channels);
    self->fmt_.sample_format = SampleFormat::FLOAT_LE;

    const ogg_int64_t total = ov_pcm_total(&self->vf_, -1);
    self->total_frames_ = total > 0 ? static_cast<std::uint64_t>(total) : 0;

    parse_vorbis_comment(ov_comment(&self->vf_, -1), self->tags_);
    return self;
}

std::expected<std::size_t, Error>
VorbisDecoder::read(std::span<std::byte> dst, std::size_t max_frames) {
    if (max_frames == 0) {
        return 0;
    }
    const unsigned channels = fmt_.channels;
    const unsigned frame_bytes = fmt_.frame_bytes();
    const std::size_t want_bytes = max_frames * frame_bytes;
    if (dst.size() < want_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "destination buffer too small"});
    }

    float** planar = nullptr;
    int link = 0;
    const long got = ov_read_float(&vf_, &planar, static_cast<int>(max_frames), &link);
    if (got < 0) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "ov_read_float negative return"});
    }
    if (got == 0) {
        return 0;
    }

    auto* out = reinterpret_cast<float*>(dst.data());
    const long n = got;
    for (long f = 0; f < n; ++f) {
        for (unsigned c = 0; c < channels; ++c) {
            *out++ = planar[c][f];
        }
    }
    return static_cast<std::size_t>(got);
}

std::expected<void, Error> VorbisDecoder::seek_frame(std::uint64_t frame) {
    if (ov_pcm_seek(&vf_, static_cast<ogg_int64_t>(frame)) != 0) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, "ov_pcm_seek failed"});
    }
    return {};
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_vorbis_decoder(const std::filesystem::path& path) {
    auto d = VorbisDecoder::open(path);
    if (!d) {
        return std::unexpected(d.error());
    }
    return std::unique_ptr<IDecoder>(std::move(*d));
}

} // namespace transporter::engine
