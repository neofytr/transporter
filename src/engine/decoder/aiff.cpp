// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoders.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace transporter::engine {

namespace {

constexpr std::uint32_t fourcc_be(const char (&s)[5]) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]));
}

constexpr std::uint32_t ID_FORM = fourcc_be("FORM");
constexpr std::uint32_t ID_AIFF = fourcc_be("AIFF");
constexpr std::uint32_t ID_AIFC = fourcc_be("AIFC");
constexpr std::uint32_t ID_COMM = fourcc_be("COMM");
constexpr std::uint32_t ID_SSND = fourcc_be("SSND");
constexpr std::uint32_t ID_NAME = fourcc_be("NAME");
constexpr std::uint32_t ID_AUTH = fourcc_be("AUTH");
constexpr std::uint32_t ID_ANNO = fourcc_be("ANNO");
constexpr std::uint32_t ID_NONE = fourcc_be("NONE");
constexpr std::uint32_t ID_FL32 = fourcc_be("FL32");
constexpr std::uint32_t ID_fl32 = fourcc_be("fl32");
constexpr std::uint32_t ID_sowt = fourcc_be("sowt");

std::uint16_t rd_u16be(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) | static_cast<std::uint16_t>(p[1]));
}
std::uint32_t rd_u32be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

// AIFF stores sample rate as IEEE 754 80-bit extended ("long double") in BE.
// Decode the integer part — AIFF sample rates are always integral.
std::uint32_t rd_extended_be(const std::uint8_t* p) noexcept {
    const std::uint16_t sign_exp =
        static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    const std::uint16_t exp_biased = sign_exp & 0x7FFFu;
    std::uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i) {
        mantissa = (mantissa << 8) | p[2 + i];
    }
    if (exp_biased == 0 && mantissa == 0) {
        return 0;
    }
    const int exp = static_cast<int>(exp_biased) - 16383;
    if (exp < 0) {
        return 0;
    }
    if (exp > 63) {
        return 0;
    }
    const int shift = 63 - exp;
    return static_cast<std::uint32_t>(mantissa >> shift);
}

std::unexpected<Error> malformed(std::string m) {
    return std::unexpected(Error{ErrorCode::WavMalformed, std::move(m)});
}

std::unexpected<Error> unsupported(std::string m) {
    return std::unexpected(Error{ErrorCode::WavUnsupportedTag, std::move(m)});
}

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

// Byte-swap interleaved samples in place. width = 2,3,4 bytes per sample.
void swap_in_place(std::byte* p, std::size_t bytes, unsigned width) noexcept {
    if (width == 2) {
        for (std::size_t i = 0; i + 1 < bytes; i += 2) {
            std::swap(p[i], p[i + 1]);
        }
    } else if (width == 3) {
        for (std::size_t i = 0; i + 2 < bytes; i += 3) {
            std::swap(p[i], p[i + 2]);
        }
    } else if (width == 4) {
        for (std::size_t i = 0; i + 3 < bytes; i += 4) {
            std::swap(p[i], p[i + 3]);
            std::swap(p[i + 1], p[i + 2]);
        }
    }
}

class AiffDecoder final : public IDecoder {
public:
    static std::expected<std::unique_ptr<AiffDecoder>, Error>
    open(const std::filesystem::path& path);

    PcmFormat format() const noexcept override { return fmt_; }
    std::uint64_t total_frames() const noexcept override { return total_frames_; }
    const Tags& tags() const noexcept override { return tags_; }

    std::expected<std::size_t, Error>
    read(std::span<std::byte> dst, std::size_t max_frames) override;

    std::expected<void, Error> seek_frame(std::uint64_t frame) override;

private:
    FileHandle fh_;
    PcmFormat fmt_{};
    Tags tags_{};
    std::uint64_t total_frames_ = 0;
    std::uint64_t data_offset_ = 0;
    std::uint64_t frames_read_ = 0;
    unsigned frame_bytes_ = 0;
    bool needs_swap_ = true; // AIFF is BE; AIFC `sowt` is already LE.

    explicit AiffDecoder(FileHandle fh) : fh_(std::move(fh)) {}
};

std::expected<std::unique_ptr<AiffDecoder>, Error>
AiffDecoder::open(const std::filesystem::path& path) {
    FileHandle fh{std::fopen(path.c_str(), "rb")};
    if (!fh.get()) {
        std::string msg = "fopen ";
        msg += path.string();
        msg += ": ";
        msg += std::strerror(errno);
        return std::unexpected(Error{ErrorCode::FileOpenFailed, std::move(msg)});
    }
    auto* fp = fh.get();

    std::array<std::uint8_t, 12> hdr{};
    if (std::fread(hdr.data(), 1, hdr.size(), fp) != hdr.size()) {
        return malformed("file too short for FORM header");
    }
    if (rd_u32be(hdr.data()) != ID_FORM) {
        return malformed("missing FORM magic");
    }
    const std::uint32_t form_type = rd_u32be(hdr.data() + 8);
    const bool is_aifc = form_type == ID_AIFC;
    if (form_type != ID_AIFF && !is_aifc) {
        return malformed("FORM type is not AIFF or AIFC");
    }

    bool comm_seen = false;
    bool ssnd_seen = false;
    std::uint16_t channels = 0;
    std::uint32_t num_sample_frames = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t compression_type = ID_NONE;
    std::uint64_t data_offset = 0;
    bool needs_swap = true;
    Tags tags;

    while (true) {
        std::array<std::uint8_t, 8> chunk_hdr{};
        const auto got = std::fread(chunk_hdr.data(), 1, chunk_hdr.size(), fp);
        if (got == 0) {
            break;
        }
        if (got != chunk_hdr.size()) {
            return malformed("truncated chunk header");
        }
        const std::uint32_t id = rd_u32be(chunk_hdr.data());
        const std::uint32_t sz = rd_u32be(chunk_hdr.data() + 4);

        if (id == ID_COMM) {
            std::vector<std::uint8_t> body(sz);
            if (sz != 0 && std::fread(body.data(), 1, sz, fp) != sz) {
                return malformed("truncated COMM");
            }
            if (sz < 18) {
                return malformed("COMM shorter than 18 bytes");
            }
            channels = rd_u16be(body.data());
            num_sample_frames = rd_u32be(body.data() + 2);
            bits_per_sample = rd_u16be(body.data() + 6);
            sample_rate = rd_extended_be(body.data() + 8);
            if (is_aifc) {
                if (sz < 22) {
                    return malformed("AIFC COMM shorter than 22 bytes");
                }
                compression_type = rd_u32be(body.data() + 18);
            } else {
                compression_type = ID_NONE;
            }
            comm_seen = true;
            if ((sz & 1u) != 0u) {
                std::fseek(fp, 1, SEEK_CUR);
            }
        } else if (id == ID_SSND) {
            // SSND body: u32 offset, u32 blockSize, then samples.
            std::array<std::uint8_t, 8> ssnd_hdr{};
            if (sz < 8 || std::fread(ssnd_hdr.data(), 1, 8, fp) != 8) {
                return malformed("truncated SSND header");
            }
            const std::uint32_t offset = rd_u32be(ssnd_hdr.data());
            data_offset = static_cast<std::uint64_t>(std::ftell(fp)) + offset;
            ssnd_seen = true;
            const long advance =
                static_cast<long>(sz) - 8 + ((sz & 1u) != 0u ? 1 : 0);
            if (std::fseek(fp, advance, SEEK_CUR) != 0) {
                break;
            }
        } else if (id == ID_NAME || id == ID_AUTH || id == ID_ANNO) {
            std::vector<std::uint8_t> body(sz);
            if (sz != 0 && std::fread(body.data(), 1, sz, fp) != sz) {
                return malformed("truncated text chunk");
            }
            std::string s(reinterpret_cast<const char*>(body.data()), sz);
            while (!s.empty() && s.back() == '\0') {
                s.pop_back();
            }
            if (id == ID_NAME) {
                tags.title = std::move(s);
            } else if (id == ID_AUTH) {
                tags.artist = std::move(s);
            }
            // ANNO is free-form; ignore for tag mapping.
            if ((sz & 1u) != 0u) {
                std::fseek(fp, 1, SEEK_CUR);
            }
        } else {
            const long advance =
                static_cast<long>(sz) + ((sz & 1u) != 0u ? 1 : 0);
            if (std::fseek(fp, advance, SEEK_CUR) != 0) {
                return malformed("fseek over unknown chunk failed");
            }
        }
    }

    if (!comm_seen) {
        return malformed("COMM chunk not found");
    }
    if (!ssnd_seen) {
        return malformed("SSND chunk not found");
    }

    SampleFormat sf{};
    bool floating = false;
    if (is_aifc) {
        if (compression_type == ID_NONE) {
            needs_swap = true;
        } else if (compression_type == ID_sowt) {
            needs_swap = false;
        } else if (compression_type == ID_FL32 || compression_type == ID_fl32) {
            if (bits_per_sample != 32) {
                return malformed("AIFC FL32 with non-32-bit sample size");
            }
            floating = true;
            needs_swap = true;
        } else {
            char fc[5] = {static_cast<char>((compression_type >> 24) & 0xFF),
                          static_cast<char>((compression_type >> 16) & 0xFF),
                          static_cast<char>((compression_type >> 8) & 0xFF),
                          static_cast<char>(compression_type & 0xFF), 0};
            std::string msg = "AIFC compressionType '";
            msg += fc;
            msg += "' refused (PCM-only path)";
            return unsupported(std::move(msg));
        }
    } else {
        needs_swap = true;
    }

    if (floating) {
        sf = SampleFormat::FLOAT_LE;
    } else if (bits_per_sample == 16) {
        sf = SampleFormat::S16_LE;
    } else if (bits_per_sample == 24) {
        sf = SampleFormat::S24_3LE;
    } else if (bits_per_sample == 32) {
        sf = SampleFormat::S32_LE;
    } else {
        std::string msg = "AIFF sampleSize=";
        msg += std::to_string(bits_per_sample);
        msg += " not handled";
        return unsupported(std::move(msg));
    }

    PcmFormat fmt{};
    fmt.sample_rate_hz = sample_rate;
    fmt.channels = channels;
    fmt.sample_format = sf;
    if (fmt.frame_bytes() == 0) {
        return malformed("AIFF frame_bytes == 0");
    }

    std::unique_ptr<AiffDecoder> dec{new AiffDecoder(std::move(fh))};
    dec->fmt_ = fmt;
    dec->tags_ = std::move(tags);
    dec->total_frames_ = num_sample_frames;
    dec->data_offset_ = data_offset;
    dec->frame_bytes_ = fmt.frame_bytes();
    dec->needs_swap_ = needs_swap;

    if (std::fseek(dec->fh_.get(), static_cast<long>(data_offset), SEEK_SET) != 0) {
        return malformed("fseek to SSND body failed");
    }
    return dec;
}

std::expected<std::size_t, Error>
AiffDecoder::read(std::span<std::byte> dst, std::size_t max_frames) {
    if (max_frames == 0) {
        return 0;
    }
    const std::uint64_t remaining = total_frames_ - frames_read_;
    if (remaining == 0) {
        return 0;
    }
    const std::size_t want = max_frames < remaining
                                 ? max_frames
                                 : static_cast<std::size_t>(remaining);
    const std::size_t want_bytes = want * frame_bytes_;
    if (dst.size() < want_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "destination buffer too small"});
    }
    const std::size_t got = std::fread(dst.data(), 1, want_bytes, fh_.get());
    if (got != want_bytes) {
        return std::unexpected(
            Error{ErrorCode::DecoderReadFailed, "short read on SSND data"});
    }
    if (needs_swap_) {
        unsigned width = sample_format_bytes_per_sample(fmt_.sample_format);
        swap_in_place(dst.data(), got, width);
    }
    const std::size_t got_frames = got / frame_bytes_;
    frames_read_ += got_frames;
    return got_frames;
}

std::expected<void, Error> AiffDecoder::seek_frame(std::uint64_t frame) {
    if (frame > total_frames_) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, "seek beyond total_frames"});
    }
    const std::uint64_t off = data_offset_ + frame * frame_bytes_;
    if (std::fseek(fh_.get(), static_cast<long>(off), SEEK_SET) != 0) {
        return std::unexpected(
            Error{ErrorCode::DecoderSeekFailed, std::strerror(errno)});
    }
    frames_read_ = frame;
    return {};
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_aiff_decoder(const std::filesystem::path& path) {
    auto d = AiffDecoder::open(path);
    if (!d) {
        return std::unexpected(d.error());
    }
    return std::unique_ptr<IDecoder>(std::move(*d));
}

} // namespace transporter::engine
