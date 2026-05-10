// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoders.hpp"

#include <mpg123.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace transporter::engine {

namespace {

// mpg123_init / mpg123_exit are process-global; libmpg123 5.x docs that you
// can call init repeatedly and it is reference counted, but other consumers
// in-process may also call it. Guard with std::once_flag and skip exit.
void ensure_mpg123_inited() {
    static std::once_flag flag;
    std::call_once(flag, [] { mpg123_init(); });
}

struct HandleDeleter {
    void operator()(mpg123_handle* h) const noexcept {
        if (h) {
            mpg123_close(h);
            mpg123_delete(h);
        }
    }
};

class Mp3Decoder final : public IDecoder {
public:
    static std::expected<std::unique_ptr<Mp3Decoder>, Error>
    open(const std::filesystem::path& path);

    PcmFormat format() const noexcept override { return fmt_; }
    std::uint64_t total_frames() const noexcept override { return total_frames_; }
    const Tags& tags() const noexcept override { return tags_; }

    std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) override;

    std::expected<void, Error> seek_frame(std::uint64_t frame) override;

private:
    Mp3Decoder() = default;

    std::unique_ptr<mpg123_handle, HandleDeleter> mh_;
    PcmFormat fmt_{};
    Tags tags_{};
    std::uint64_t total_frames_ = 0;
    unsigned frame_bytes_ = 0;
};

void slurp_id3v2(const mpg123_id3v2* v2, Tags& out) {
    auto take = [](const mpg123_string* s) -> std::string {
        if (!s || !s->p || s->fill == 0) {
            return {};
        }
        std::size_t n = s->fill;
        while (n > 0 && s->p[n - 1] == '\0') {
            --n;
        }
        return std::string(s->p, n);
    };
    if (v2->title) {
        out.title = take(v2->title);
    }
    if (v2->artist) {
        out.artist = take(v2->artist);
    }
    if (v2->album) {
        out.album = take(v2->album);
    }
    if (v2->year) {
        out.date = take(v2->year);
    }
    for (std::size_t i = 0; i < v2->texts; ++i) {
        const auto& t = v2->text[i];
        const std::string_view id{t.id, 4};
        if (id == "TRCK") {
            out.track_no = take(&t.text);
        } else if (out.date.empty() && (id == "TYER" || id == "TDRC")) {
            out.date = take(&t.text);
        }
    }
}

void slurp_id3v1(const mpg123_id3v1* v1, Tags& out) {
    auto trim_pad = [](const char* s, std::size_t n) -> std::string {
        std::string r(s, n);
        while (!r.empty() && (r.back() == '\0' || r.back() == ' ')) {
            r.pop_back();
        }
        return r;
    };
    if (out.title.empty()) {
        out.title = trim_pad(v1->title, sizeof v1->title);
    }
    if (out.artist.empty()) {
        out.artist = trim_pad(v1->artist, sizeof v1->artist);
    }
    if (out.album.empty()) {
        out.album = trim_pad(v1->album, sizeof v1->album);
    }
    if (out.date.empty()) {
        out.date = trim_pad(v1->year, sizeof v1->year);
    }
    if (out.track_no.empty() && v1->comment[28] == 0 && v1->comment[29] != 0) {
        // ID3v1.1 track number lives in comment[29].
        out.track_no = std::to_string(static_cast<unsigned>(
            static_cast<unsigned char>(v1->comment[29])));
    }
}

std::expected<std::unique_ptr<Mp3Decoder>, Error>
Mp3Decoder::open(const std::filesystem::path& path) {
    ensure_mpg123_inited();

    int err = MPG123_OK;
    mpg123_handle* raw = mpg123_new(nullptr, &err);
    if (!raw) {
        std::string msg = "mpg123_new: ";
        msg += mpg123_plain_strerror(err);
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }
    std::unique_ptr<Mp3Decoder> self{new Mp3Decoder()};
    self->mh_.reset(raw);

    // Quiet errors to stderr; gapless on; do not force endian (we want LE on
    // x86-64 and that's already native). No FORCE_RATE: native rate per spec.
    mpg123_param(raw, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_GAPLESS, 0.0);
    mpg123_param(raw, MPG123_REMOVE_FLAGS, MPG123_FORCE_FLOAT | MPG123_FORCE_8BIT, 0.0);

    // Restrict output formats to S16_LE at the file's reported rate; configure
    // after open via getformat once we know the rate.
    err = mpg123_open(raw, path.c_str());
    if (err != MPG123_OK) {
        std::string msg = "mpg123_open: ";
        msg += mpg123_plain_strerror(err);
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }

    // Scan to compute exact length.
    mpg123_scan(raw);

    long rate = 0;
    int channels = 0;
    int encoding = 0;
    err = mpg123_getformat(raw, &rate, &channels, &encoding);
    if (err != MPG123_OK) {
        std::string msg = "mpg123_getformat: ";
        msg += mpg123_plain_strerror(err);
        return std::unexpected(Error{ErrorCode::DecoderInitFailed, std::move(msg)});
    }

    // Lock the output format so libmpg123 does not flip mid-stream.
    mpg123_format_none(raw);
    if (mpg123_format(raw, rate, channels, MPG123_ENC_SIGNED_16) != MPG123_OK) {
        return std::unexpected(Error{ErrorCode::DecoderInitFailed,
                                     "mpg123_format(S16) rejected"});
    }

    self->fmt_.sample_rate_hz = static_cast<std::uint32_t>(rate);
    self->fmt_.channels = static_cast<std::uint16_t>(channels);
    self->fmt_.sample_format = SampleFormat::S16_LE;
    self->frame_bytes_ = self->fmt_.frame_bytes();

    const std::int64_t len = mpg123_length64(raw);
    self->total_frames_ = len > 0 ? static_cast<std::uint64_t>(len) : 0;

    if (mpg123_meta_check(raw) & MPG123_ID3) {
        mpg123_id3v1* v1 = nullptr;
        mpg123_id3v2* v2 = nullptr;
        if (mpg123_id3(raw, &v1, &v2) == MPG123_OK) {
            if (v2) {
                slurp_id3v2(v2, self->tags_);
            }
            if (v1) {
                slurp_id3v1(v1, self->tags_);
            }
        }
    }

    return self;
}

std::expected<std::size_t, Error>
Mp3Decoder::read(std::span<std::byte> dst, std::size_t max_frames) {
    if (max_frames == 0) {
        return 0;
    }
    const std::size_t want_bytes = max_frames * frame_bytes_;
    if (dst.size() < want_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "destination buffer too small"});
    }
    std::size_t done = 0;
    const int rc = mpg123_read(mh_.get(), dst.data(), want_bytes, &done);
    if (rc == MPG123_OK || rc == MPG123_DONE) {
        return done / frame_bytes_;
    }
    if (rc == MPG123_NEW_FORMAT) {
        // Format change mid-stream: refuse — bit-perfect contract is one
        // format per file. mpg123 with format locked above should not do this.
        return std::unexpected(Error{ErrorCode::DecoderReadFailed,
                                     "MP3: stream format changed mid-file"});
    }
    if (rc == MPG123_NEED_MORE) {
        // Internal buffering ran out — treat as EOF with what we got.
        return done / frame_bytes_;
    }
    std::string msg = "mpg123_read: ";
    msg += mpg123_strerror(mh_.get());
    return std::unexpected(Error{ErrorCode::DecoderReadFailed, std::move(msg)});
}

std::expected<void, Error> Mp3Decoder::seek_frame(std::uint64_t frame) {
    if (mpg123_seek64(mh_.get(), static_cast<std::int64_t>(frame), SEEK_SET) < 0) {
        std::string msg = "mpg123_seek: ";
        msg += mpg123_strerror(mh_.get());
        return std::unexpected(Error{ErrorCode::DecoderSeekFailed, std::move(msg)});
    }
    return {};
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_mp3_decoder(const std::filesystem::path& path) {
    auto d = Mp3Decoder::open(path);
    if (!d) {
        return std::unexpected(d.error());
    }
    return std::unique_ptr<IDecoder>(std::move(*d));
}

} // namespace transporter::engine
