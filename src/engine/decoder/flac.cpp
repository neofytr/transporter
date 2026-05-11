// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoders.hpp"

#include <FLAC/format.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace transporter::engine {

namespace {

struct DecoderDeleter {
    void operator()(FLAC__StreamDecoder* p) const noexcept {
        if (p) {
            FLAC__stream_decoder_delete(p);
        }
    }
};

class FlacDecoder final : public IDecoder {
public:
    static std::expected<std::unique_ptr<FlacDecoder>, Error>
    open(const std::filesystem::path& path);

    ~FlacDecoder() override {
        if (decoder_) {
            FLAC__stream_decoder_finish(decoder_.get());
        }
    }

    PcmFormat format() const noexcept override { return fmt_; }
    std::uint64_t total_frames() const noexcept override { return total_frames_; }
    const Tags& tags() const noexcept override { return tags_; }

    std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) override;

    std::expected<void, Error> seek_frame(std::uint64_t frame) override;

private:
    FlacDecoder() = default;

    std::unique_ptr<FLAC__StreamDecoder, DecoderDeleter> decoder_;
    PcmFormat fmt_{};
    Tags tags_{};
    std::uint64_t total_frames_ = 0;

    // Carry-over buffer for a partially consumed frame from libFLAC.
    std::vector<std::byte> carry_;
    std::size_t carry_off_ = 0; // byte offset into carry_

    bool eof_ = false;
    bool error_ = false;
    std::string error_msg_;

    static FLAC__StreamDecoderWriteStatus
    write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
             const FLAC__int32* const buffer[], void* client);
    static void metadata_cb(const FLAC__StreamDecoder*,
                            const FLAC__StreamMetadata* meta, void* client);
    static void error_cb(const FLAC__StreamDecoder*,
                         FLAC__StreamDecoderErrorStatus status, void* client);
};

void parse_vorbis_comment(const FLAC__StreamMetadata_VorbisComment& vc, Tags& out) {
    for (FLAC__uint32 i = 0; i < vc.num_comments; ++i) {
        const auto& c = vc.comments[i];
        std::string_view s{reinterpret_cast<const char*>(c.entry), c.length};
        const auto eq = s.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        std::string_view key = s.substr(0, eq);
        std::string_view val = s.substr(eq + 1);
        std::string keylc;
        keylc.reserve(key.size());
        for (char ch : key) {
            keylc.push_back(static_cast<char>(
                ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch));
        }
        if (keylc == "artist") {
            out.artist.assign(val);
        } else if (keylc == "albumartist" || keylc == "album_artist" ||
                   keylc == "album artist") {
            out.album_artist.assign(val);
        } else if (keylc == "album") {
            out.album.assign(val);
        } else if (keylc == "title") {
            out.title.assign(val);
        } else if (keylc == "tracknumber" || keylc == "track") {
            out.track_no.assign(val);
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

FLAC__StreamDecoderWriteStatus
FlacDecoder::write_cb(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                      const FLAC__int32* const buffer[], void* client) {
    auto* self = static_cast<FlacDecoder*>(client);
    const unsigned channels = frame->header.channels;
    const unsigned blocksize = frame->header.blocksize;
    const unsigned bits = frame->header.bits_per_sample;

    const SampleFormat sf = self->fmt_.sample_format;
    self->carry_.clear();
    self->carry_off_ = 0;

    if (sf == SampleFormat::S16_LE) {
        self->carry_.resize(static_cast<std::size_t>(blocksize) * channels * 2);
        auto* out = reinterpret_cast<std::int16_t*>(self->carry_.data());
        for (unsigned f = 0; f < blocksize; ++f) {
            for (unsigned c = 0; c < channels; ++c) {
                std::int32_t v = buffer[c][f];
                if (bits != 16) {
                    // Promote 8/12/etc. to 16 (rare). Right-justified -> left-pad.
                    const int shift = 16 - static_cast<int>(bits);
                    v = shift > 0 ? v << shift : v >> -shift;
                }
                if (v > 32767) {
                    v = 32767;
                }
                if (v < -32768) {
                    v = -32768;
                }
                *out++ = static_cast<std::int16_t>(v);
            }
        }
    } else if (sf == SampleFormat::S24_3LE) {
        self->carry_.resize(static_cast<std::size_t>(blocksize) * channels * 3);
        auto* out = reinterpret_cast<std::uint8_t*>(self->carry_.data());
        for (unsigned f = 0; f < blocksize; ++f) {
            for (unsigned c = 0; c < channels; ++c) {
                const std::int32_t v = buffer[c][f];
                const std::uint32_t u = static_cast<std::uint32_t>(v) & 0x00FFFFFFu;
                *out++ = static_cast<std::uint8_t>(u & 0xFF);
                *out++ = static_cast<std::uint8_t>((u >> 8) & 0xFF);
                *out++ = static_cast<std::uint8_t>((u >> 16) & 0xFF);
            }
        }
    } else if (sf == SampleFormat::S32_LE) {
        self->carry_.resize(static_cast<std::size_t>(blocksize) * channels * 4);
        auto* out = reinterpret_cast<std::int32_t*>(self->carry_.data());
        for (unsigned f = 0; f < blocksize; ++f) {
            for (unsigned c = 0; c < channels; ++c) {
                *out++ = buffer[c][f];
            }
        }
    } else {
        self->error_ = true;
        self->error_msg_ = "FLAC sample format not S16/S24/S32";
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacDecoder::metadata_cb(const FLAC__StreamDecoder*,
                              const FLAC__StreamMetadata* meta, void* client) {
    auto* self = static_cast<FlacDecoder*>(client);
    if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& si = meta->data.stream_info;
        self->fmt_.sample_rate_hz = si.sample_rate;
        self->fmt_.channels = static_cast<std::uint16_t>(si.channels);
        if (si.bits_per_sample <= 16) {
            self->fmt_.sample_format = SampleFormat::S16_LE;
        } else if (si.bits_per_sample <= 24) {
            self->fmt_.sample_format = SampleFormat::S24_3LE;
        } else {
            self->fmt_.sample_format = SampleFormat::S32_LE;
        }
        self->total_frames_ = si.total_samples;
    } else if (meta->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
        parse_vorbis_comment(meta->data.vorbis_comment, self->tags_);
    }
}

void FlacDecoder::error_cb(const FLAC__StreamDecoder*,
                           FLAC__StreamDecoderErrorStatus status, void* client) {
    auto* self = static_cast<FlacDecoder*>(client);
    self->error_ = true;
    self->error_msg_ = "FLAC decoder error: ";
    self->error_msg_ += FLAC__StreamDecoderErrorStatusString[status];
}

std::expected<std::unique_ptr<FlacDecoder>, Error>
FlacDecoder::open(const std::filesystem::path& path) {
    std::unique_ptr<FlacDecoder> self{new FlacDecoder()};

    FLAC__StreamDecoder* raw = FLAC__stream_decoder_new();
    if (!raw) {
        return std::unexpected(Error{ErrorCode::DecoderInitFailed,
                                     "FLAC__stream_decoder_new failed"});
    }
    self->decoder_.reset(raw);

    FLAC__stream_decoder_set_md5_checking(raw, false);
    FLAC__stream_decoder_set_metadata_respond(raw, FLAC__METADATA_TYPE_VORBIS_COMMENT);

    const FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_file(
        raw, path.c_str(), &FlacDecoder::write_cb, &FlacDecoder::metadata_cb,
        &FlacDecoder::error_cb, self.get());
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        std::string msg = "FLAC__stream_decoder_init_file: ";
        msg += FLAC__StreamDecoderInitStatusString[st];
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }

    if (!FLAC__stream_decoder_process_until_end_of_metadata(raw)) {
        return std::unexpected(Error{ErrorCode::DecoderInitFailed,
                                     "FLAC: process_until_end_of_metadata failed"});
    }
    if (self->fmt_.sample_rate_hz == 0 || self->fmt_.channels == 0) {
        return std::unexpected(Error{ErrorCode::DecoderInitFailed,
                                     "FLAC: STREAMINFO missing or invalid"});
    }
    return self;
}

std::expected<std::size_t, Error>
FlacDecoder::read(std::span<std::byte> dst, std::size_t max_frames) {
    if (max_frames == 0) {
        return 0;
    }
    const unsigned frame_bytes = fmt_.frame_bytes();
    if (dst.size() < max_frames * frame_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "destination buffer too small"});
    }

    std::size_t frames_written = 0;
    auto* p = dst.data();

    while (frames_written < max_frames) {
        if (carry_off_ >= carry_.size()) {
            if (eof_) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(decoder_.get())) {
                return std::unexpected(
                    Error{ErrorCode::DecoderReadFailed,
                          error_msg_.empty() ? "FLAC process_single failed"
                                             : error_msg_});
            }
            const FLAC__StreamDecoderState st =
                FLAC__stream_decoder_get_state(decoder_.get());
            if (st == FLAC__STREAM_DECODER_END_OF_STREAM) {
                eof_ = true;
                if (carry_off_ >= carry_.size()) {
                    break;
                }
            }
            if (error_) {
                return std::unexpected(Error{ErrorCode::DecoderReadFailed, error_msg_});
            }
            if (carry_off_ >= carry_.size()) {
                continue;
            }
        }
        const std::size_t carry_bytes = carry_.size() - carry_off_;
        const std::size_t want_frames = max_frames - frames_written;
        const std::size_t avail_frames = carry_bytes / frame_bytes;
        const std::size_t take_frames =
            want_frames < avail_frames ? want_frames : avail_frames;
        const std::size_t take_bytes = take_frames * frame_bytes;
        std::memcpy(p, carry_.data() + carry_off_, take_bytes);
        p += take_bytes;
        carry_off_ += take_bytes;
        frames_written += take_frames;
    }
    return frames_written;
}

std::expected<void, Error> FlacDecoder::seek_frame(std::uint64_t frame) {
    if (!FLAC__stream_decoder_seek_absolute(decoder_.get(), frame)) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, "FLAC seek_absolute failed"});
    }
    carry_.clear();
    carry_off_ = 0;
    eof_ = false;
    error_ = false;
    error_msg_.clear();
    return {};
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_flac_decoder(const std::filesystem::path& path) {
    auto d = FlacDecoder::open(path);
    if (!d) {
        return std::unexpected(d.error());
    }
    return std::unique_ptr<IDecoder>(std::move(*d));
}

} // namespace transporter::engine
