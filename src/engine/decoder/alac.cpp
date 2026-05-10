// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoders.hpp"
#include "mp4_demux.hpp"

#include <ALACBitUtilities.h>
#include <ALACDecoder.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace transporter::engine {

namespace {

class FileHandle {
public:
    explicit FileHandle(std::FILE* f) : f_(f) {}
    ~FileHandle() {
        if (f_) {
            std::fclose(f_);
        }
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& o) noexcept : f_(o.f_) { o.f_ = nullptr; }
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) {
            if (f_) {
                std::fclose(f_);
            }
            f_ = o.f_;
            o.f_ = nullptr;
        }
        return *this;
    }
    std::FILE* get() const noexcept { return f_; }

private:
    std::FILE* f_;
};

class AlacDecoder final : public IDecoder {
public:
    static std::expected<std::unique_ptr<AlacDecoder>, Error>
    open(const std::filesystem::path& path);

    PcmFormat format() const noexcept override { return fmt_; }
    std::uint64_t total_frames() const noexcept override { return total_frames_; }
    const Tags& tags() const noexcept override { return tags_; }

    std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) override;

    std::expected<void, Error> seek_frame(std::uint64_t frame) override;

private:
    AlacDecoder() = default;

    FileHandle fh_{nullptr};
    PcmFormat fmt_{};
    Tags tags_{};
    std::uint64_t total_frames_ = 0;
    unsigned per_sample_bytes_ = 0; // bytes per channel per frame
    unsigned channels_ = 0;

    std::unique_ptr<ALACDecoder> codec_;
    std::vector<mp4::Sample> samples_;
    std::size_t next_sample_ = 0;

    // Buffers
    std::vector<std::uint8_t> packet_buf_;     // one MP4 sample worth
    std::vector<std::byte> decoded_buf_;       // up to frames_per_packet
    std::size_t decoded_frames_ = 0;
    std::size_t decoded_off_frames_ = 0;

    std::uint64_t played_frames_ = 0;
    std::uint32_t frames_per_packet_ = 0;
};

std::expected<std::unique_ptr<AlacDecoder>, Error>
AlacDecoder::open(const std::filesystem::path& path) {
    FileHandle fh{std::fopen(path.c_str(), "rb")};
    if (!fh.get()) {
        std::string msg = "fopen ";
        msg += path.string();
        msg += ": ";
        msg += std::strerror(errno);
        return std::unexpected(Error{ErrorCode::FileOpenFailed, std::move(msg)});
    }

    auto demuxed = mp4::parse(fh.get());
    if (!demuxed) {
        return std::unexpected(demuxed.error());
    }

    if (demuxed->channels == 0 || demuxed->sample_rate_hz == 0) {
        return std::unexpected(Error{ErrorCode::DecoderInitFailed,
                                     "ALAC: channels/sample_rate not set"});
    }

    SampleFormat sf;
    unsigned per_sample = 0;
    switch (demuxed->bits_per_sample) {
    case 16:
        sf = SampleFormat::S16_LE;
        per_sample = 2;
        break;
    case 24:
        sf = SampleFormat::S24_3LE;
        per_sample = 3;
        break;
    case 32:
        sf = SampleFormat::S32_LE;
        per_sample = 4;
        break;
    default: {
        std::string msg = "ALAC bitDepth=";
        msg += std::to_string(demuxed->bits_per_sample);
        msg += " not handled";
        return std::unexpected(Error{ErrorCode::WavUnsupportedTag, std::move(msg)});
    }
    }

    std::unique_ptr<AlacDecoder> self{new AlacDecoder()};
    self->fh_ = std::move(fh);
    self->fmt_.sample_rate_hz = demuxed->sample_rate_hz;
    self->fmt_.channels = demuxed->channels;
    self->fmt_.sample_format = sf;
    self->per_sample_bytes_ = per_sample;
    self->channels_ = demuxed->channels;
    self->tags_ = std::move(demuxed->tags);
    self->samples_ = std::move(demuxed->samples);
    self->frames_per_packet_ = demuxed->default_samples_per_packet != 0
                                   ? demuxed->default_samples_per_packet
                                   : 4096;

    self->codec_.reset(new ALACDecoder());
    const std::int32_t init_rc = self->codec_->Init(
        const_cast<void*>(static_cast<const void*>(
            demuxed->alac_magic_cookie.data())),
        static_cast<std::uint32_t>(demuxed->alac_magic_cookie.size()));
    if (init_rc != ALAC_noErr) {
        std::string msg = "ALACDecoder::Init failed: ";
        msg += std::to_string(init_rc);
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }

    // Total frames: prefer mdia duration when timescale matches sample_rate.
    if (demuxed->duration_in_timescale != 0 && demuxed->timescale != 0) {
        if (demuxed->timescale == demuxed->sample_rate_hz) {
            self->total_frames_ = demuxed->duration_in_timescale;
        } else {
            self->total_frames_ = static_cast<std::uint64_t>(
                static_cast<long double>(demuxed->duration_in_timescale) *
                static_cast<long double>(demuxed->sample_rate_hz) /
                static_cast<long double>(demuxed->timescale));
        }
    }
    if (self->total_frames_ == 0) {
        // Fallback: sample count * frames_per_packet, minus partial last packet.
        self->total_frames_ = static_cast<std::uint64_t>(self->samples_.size()) *
                              self->frames_per_packet_;
    }

    self->decoded_buf_.resize(static_cast<std::size_t>(self->frames_per_packet_) *
                              self->channels_ * self->per_sample_bytes_);
    return self;
}

std::expected<std::size_t, Error>
AlacDecoder::read(std::span<std::byte> dst, std::size_t max_frames) {
    if (max_frames == 0) {
        return 0;
    }
    const unsigned frame_bytes = fmt_.frame_bytes();
    if (dst.size() < max_frames * frame_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "destination buffer too small"});
    }

    const std::uint64_t remaining = total_frames_ - played_frames_;
    if (remaining == 0) {
        return 0;
    }
    const std::size_t cap_frames =
        max_frames < remaining ? max_frames : static_cast<std::size_t>(remaining);

    auto* p = dst.data();
    std::size_t written = 0;

    while (written < cap_frames) {
        if (decoded_off_frames_ >= decoded_frames_) {
            if (next_sample_ >= samples_.size()) {
                break;
            }
            const auto& s = samples_[next_sample_++];
            packet_buf_.resize(s.size);
            if (std::fseek(fh_.get(), static_cast<long>(s.offset), SEEK_SET) != 0) {
                return std::unexpected(
                    Error{ErrorCode::DecoderReadFailed, "ALAC: fseek to sample failed"});
            }
            if (std::fread(packet_buf_.data(), 1, s.size, fh_.get()) != s.size) {
                return std::unexpected(
                    Error{ErrorCode::DecoderReadFailed, "ALAC: short read on sample"});
            }
            BitBuffer bb;
            BitBufferInit(&bb, packet_buf_.data(), s.size);
            std::uint32_t out_n = 0;
            const std::int32_t rc = codec_->Decode(
                &bb, reinterpret_cast<std::uint8_t*>(decoded_buf_.data()),
                frames_per_packet_, channels_, &out_n);
            if (rc != ALAC_noErr) {
                std::string msg = "ALACDecoder::Decode rc=";
                msg += std::to_string(rc);
                return std::unexpected(Error{ErrorCode::DecoderReadFailed, std::move(msg)});
            }
            decoded_frames_ = out_n;
            decoded_off_frames_ = 0;
            if (decoded_frames_ == 0) {
                continue;
            }
        }

        const std::size_t avail = decoded_frames_ - decoded_off_frames_;
        const std::size_t want = cap_frames - written;
        const std::size_t take = avail < want ? avail : want;
        std::memcpy(p, decoded_buf_.data() + decoded_off_frames_ * frame_bytes,
                    take * frame_bytes);
        p += take * frame_bytes;
        decoded_off_frames_ += take;
        written += take;
        played_frames_ += take;
    }
    return written;
}

std::expected<void, Error> AlacDecoder::seek_frame(std::uint64_t frame) {
    if (frame > total_frames_) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, "seek beyond total_frames"});
    }
    // Coarse seek to packet boundary. Snap to nearest preceding packet.
    if (frames_per_packet_ == 0) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, "ALAC: frames_per_packet is zero"});
    }
    const std::size_t packet_idx =
        static_cast<std::size_t>(frame / frames_per_packet_);
    next_sample_ = packet_idx;
    decoded_off_frames_ = 0;
    decoded_frames_ = 0;
    played_frames_ = static_cast<std::uint64_t>(packet_idx) * frames_per_packet_;
    return {};
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_alac_decoder(const std::filesystem::path& path) {
    auto d = AlacDecoder::open(path);
    if (!d) {
        return std::unexpected(d.error());
    }
    return std::unique_ptr<IDecoder>(std::move(*d));
}

} // namespace transporter::engine
