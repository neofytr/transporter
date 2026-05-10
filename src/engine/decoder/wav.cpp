// SPDX-License-Identifier: GPL-3.0-or-later

#include <transporter/engine/wav.hpp>

#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace transporter::engine {

namespace {

// WAVE format tags from the standard.
constexpr std::uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr std::uint16_t WAVE_FORMAT_IEEE_FLOAT = 0x0003;
constexpr std::uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

// 16-byte GUIDs used inside WAVE_FORMAT_EXTENSIBLE's SubFormat field. Stored
// little-endian on disk; we compare byte-for-byte.
constexpr std::array<std::uint8_t, 16> SUBFORMAT_PCM = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
};
constexpr std::array<std::uint8_t, 16> SUBFORMAT_IEEE_FLOAT = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
};

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3])) << 24);
}

constexpr std::uint32_t ID_RIFF = fourcc("RIFF");
constexpr std::uint32_t ID_WAVE = fourcc("WAVE");
constexpr std::uint32_t ID_FMT_ = fourcc("fmt ");
constexpr std::uint32_t ID_DATA = fourcc("data");

std::uint16_t rd_u16le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}
std::uint32_t rd_u32le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::unexpected<Error> malformed(std::string m) {
    return std::unexpected(Error{ErrorCode::WavMalformed, std::move(m)});
}

std::unexpected<Error> unsupported(std::string m) {
    return std::unexpected(Error{ErrorCode::WavUnsupportedTag, std::move(m)});
}

struct FmtChunk {
    std::uint16_t format_tag;
    std::uint16_t channels;
    std::uint32_t sample_rate;
    std::uint32_t byte_rate;
    std::uint16_t block_align;
    std::uint16_t bits_per_sample;
    std::uint16_t valid_bits_per_sample; // EXTENSIBLE only
    std::array<std::uint8_t, 16> subformat_guid; // EXTENSIBLE only
    bool is_extensible;
};

std::expected<FmtChunk, Error> parse_fmt(std::span<const std::uint8_t> body) {
    if (body.size() < 16) {
        return malformed("fmt chunk shorter than 16 bytes");
    }
    FmtChunk f{};
    f.format_tag = rd_u16le(body.data() + 0);
    f.channels = rd_u16le(body.data() + 2);
    f.sample_rate = rd_u32le(body.data() + 4);
    f.byte_rate = rd_u32le(body.data() + 8);
    f.block_align = rd_u16le(body.data() + 12);
    f.bits_per_sample = rd_u16le(body.data() + 14);
    f.is_extensible = false;

    if (f.format_tag == WAVE_FORMAT_EXTENSIBLE) {
        // EXTENSIBLE: 16-byte base + 2 cbSize + 22 extension = 40 total
        if (body.size() < 40) {
            return malformed("WAVE_FORMAT_EXTENSIBLE chunk shorter than 40 bytes");
        }
        const std::uint16_t cb_size = rd_u16le(body.data() + 16);
        if (cb_size < 22) {
            return malformed("WAVE_FORMAT_EXTENSIBLE cbSize < 22");
        }
        f.valid_bits_per_sample = rd_u16le(body.data() + 18);
        // skip 4-byte channel mask at +20..+24
        std::memcpy(f.subformat_guid.data(), body.data() + 24, 16);
        f.is_extensible = true;
    }

    if (f.channels == 0) {
        return malformed("fmt: channels == 0");
    }
    if (f.sample_rate == 0) {
        return malformed("fmt: sample_rate == 0");
    }
    if (f.block_align == 0) {
        return malformed("fmt: block_align == 0");
    }
    return f;
}

std::expected<SampleFormat, Error> resolve_sample_format(const FmtChunk& f) {
    std::uint16_t effective_tag = f.format_tag;
    if (f.is_extensible) {
        if (f.subformat_guid == SUBFORMAT_PCM) {
            effective_tag = WAVE_FORMAT_PCM;
        } else if (f.subformat_guid == SUBFORMAT_IEEE_FLOAT) {
            effective_tag = WAVE_FORMAT_IEEE_FLOAT;
        } else {
            return unsupported("WAVE_FORMAT_EXTENSIBLE GUID is not PCM or IEEE_FLOAT");
        }
    }

    const std::uint16_t bits = f.is_extensible && f.valid_bits_per_sample != 0
                                   ? f.valid_bits_per_sample
                                   : f.bits_per_sample;
    const std::uint16_t container_bits = f.bits_per_sample;

    if (effective_tag == WAVE_FORMAT_PCM) {
        if (bits == 16 && container_bits == 16) {
            return SampleFormat::S16_LE;
        }
        if (bits == 24 && container_bits == 24) {
            return SampleFormat::S24_3LE;
        }
        if (bits == 24 && container_bits == 32) {
            return SampleFormat::S24_LE;
        }
        if (bits == 32 && container_bits == 32) {
            return SampleFormat::S32_LE;
        }
        std::string msg = "PCM bits=";
        msg += std::to_string(bits);
        msg += " container=";
        msg += std::to_string(container_bits);
        msg += " not handled";
        return unsupported(std::move(msg));
    }

    if (effective_tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (bits == 32 && container_bits == 32) {
            return SampleFormat::FLOAT_LE;
        }
        std::string msg = "IEEE_FLOAT bits=";
        msg += std::to_string(bits);
        msg += " container=";
        msg += std::to_string(container_bits);
        msg += " not handled";
        return unsupported(std::move(msg));
    }

    std::string msg = "wFormatTag 0x";
    char buf[5];
    std::snprintf(buf, sizeof buf, "%04X", effective_tag);
    msg += buf;
    msg += " refused (uLaw, aLaw, ADPCM, MS-PCM, etc. are not bit-perfect)";
    return unsupported(std::move(msg));
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
    FileHandle& operator=(FileHandle&&) = delete;
    std::FILE* get() const noexcept { return f_; }

private:
    std::FILE* f_;
};

} // namespace

std::expected<WavFile, Error> load_wav(const std::filesystem::path& path) {
    FileHandle fh{std::fopen(path.c_str(), "rb")};
    if (!fh.get()) {
        std::string msg = "fopen ";
        msg += path.string();
        msg += ": ";
        msg += std::strerror(errno);
        return std::unexpected(Error{ErrorCode::FileOpenFailed, std::move(msg)});
    }
    auto* fp = fh.get();

    // RIFF header: 'RIFF' <size:u32> 'WAVE'
    std::array<std::uint8_t, 12> hdr{};
    if (std::fread(hdr.data(), 1, hdr.size(), fp) != hdr.size()) {
        return malformed("file too short for RIFF header");
    }
    if (rd_u32le(hdr.data()) != ID_RIFF) {
        return malformed("missing RIFF magic");
    }
    if (rd_u32le(hdr.data() + 8) != ID_WAVE) {
        return malformed("missing WAVE magic");
    }

    // Chunk walk. Each chunk: 4-byte id, 4-byte little-endian size, body, then
    // pad byte if size is odd.
    std::expected<FmtChunk, Error> fmt_result = std::unexpected(
        Error{ErrorCode::WavMalformed, "fmt chunk not found"});
    bool fmt_seen = false;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    bool data_seen = false;

    while (!data_seen) {
        std::array<std::uint8_t, 8> chunk_hdr{};
        const auto got = std::fread(chunk_hdr.data(), 1, chunk_hdr.size(), fp);
        if (got == 0) {
            break;
        }
        if (got != chunk_hdr.size()) {
            return malformed("truncated chunk header");
        }
        const std::uint32_t id = rd_u32le(chunk_hdr.data());
        const std::uint32_t sz = rd_u32le(chunk_hdr.data() + 4);

        if (id == ID_FMT_) {
            std::vector<std::uint8_t> body(sz);
            if (sz != 0 && std::fread(body.data(), 1, sz, fp) != sz) {
                return malformed("truncated fmt chunk body");
            }
            auto parsed = parse_fmt(body);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            fmt_result = parsed;
            fmt_seen = true;
            if ((sz & 1u) != 0u) {
                std::fseek(fp, 1, SEEK_CUR);
            }
        } else if (id == ID_DATA) {
            if (!fmt_seen) {
                return malformed("data chunk before fmt chunk");
            }
            data_offset = static_cast<std::uint64_t>(std::ftell(fp));
            data_size = sz;
            data_seen = true;
            // Don't advance: we'll seek back to data_offset after the loop to
            // read the body.
            break;
        } else {
            // Skip unknown chunk + pad.
            const long advance = static_cast<long>(sz) + ((sz & 1u) != 0u ? 1 : 0);
            if (std::fseek(fp, advance, SEEK_CUR) != 0) {
                return malformed("fseek over unknown chunk failed");
            }
        }
    }

    if (!fmt_seen) {
        return malformed("fmt chunk not found");
    }
    if (!data_seen) {
        return malformed("data chunk not found");
    }

    const FmtChunk& fmt = *fmt_result;
    auto sample_fmt = resolve_sample_format(fmt);
    if (!sample_fmt) {
        return std::unexpected(sample_fmt.error());
    }

    PcmFormat out_fmt{};
    out_fmt.sample_rate_hz = fmt.sample_rate;
    out_fmt.channels = fmt.channels;
    out_fmt.sample_format = *sample_fmt;

    const unsigned frame_bytes = out_fmt.frame_bytes();
    if (frame_bytes == 0) {
        return malformed("computed frame_bytes == 0");
    }
    if ((data_size % frame_bytes) != 0) {
        return malformed("data chunk size not a multiple of frame size");
    }

    WavFile wav{};
    wav.format = out_fmt;
    wav.total_frames = data_size / frame_bytes;
    wav.samples.resize(data_size);

    if (std::fseek(fp, static_cast<long>(data_offset), SEEK_SET) != 0) {
        return malformed("fseek to data body failed");
    }
    if (data_size != 0 &&
        std::fread(wav.samples.data(), 1, data_size, fp) != data_size) {
        return malformed("truncated data body");
    }

    return wav;
}

} // namespace transporter::engine
