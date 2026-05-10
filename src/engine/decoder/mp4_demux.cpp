// SPDX-License-Identifier: GPL-3.0-or-later

#include "mp4_demux.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace transporter::engine::mp4 {

namespace {

constexpr std::uint32_t fcc(const char (&s)[5]) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]));
}

constexpr std::uint32_t A_FTYP = fcc("ftyp");
constexpr std::uint32_t A_MOOV = fcc("moov");
constexpr std::uint32_t A_MVHD = fcc("mvhd");
constexpr std::uint32_t A_TRAK = fcc("trak");
constexpr std::uint32_t A_MDIA = fcc("mdia");
constexpr std::uint32_t A_MDHD = fcc("mdhd");
constexpr std::uint32_t A_HDLR = fcc("hdlr");
constexpr std::uint32_t A_MINF = fcc("minf");
constexpr std::uint32_t A_STBL = fcc("stbl");
constexpr std::uint32_t A_STSD = fcc("stsd");
constexpr std::uint32_t A_STTS = fcc("stts");
constexpr std::uint32_t A_STSC = fcc("stsc");
constexpr std::uint32_t A_STSZ = fcc("stsz");
constexpr std::uint32_t A_STCO = fcc("stco");
constexpr std::uint32_t A_CO64 = fcc("co64");
constexpr std::uint32_t A_ALAC = fcc("alac");
constexpr std::uint32_t A_UDTA = fcc("udta");
constexpr std::uint32_t A_META = fcc("meta");
constexpr std::uint32_t A_ILST = fcc("ilst");
constexpr std::uint32_t A_DATA = fcc("data");
constexpr std::uint32_t A_HDLR_SOUN = fcc("soun");
constexpr std::uint32_t A_TRKN = fcc("trkn");
constexpr std::uint32_t A_FREE = fcc("free");
constexpr std::uint32_t A_SKIP = fcc("skip");
constexpr std::uint32_t A_MOOF = fcc("moof");
constexpr std::uint32_t A_MVEX = fcc("mvex");

// "©nam" etc: 0xA9 then three ascii bytes. fcc() takes char[5] but high-bit
// chars confuse signed promotion. Build them directly.
constexpr std::uint32_t make_copyright_id(char a, char b, char c) {
    return (0xA9u << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(c));
}
constexpr std::uint32_t IL_NAM = make_copyright_id('n', 'a', 'm');
constexpr std::uint32_t IL_ART = make_copyright_id('A', 'R', 'T');
constexpr std::uint32_t IL_ALB = make_copyright_id('a', 'l', 'b');
constexpr std::uint32_t IL_DAY = make_copyright_id('d', 'a', 'y');

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
std::uint64_t rd_u64be(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(rd_u32be(p)) << 32) |
           static_cast<std::uint64_t>(rd_u32be(p + 4));
}

std::unexpected<Error> bad(std::string m) {
    return std::unexpected(Error{ErrorCode::WavMalformed, std::move(m)});
}

// Read whole atom body of size `n` at the current fp position into a vec.
std::expected<std::vector<std::uint8_t>, Error> read_body(std::FILE* fp, std::uint64_t n) {
    std::vector<std::uint8_t> body;
    if (n > (1ull << 30)) {
        return bad("MP4 atom body unreasonably large");
    }
    body.resize(static_cast<std::size_t>(n));
    if (n != 0 && std::fread(body.data(), 1, body.size(), fp) != body.size()) {
        return bad("short read on MP4 atom body");
    }
    return body;
}

// Atom header read: returns (type, body_size_or_remainder, abs_end_offset).
struct AtomHeader {
    std::uint32_t type;
    std::uint64_t body_size; // size of body following this header
    std::uint64_t end;       // absolute end offset of the atom
};

std::expected<AtomHeader, Error> read_atom_header(std::FILE* fp) {
    std::array<std::uint8_t, 8> hdr{};
    const auto here_long = std::ftell(fp);
    if (here_long < 0) {
        return bad("ftell failed before atom header");
    }
    const auto here = static_cast<std::uint64_t>(here_long);
    if (std::fread(hdr.data(), 1, hdr.size(), fp) != hdr.size()) {
        return bad("short read on atom header");
    }
    std::uint64_t size = rd_u32be(hdr.data());
    const std::uint32_t type = rd_u32be(hdr.data() + 4);
    std::uint64_t header_size = 8;
    if (size == 1) {
        std::array<std::uint8_t, 8> ext{};
        if (std::fread(ext.data(), 1, ext.size(), fp) != ext.size()) {
            return bad("short read on extended atom size");
        }
        size = rd_u64be(ext.data());
        header_size = 16;
    } else if (size == 0) {
        // Atom extends to EOF. Compute file size.
        const auto cur = std::ftell(fp);
        if (cur < 0) {
            return bad("ftell after type read failed");
        }
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            return bad("fseek end failed");
        }
        const auto end = std::ftell(fp);
        if (end < 0) {
            return bad("ftell end failed");
        }
        size = static_cast<std::uint64_t>(end) - here;
        if (std::fseek(fp, cur, SEEK_SET) != 0) {
            return bad("fseek restore failed");
        }
    }
    if (size < header_size) {
        return bad("atom size smaller than header");
    }
    AtomHeader out{};
    out.type = type;
    out.body_size = size - header_size;
    out.end = here + size;
    return out;
}

struct StszInfo {
    std::uint32_t default_size = 0;
    std::vector<std::uint32_t> sizes; // empty if uniform
};

struct StscEntry {
    std::uint32_t first_chunk;
    std::uint32_t samples_per_chunk;
    std::uint32_t sample_description_index;
};

struct SttsEntry {
    std::uint32_t sample_count;
    std::uint32_t sample_delta;
};

std::expected<StszInfo, Error> parse_stsz(std::span<const std::uint8_t> body) {
    if (body.size() < 12) {
        return bad("stsz too short");
    }
    StszInfo s{};
    s.default_size = rd_u32be(body.data() + 4);
    const std::uint32_t count = rd_u32be(body.data() + 8);
    if (s.default_size == 0) {
        if (body.size() < 12 + 4ull * count) {
            return bad("stsz body shorter than declared count");
        }
        s.sizes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            s.sizes.push_back(rd_u32be(body.data() + 12 + 4 * i));
        }
    } else {
        s.sizes.assign(count, s.default_size);
    }
    return s;
}

std::expected<std::vector<StscEntry>, Error>
parse_stsc(std::span<const std::uint8_t> body) {
    if (body.size() < 8) {
        return bad("stsc too short");
    }
    const std::uint32_t count = rd_u32be(body.data() + 4);
    if (body.size() < 8 + 12ull * count) {
        return bad("stsc body shorter than declared count");
    }
    std::vector<StscEntry> v;
    v.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        StscEntry e{};
        e.first_chunk = rd_u32be(body.data() + 8 + 12 * i);
        e.samples_per_chunk = rd_u32be(body.data() + 8 + 12 * i + 4);
        e.sample_description_index = rd_u32be(body.data() + 8 + 12 * i + 8);
        v.push_back(e);
    }
    return v;
}

std::expected<std::vector<std::uint64_t>, Error>
parse_co64(std::span<const std::uint8_t> body) {
    if (body.size() < 8) {
        return bad("co64 too short");
    }
    const std::uint32_t count = rd_u32be(body.data() + 4);
    if (body.size() < 8 + 8ull * count) {
        return bad("co64 body shorter than declared count");
    }
    std::vector<std::uint64_t> v;
    v.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        v.push_back(rd_u64be(body.data() + 8 + 8 * i));
    }
    return v;
}

std::expected<std::vector<std::uint64_t>, Error>
parse_stco(std::span<const std::uint8_t> body) {
    if (body.size() < 8) {
        return bad("stco too short");
    }
    const std::uint32_t count = rd_u32be(body.data() + 4);
    if (body.size() < 8 + 4ull * count) {
        return bad("stco body shorter than declared count");
    }
    std::vector<std::uint64_t> v;
    v.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        v.push_back(static_cast<std::uint64_t>(rd_u32be(body.data() + 8 + 4 * i)));
    }
    return v;
}

std::expected<std::vector<SttsEntry>, Error>
parse_stts(std::span<const std::uint8_t> body) {
    if (body.size() < 8) {
        return bad("stts too short");
    }
    const std::uint32_t count = rd_u32be(body.data() + 4);
    if (body.size() < 8 + 8ull * count) {
        return bad("stts body shorter than declared count");
    }
    std::vector<SttsEntry> v;
    v.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        SttsEntry e{};
        e.sample_count = rd_u32be(body.data() + 8 + 8 * i);
        e.sample_delta = rd_u32be(body.data() + 8 + 8 * i + 4);
        v.push_back(e);
    }
    return v;
}

// Build flat sample table from STSZ + STSC + chunk-offsets.
std::vector<Sample>
build_samples(const StszInfo& stsz, const std::vector<StscEntry>& stsc,
              const std::vector<std::uint64_t>& chunk_offsets) {
    std::vector<Sample> out;
    out.reserve(stsz.sizes.size());
    if (stsz.sizes.empty() || stsc.empty() || chunk_offsets.empty()) {
        return out;
    }
    std::size_t sample_idx = 0;
    for (std::size_t ci = 0; ci < chunk_offsets.size() && sample_idx < stsz.sizes.size();
         ++ci) {
        const std::uint32_t chunk_one_based = static_cast<std::uint32_t>(ci + 1);
        // Find STSC entry covering this chunk (largest first_chunk <= chunk_one_based).
        std::size_t stsc_idx = 0;
        for (std::size_t k = 0; k < stsc.size(); ++k) {
            if (stsc[k].first_chunk <= chunk_one_based) {
                stsc_idx = k;
            } else {
                break;
            }
        }
        const std::uint32_t spc = stsc[stsc_idx].samples_per_chunk;
        std::uint64_t off = chunk_offsets[ci];
        for (std::uint32_t s = 0; s < spc && sample_idx < stsz.sizes.size(); ++s) {
            Sample sm{};
            sm.offset = off;
            sm.size = stsz.sizes[sample_idx];
            out.push_back(sm);
            off += sm.size;
            ++sample_idx;
        }
    }
    return out;
}

// stbl child parser.
struct StblParts {
    std::vector<std::byte> alac_cookie;
    std::uint32_t sample_rate_hz = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits_per_sample = 0;
    StszInfo stsz{};
    std::vector<StscEntry> stsc{};
    std::vector<std::uint64_t> chunk_offsets{};
    std::vector<SttsEntry> stts{};
};

// stsd entry walk. Each entry: 8-byte size+type, 6 reserved + u16 ref-index,
// then per-codec extension. For audio sample-description we have AudioSampleEntry
// (v0 or v1) followed by a child `alac` atom carrying the magic cookie.
std::expected<void, Error> parse_audio_sample_entry(std::span<const std::uint8_t> body,
                                                   StblParts& out) {
    // body starts at the atom size+type header. AudioSampleEntry extension
    // follows after 8 bytes of size+type plus 8 bytes of SampleEntry preamble
    // (6 reserved + 2 data_reference_index). Layout from ISO/IEC 14496-12:
    //   +16  uint16 reserved (8 bytes total here)
    //   +24  uint16 channels
    //   +26  uint16 sample_size
    //   +28  uint16 pre_defined
    //   +30  uint16 reserved
    //   +32  uint32 samplerate (16.16 fixed)
    //   +36  child atoms
    if (body.size() < 36) {
        return bad("audio sample entry shorter than 36 bytes");
    }
    out.channels = rd_u16be(body.data() + 24);
    out.bits_per_sample = rd_u16be(body.data() + 26);
    out.sample_rate_hz = rd_u32be(body.data() + 32) >> 16;

    std::size_t off = 36;
    // V1 audio sample entries (rare) and V2 (very rare for ALAC) have
    // additional fields. We do not detect them here — ALAC writers in the
    // wild use the plain v0 layout. Anything past offset 36 we treat as
    // child atoms.

    // Walk child atoms inside the audio sample entry, looking for `alac`.
    while (off + 8 <= body.size()) {
        const std::uint32_t sz = rd_u32be(body.data() + off);
        const std::uint32_t ty = rd_u32be(body.data() + off + 4);
        if (sz < 8 || off + sz > body.size()) {
            break;
        }
        if (ty == A_ALAC) {
            // Per ALAC spec, the child `alac` atom inside the audio sample
            // entry holds: 4 bytes flags + ALACSpecificConfig (24 bytes for
            // v0) and optionally a chan atom. Some files double-wrap with
            // another `alac` (legacy QT). Strip leading 4 flag bytes if present.
            std::size_t cookie_start = off + 8;
            std::size_t cookie_end = off + sz;
            if (cookie_end > cookie_start + 4) {
                // Many writers store: full-atom flags(4) + cookie. The ALAC
                // ref decoder accepts the cookie starting from the
                // ALACSpecificConfig block. Skip the 4 flag bytes.
                // Verify the next 4 bytes look like a frame_length field
                // (typically 0x00001000 = 4096), but don't enforce.
                cookie_start += 4;
            }
            // Possible nested `alac` wrapper (some QT writers): if first 8
            // bytes look like a child atom whose type is `alac`, descend.
            if (cookie_end >= cookie_start + 8) {
                const std::uint32_t inner_sz = rd_u32be(body.data() + cookie_start);
                const std::uint32_t inner_ty = rd_u32be(body.data() + cookie_start + 4);
                if (inner_ty == A_ALAC && inner_sz >= 12 &&
                    cookie_start + inner_sz <= cookie_end) {
                    cookie_start += 8;
                    // Skip another 4 flag bytes if present.
                    if (cookie_end >= cookie_start + 4) {
                        cookie_start += 4;
                    }
                }
            }
            if (cookie_end > cookie_start) {
                out.alac_cookie.assign(
                    reinterpret_cast<const std::byte*>(body.data() + cookie_start),
                    reinterpret_cast<const std::byte*>(body.data() + cookie_end));
            }
            return {};
        }
        off += sz;
    }
    return bad("stsd: ALAC config atom not found");
}

std::expected<void, Error> parse_stsd(std::span<const std::uint8_t> body, StblParts& out) {
    if (body.size() < 8) {
        return bad("stsd too short");
    }
    const std::uint32_t count = rd_u32be(body.data() + 4);
    std::size_t off = 8;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (off + 8 > body.size()) {
            return bad("stsd entry truncated");
        }
        const std::uint32_t sz = rd_u32be(body.data() + off);
        const std::uint32_t ty = rd_u32be(body.data() + off + 4);
        if (sz < 16 || off + sz > body.size()) {
            return bad("stsd entry size out of range");
        }
        if (ty == A_ALAC) {
            auto r = parse_audio_sample_entry(
                std::span<const std::uint8_t>{body.data() + off, sz}, out);
            if (!r) {
                return r;
            }
            return {};
        }
        off += sz;
    }
    return std::unexpected(Error{ErrorCode::WavUnsupportedTag,
                                 "MP4 audio sample entry is not 'alac' "
                                 "(this decoder is ALAC-only)"});
}

std::expected<void, Error> parse_stbl(std::FILE* fp, std::uint64_t end, StblParts& out) {
    while (true) {
        const auto tell = std::ftell(fp);
        if (tell < 0) {
            return bad("ftell in stbl failed");
        }
        if (static_cast<std::uint64_t>(tell) >= end) {
            break;
        }
        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->end > end) {
            return bad("stbl child overruns parent");
        }
        if (h->type == A_STSD) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            auto r = parse_stsd(std::span<const std::uint8_t>(*body), out);
            if (!r) {
                return r;
            }
        } else if (h->type == A_STSZ) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            auto r = parse_stsz(std::span<const std::uint8_t>(*body));
            if (!r) {
                return std::unexpected(r.error());
            }
            out.stsz = std::move(*r);
        } else if (h->type == A_STSC) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            auto r = parse_stsc(std::span<const std::uint8_t>(*body));
            if (!r) {
                return std::unexpected(r.error());
            }
            out.stsc = std::move(*r);
        } else if (h->type == A_STCO) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            auto r = parse_stco(std::span<const std::uint8_t>(*body));
            if (!r) {
                return std::unexpected(r.error());
            }
            out.chunk_offsets = std::move(*r);
        } else if (h->type == A_CO64) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            auto r = parse_co64(std::span<const std::uint8_t>(*body));
            if (!r) {
                return std::unexpected(r.error());
            }
            out.chunk_offsets = std::move(*r);
        } else if (h->type == A_STTS) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            auto r = parse_stts(std::span<const std::uint8_t>(*body));
            if (!r) {
                return std::unexpected(r.error());
            }
            out.stts = std::move(*r);
        } else {
            if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
                return bad("fseek over stbl child failed");
            }
        }
    }
    return {};
}

std::expected<void, Error> parse_minf(std::FILE* fp, std::uint64_t end, StblParts& out) {
    while (true) {
        const auto tell = std::ftell(fp);
        if (tell < 0) {
            return bad("ftell minf failed");
        }
        if (static_cast<std::uint64_t>(tell) >= end) {
            break;
        }
        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->end > end) {
            return bad("minf child overruns parent");
        }
        if (h->type == A_STBL) {
            auto r = parse_stbl(fp, h->end, out);
            if (!r) {
                return r;
            }
        } else if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
            return bad("fseek over minf child failed");
        }
    }
    return {};
}

std::expected<void, Error> parse_mdia(std::FILE* fp, std::uint64_t end,
                                      bool& is_audio, std::uint32_t& mdia_timescale,
                                      std::uint64_t& mdia_duration, StblParts& out) {
    while (true) {
        const auto tell = std::ftell(fp);
        if (tell < 0) {
            return bad("ftell mdia failed");
        }
        if (static_cast<std::uint64_t>(tell) >= end) {
            break;
        }
        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->end > end) {
            return bad("mdia child overruns parent");
        }
        if (h->type == A_MDHD) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            if (body->size() < 4) {
                return bad("mdhd too short");
            }
            const std::uint8_t v = (*body)[0];
            if (v == 1) {
                if (body->size() < 4 + 8 + 8 + 4 + 8) {
                    return bad("mdhd v1 truncated");
                }
                mdia_timescale = rd_u32be(body->data() + 4 + 8 + 8);
                mdia_duration = rd_u64be(body->data() + 4 + 8 + 8 + 4);
            } else {
                if (body->size() < 4 + 4 + 4 + 4 + 4) {
                    return bad("mdhd v0 truncated");
                }
                mdia_timescale = rd_u32be(body->data() + 4 + 4 + 4);
                mdia_duration = rd_u32be(body->data() + 4 + 4 + 4 + 4);
            }
        } else if (h->type == A_HDLR) {
            auto body = read_body(fp, h->body_size);
            if (!body) {
                return std::unexpected(body.error());
            }
            if (body->size() >= 12) {
                const std::uint32_t handler = rd_u32be(body->data() + 8);
                if (handler == A_HDLR_SOUN) {
                    is_audio = true;
                }
            }
        } else if (h->type == A_MINF) {
            auto r = parse_minf(fp, h->end, out);
            if (!r) {
                return r;
            }
        } else if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
            return bad("fseek over mdia child failed");
        }
    }
    return {};
}

void apply_ilst_data(std::uint32_t key, std::span<const std::uint8_t> data, Tags& out) {
    // iTunes data atom: 4 bytes type indicator + 4 reserved + payload.
    if (data.size() < 8) {
        return;
    }
    std::span<const std::uint8_t> payload{data.data() + 8, data.size() - 8};
    auto take_text = [&]() {
        return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
    };
    switch (key) {
    case IL_NAM:
        out.title = take_text();
        break;
    case IL_ART:
        out.artist = take_text();
        break;
    case IL_ALB:
        out.album = take_text();
        break;
    case IL_DAY:
        out.date = take_text();
        break;
    case A_TRKN:
        if (payload.size() >= 6) {
            // 2 reserved, then u16 track number, u16 total tracks.
            const std::uint16_t track = rd_u16be(payload.data() + 2);
            out.track_no = std::to_string(track);
        }
        break;
    default:
        break;
    }
}

std::expected<void, Error> parse_ilst(std::FILE* fp, std::uint64_t end, Tags& tags) {
    while (true) {
        const auto tell = std::ftell(fp);
        if (tell < 0) {
            return bad("ftell ilst failed");
        }
        if (static_cast<std::uint64_t>(tell) >= end) {
            break;
        }
        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->end > end) {
            return bad("ilst child overruns");
        }
        // Each ilst child holds a `data` atom child.
        auto body = read_body(fp, h->body_size);
        if (!body) {
            return std::unexpected(body.error());
        }
        // Walk children of this ilst entry to find `data`.
        std::size_t off = 0;
        while (off + 8 <= body->size()) {
            const std::uint32_t sz = rd_u32be(body->data() + off);
            const std::uint32_t ty = rd_u32be(body->data() + off + 4);
            if (sz < 8 || off + sz > body->size()) {
                break;
            }
            if (ty == A_DATA) {
                std::span<const std::uint8_t> data{body->data() + off + 8, sz - 8};
                apply_ilst_data(h->type, data, tags);
            }
            off += sz;
        }
    }
    return {};
}

std::expected<void, Error> parse_meta(std::FILE* fp, std::uint64_t end, Tags& tags) {
    // `meta` is a fullbox: 4 bytes (version+flags) before children.
    std::array<std::uint8_t, 4> vh{};
    if (std::fread(vh.data(), 1, 4, fp) != 4) {
        return bad("meta version+flags");
    }
    while (true) {
        const auto tell = std::ftell(fp);
        if (tell < 0) {
            return bad("ftell meta failed");
        }
        if (static_cast<std::uint64_t>(tell) >= end) {
            break;
        }
        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->end > end) {
            return bad("meta child overruns");
        }
        if (h->type == A_ILST) {
            auto r = parse_ilst(fp, h->end, tags);
            if (!r) {
                return r;
            }
        } else if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
            return bad("fseek over meta child failed");
        }
    }
    return {};
}

std::expected<void, Error> parse_udta(std::FILE* fp, std::uint64_t end, Tags& tags) {
    while (true) {
        const auto tell = std::ftell(fp);
        if (tell < 0) {
            return bad("ftell udta failed");
        }
        if (static_cast<std::uint64_t>(tell) >= end) {
            break;
        }
        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->end > end) {
            return bad("udta child overruns");
        }
        if (h->type == A_META) {
            auto r = parse_meta(fp, h->end, tags);
            if (!r) {
                return r;
            }
        } else if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
            return bad("fseek over udta child failed");
        }
    }
    return {};
}

} // namespace

std::expected<ParseResult, Error> parse(std::FILE* fp) {
    if (std::fseek(fp, 0, SEEK_SET) != 0) {
        return bad("rewind failed");
    }
    bool ftyp_seen = false;
    bool moov_seen = false;
    ParseResult result{};
    StblParts stbl{};
    bool track_is_audio = false;
    bool any_track_processed = false;
    std::uint32_t mdia_timescale = 0;
    std::uint64_t mdia_duration = 0;

    while (true) {
        const auto here = std::ftell(fp);
        if (here < 0) {
            break;
        }
        std::array<std::uint8_t, 8> peek{};
        const auto got = std::fread(peek.data(), 1, peek.size(), fp);
        if (got == 0) {
            break;
        }
        if (got != 8) {
            return bad("short read on top-level header");
        }
        if (std::fseek(fp, here, SEEK_SET) != 0) {
            return bad("fseek restore at top level");
        }

        auto h = read_atom_header(fp);
        if (!h) {
            return std::unexpected(h.error());
        }
        if (h->type == A_FTYP) {
            ftyp_seen = true;
            if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
                return bad("fseek over ftyp");
            }
        } else if (h->type == A_MOOF || h->type == A_MVEX) {
            return std::unexpected(Error{ErrorCode::WavUnsupportedTag,
                                         "fragmented MP4 not supported"});
        } else if (h->type == A_MOOV) {
            moov_seen = true;
            const std::uint64_t moov_end = h->end;
            while (true) {
                const auto t = std::ftell(fp);
                if (t < 0 || static_cast<std::uint64_t>(t) >= moov_end) {
                    break;
                }
                auto ch = read_atom_header(fp);
                if (!ch) {
                    return std::unexpected(ch.error());
                }
                if (ch->end > moov_end) {
                    return bad("moov child overruns");
                }
                if (ch->type == A_MVHD) {
                    auto body = read_body(fp, ch->body_size);
                    if (!body) {
                        return std::unexpected(body.error());
                    }
                    if (body->size() < 4) {
                        return bad("mvhd too short");
                    }
                    const std::uint8_t v = (*body)[0];
                    if (v == 1) {
                        if (body->size() >= 4 + 8 + 8 + 4) {
                            result.timescale = rd_u32be(body->data() + 4 + 8 + 8);
                        }
                    } else if (body->size() >= 4 + 4 + 4 + 4) {
                        result.timescale = rd_u32be(body->data() + 4 + 4 + 4);
                    }
                } else if (ch->type == A_TRAK) {
                    if (any_track_processed) {
                        // Skip additional tracks; we only handle one audio track.
                        if (std::fseek(fp, static_cast<long>(ch->end), SEEK_SET) != 0) {
                            return bad("fseek over extra trak");
                        }
                        continue;
                    }
                    // Walk this trak in place.
                    const std::uint64_t trak_end = ch->end;
                    StblParts local_stbl{};
                    bool local_audio = false;
                    std::uint32_t local_mdia_ts = 0;
                    std::uint64_t local_mdia_dur = 0;
                    while (true) {
                        const auto tt = std::ftell(fp);
                        if (tt < 0 || static_cast<std::uint64_t>(tt) >= trak_end) {
                            break;
                        }
                        auto tch = read_atom_header(fp);
                        if (!tch) {
                            return std::unexpected(tch.error());
                        }
                        if (tch->end > trak_end) {
                            return bad("trak child overruns");
                        }
                        if (tch->type == A_MDIA) {
                            auto r = parse_mdia(fp, tch->end, local_audio, local_mdia_ts,
                                                local_mdia_dur, local_stbl);
                            if (!r) {
                                return std::unexpected(r.error());
                            }
                        } else if (std::fseek(fp, static_cast<long>(tch->end), SEEK_SET) != 0) {
                            return bad("fseek over trak child");
                        }
                    }
                    if (local_audio) {
                        track_is_audio = true;
                        any_track_processed = true;
                        stbl = std::move(local_stbl);
                        mdia_timescale = local_mdia_ts;
                        mdia_duration = local_mdia_dur;
                    }
                } else if (ch->type == A_UDTA) {
                    auto r = parse_udta(fp, ch->end, result.tags);
                    if (!r) {
                        return std::unexpected(r.error());
                    }
                } else if (std::fseek(fp, static_cast<long>(ch->end), SEEK_SET) != 0) {
                    return bad("fseek over moov child");
                }
            }
        } else if (h->type == A_FREE || h->type == A_SKIP) {
            if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
                return bad("fseek over free/skip");
            }
        } else {
            // Skip mdat etc. at the top level until we have moov.
            if (std::fseek(fp, static_cast<long>(h->end), SEEK_SET) != 0) {
                break;
            }
        }
    }

    if (!ftyp_seen) {
        return bad("no ftyp atom");
    }
    if (!moov_seen) {
        return bad("no moov atom");
    }
    if (!track_is_audio) {
        return std::unexpected(Error{ErrorCode::WavUnsupportedTag,
                                     "no audio track found in MP4"});
    }
    if (stbl.alac_cookie.empty()) {
        return std::unexpected(Error{ErrorCode::WavUnsupportedTag,
                                     "MP4 audio is not ALAC"});
    }
    if (stbl.stsz.sizes.empty()) {
        return bad("stsz missing or empty");
    }
    if (stbl.chunk_offsets.empty()) {
        return bad("stco/co64 missing or empty");
    }
    if (stbl.stsc.empty()) {
        return bad("stsc missing or empty");
    }

    result.alac_magic_cookie = std::move(stbl.alac_cookie);
    result.sample_rate_hz = stbl.sample_rate_hz;
    result.channels = stbl.channels;
    result.bits_per_sample = stbl.bits_per_sample;
    result.samples = build_samples(stbl.stsz, stbl.stsc, stbl.chunk_offsets);
    if (result.samples.empty()) {
        return bad("MP4 sample table produced zero samples");
    }
    result.duration_in_timescale = mdia_duration;
    if (mdia_timescale != 0) {
        result.timescale = mdia_timescale;
    }
    if (result.sample_rate_hz == 0 && mdia_timescale != 0) {
        // Fallback: trak/mdia timescale is the audio rate when stsd left zero.
        result.sample_rate_hz = mdia_timescale;
    }
    if (!stbl.stts.empty()) {
        std::uint32_t per_packet = 0;
        bool uniform = true;
        for (const auto& e : stbl.stts) {
            if (per_packet == 0) {
                per_packet = e.sample_delta;
            } else if (per_packet != e.sample_delta) {
                uniform = false;
                break;
            }
        }
        if (uniform) {
            result.default_samples_per_packet = per_packet;
        }
    }
    return result;
}

} // namespace transporter::engine::mp4
