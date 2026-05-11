// SPDX-License-Identifier: GPL-3.0-or-later

#include "decoders.hpp"

#include <transporter/engine/decoder_factory.hpp>
#include <transporter/engine/error.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace transporter::engine {

namespace {

enum class Format : std::uint8_t {
    Unknown,
    Wav,
    Aiff,
    Flac,
    Mp3,
    Vorbis,
    Opus,
    Alac, // includes generic m4a/mp4 — codec confirmed by magic walk
};

const char* format_name(Format f) noexcept {
    switch (f) {
    case Format::Wav:
        return "WAV";
    case Format::Aiff:
        return "AIFF";
    case Format::Flac:
        return "FLAC";
    case Format::Mp3:
        return "MP3";
    case Format::Vorbis:
        return "Vorbis";
    case Format::Opus:
        return "Opus";
    case Format::Alac:
        return "ALAC";
    case Format::Unknown:
        break;
    }
    return "?";
}

std::string lowercase(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return s;
}

Format guess_by_extension(const std::filesystem::path& p) {
    std::string ext = lowercase(p.extension().string());
    if (ext == ".wav" || ext == ".wave") {
        return Format::Wav;
    }
    if (ext == ".aif" || ext == ".aiff" || ext == ".aifc") {
        return Format::Aiff;
    }
    if (ext == ".flac") {
        return Format::Flac;
    }
    if (ext == ".m4a" || ext == ".mp4" || ext == ".alac") {
        return Format::Alac;
    }
    if (ext == ".mp3") {
        return Format::Mp3;
    }
    if (ext == ".opus") {
        return Format::Opus;
    }
    if (ext == ".ogg" || ext == ".oga" || ext == ".ogv") {
        return Format::Unknown; // Ogg container; refine via magic.
    }
    return Format::Unknown;
}

bool starts_with(const std::array<std::uint8_t, 16>& head, std::size_t n,
                 const char* prefix) {
    const std::size_t plen = std::strlen(prefix);
    if (plen > n) {
        return false;
    }
    return std::memcmp(head.data(), prefix, plen) == 0;
}

bool is_mp3_frame_sync(std::uint8_t a, std::uint8_t b) {
    // 11 bits 0xFFE then version+layer+protection+...
    return a == 0xFF && (b & 0xE0) == 0xE0;
}

// For Ogg: figure out whether it's Vorbis or Opus by reading the first
// page's payload. Page header is 27 bytes + segment table. The first
// packet for Vorbis starts with byte 0x01 then "vorbis"; Opus starts with
// "OpusHead".
Format ogg_subtype(std::FILE* fp) {
    // We've already confirmed "OggS" in head. Re-seek to start.
    if (std::fseek(fp, 0, SEEK_SET) != 0) {
        return Format::Unknown;
    }
    std::array<std::uint8_t, 27> page{};
    if (std::fread(page.data(), 1, page.size(), fp) != page.size()) {
        return Format::Unknown;
    }
    const std::uint8_t segs = page[26];
    std::vector<std::uint8_t> seg_table(segs);
    if (segs != 0 && std::fread(seg_table.data(), 1, segs, fp) != segs) {
        return Format::Unknown;
    }
    std::array<std::uint8_t, 16> first_pkt{};
    const std::size_t want = std::min<std::size_t>(first_pkt.size(),
                                                   segs == 0 ? 0u : seg_table[0]);
    if (want != 0 && std::fread(first_pkt.data(), 1, want, fp) != want) {
        return Format::Unknown;
    }
    if (want >= 7 && first_pkt[0] == 0x01 &&
        std::memcmp(first_pkt.data() + 1, "vorbis", 6) == 0) {
        return Format::Vorbis;
    }
    if (want >= 8 && std::memcmp(first_pkt.data(), "OpusHead", 8) == 0) {
        return Format::Opus;
    }
    return Format::Unknown;
}

// MP4: confirm the audio sample-description is `alac`. The full demuxer in
// mp4_demux.cpp is the authority; here we only need a quick gate so detect
// returns the right concrete decoder factory.
bool mp4_appears_alac(std::FILE* fp) {
    if (std::fseek(fp, 0, SEEK_SET) != 0) {
        return false;
    }
    // Read up to 1 MB looking for `stsd` + `alac` ASCII tags. Cheap and
    // sufficient for a detection gate; the demuxer rejects mismatch later.
    constexpr std::size_t SCAN = 1u << 20;
    std::vector<std::uint8_t> buf(SCAN);
    const std::size_t got = std::fread(buf.data(), 1, SCAN, fp);
    if (got < 16) {
        return false;
    }
    bool stsd_seen = false;
    bool alac_seen = false;
    for (std::size_t i = 0; i + 4 <= got; ++i) {
        if (!stsd_seen && std::memcmp(buf.data() + i, "stsd", 4) == 0) {
            stsd_seen = true;
        }
        if (std::memcmp(buf.data() + i, "alac", 4) == 0) {
            alac_seen = true;
            if (stsd_seen) {
                return true;
            }
        }
    }
    return stsd_seen && alac_seen;
}

Format confirm_by_magic(std::FILE* fp, Format hint) {
    std::array<std::uint8_t, 16> head{};
    if (std::fseek(fp, 0, SEEK_SET) != 0) {
        return Format::Unknown;
    }
    const std::size_t n = std::fread(head.data(), 1, head.size(), fp);
    if (n < 4) {
        return Format::Unknown;
    }

    if (n >= 12 && starts_with(head, n, "RIFF") &&
        std::memcmp(head.data() + 8, "WAVE", 4) == 0) {
        return Format::Wav;
    }
    if (n >= 12 && starts_with(head, n, "FORM") &&
        (std::memcmp(head.data() + 8, "AIFF", 4) == 0 ||
         std::memcmp(head.data() + 8, "AIFC", 4) == 0)) {
        return Format::Aiff;
    }
    if (n >= 4 && starts_with(head, n, "fLaC")) {
        return Format::Flac;
    }
    if (n >= 4 && starts_with(head, n, "OggS")) {
        return ogg_subtype(fp);
    }
    if (n >= 3 && starts_with(head, n, "ID3")) {
        return Format::Mp3;
    }
    if (n >= 8 && std::memcmp(head.data() + 4, "ftyp", 4) == 0) {
        if (mp4_appears_alac(fp)) {
            return Format::Alac;
        }
        return Format::Unknown;
    }
    if (n >= 2 && is_mp3_frame_sync(head[0], head[1])) {
        return Format::Mp3;
    }

    // For .mp3 files that begin with a smaller ID3v1 sniff sequence or
    // synced frames not at offset 0, trust the extension hint as a final
    // fallback. We do this only when the hint is itself MP3.
    if (hint == Format::Mp3) {
        return Format::Mp3;
    }
    return Format::Unknown;
}

std::unexpected<Error> mismatch(const std::string& what) {
    return std::unexpected(Error{ErrorCode::FormatNotSupported, what});
}

} // namespace

std::expected<std::unique_ptr<IDecoder>, Error>
open_decoder(const std::filesystem::path& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        std::string msg = "fopen ";
        msg += path.string();
        msg += ": ";
        msg += std::strerror(errno);
        return std::unexpected(Error{ErrorCode::FileOpenFailed, std::move(msg)});
    }

    const Format hint = guess_by_extension(path);
    const Format confirmed = confirm_by_magic(fp, hint);
    std::fclose(fp);

    if (confirmed == Format::Unknown) {
        std::string msg = "format not detected for ";
        msg += path.string();
        if (hint != Format::Unknown) {
            msg += " (extension hinted ";
            msg += format_name(hint);
            msg += ", magic mismatched)";
        }
        return mismatch(msg);
    }

    if (hint != Format::Unknown && hint != confirmed) {
        // .ogg / .oga / .ogv may contain either Vorbis or Opus — allow that
        // mismatch. Any other extension/magic disagreement is refused.
        const std::string ext = lowercase(path.extension().string());
        const bool ext_is_ogg = ext == ".ogg" || ext == ".oga" || ext == ".ogv";
        if (!ext_is_ogg) {
            std::string msg = "extension/magic mismatch: ";
            msg += format_name(hint);
            msg += " vs ";
            msg += format_name(confirmed);
            return mismatch(msg);
        }
    }

    switch (confirmed) {
    case Format::Wav:
        return open_wav_decoder(path);
    case Format::Aiff:
        return open_aiff_decoder(path);
    case Format::Flac:
        return open_flac_decoder(path);
    case Format::Alac:
        return open_alac_decoder(path);
    case Format::Mp3:
        return open_mp3_decoder(path);
    case Format::Vorbis:
        return open_vorbis_decoder(path);
    case Format::Opus:
        return open_opus_decoder(path);
    case Format::Unknown:
        break;
    }
    return mismatch("internal: detected Unknown after confirm");
}

} // namespace transporter::engine
