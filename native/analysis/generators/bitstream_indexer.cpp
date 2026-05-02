#include "analysis/generators/bitstream_indexer.h"
#include "common/win_utf8.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>

namespace vr::analysis {

namespace {

struct UnitSpan {
    int start = 0;
    int prefix = 0;
    int size = 0;
};

bool is_annex_b(const uint8_t* data, int len) {
    return (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) ||
           (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1);
}

std::vector<UnitSpan> find_annex_b_units(const uint8_t* data, int len) {
    std::vector<UnitSpan> starts;
    for (int i = 0; i + 3 < len;) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                starts.push_back({i, 3, 0});
                i += 3;
                continue;
            }
            if (i + 4 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                starts.push_back({i, 4, 0});
                i += 4;
                continue;
            }
        }
        ++i;
    }

    for (size_t i = 0; i < starts.size(); ++i) {
        const int end = (i + 1 < starts.size()) ? starts[i + 1].start : len;
        starts[i].size = end - starts[i].start;
    }
    return starts;
}

template <typename ParseFn>
void append_annex_b_units(const uint8_t* data,
                          int len,
                          bool key_packet,
                          BitstreamIndex& index,
                          ParseFn parse) {
    for (const auto& span : find_annex_b_units(data, len)) {
        VbiEntry entry{};
        entry.offset = index.source_size;
        entry.size = static_cast<uint32_t>(span.size);
        parse(data + span.start + span.prefix,
              span.size - span.prefix,
              key_packet,
              entry);
        index.entries.push_back(entry);
        index.source_size += entry.size;
    }
}

template <typename ParseFn>
void append_length_prefixed_units(const uint8_t* data,
                                  int len,
                                  bool key_packet,
                                  BitstreamIndex& index,
                                  ParseFn parse) {
    int pos = 0;
    while (pos + 4 <= len) {
        const uint32_t unit_len = (static_cast<uint32_t>(data[pos]) << 24) |
                                  (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                  (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                  static_cast<uint32_t>(data[pos + 3]);
        if (unit_len == 0 || unit_len > static_cast<uint32_t>(len - pos - 4)) {
            break;
        }

        VbiEntry entry{};
        entry.offset = index.source_size;
        entry.size = 4 + unit_len;
        parse(data + pos + 4, static_cast<int>(unit_len), key_packet, entry);
        index.entries.push_back(entry);
        index.source_size += entry.size;
        pos += 4 + static_cast<int>(unit_len);
    }
}

void parse_h264(const uint8_t* data, int len, bool key_packet, VbiEntry& entry) {
    if (len <= 0) return;
    const uint8_t nal_type = data[0] & 0x1F;
    entry.nal_type = nal_type;
    entry.layer_id = (data[0] >> 5) & 0x03; // nal_ref_idc, stored compatibly.
    if (nal_type >= 1 && nal_type <= 5) {
        entry.flags |= VBI_FLAG_IS_VCL | VBI_FLAG_IS_SLICE;
    }
    if (nal_type == 5 || key_packet) {
        entry.flags |= VBI_FLAG_IS_KEYFRAME;
    }
}

void parse_hevc(const uint8_t* data, int len, bool key_packet, VbiEntry& entry) {
    if (len < 2) return;
    const uint8_t nal_type = (data[0] >> 1) & 0x3F;
    entry.nal_type = nal_type;
    entry.layer_id = static_cast<uint8_t>(((data[0] & 0x01) << 5) | ((data[1] >> 3) & 0x1F));
    const uint8_t tid_plus1 = data[1] & 0x07;
    entry.temporal_id = tid_plus1 > 0 ? static_cast<uint8_t>(tid_plus1 - 1) : 0;
    if (nal_type <= 31) {
        entry.flags |= VBI_FLAG_IS_VCL | VBI_FLAG_IS_SLICE;
    }
    if ((nal_type >= 16 && nal_type <= 21) || key_packet) {
        entry.flags |= VBI_FLAG_IS_KEYFRAME;
    }
}

void parse_vvc(const uint8_t* data, int len, bool key_packet, VbiEntry& entry) {
    if (len < 2) return;
    entry.layer_id = data[0] & 0x3F;
    entry.nal_type = (data[1] >> 3) & 0x1F;
    const uint8_t tid_plus1 = data[1] & 0x07;
    entry.temporal_id = tid_plus1 > 0 ? static_cast<uint8_t>(tid_plus1 - 1) : 0;
    if (entry.nal_type <= 11) {
        entry.flags |= VBI_FLAG_IS_VCL;
    }
    if (entry.nal_type == 0 || entry.nal_type == 1 || entry.nal_type == 2 ||
        entry.nal_type == 3 || entry.nal_type == 7 || entry.nal_type == 8 ||
        entry.nal_type == 9 || entry.nal_type == 10) {
        entry.flags |= VBI_FLAG_IS_SLICE;
    }
    if (entry.nal_type == 7 || entry.nal_type == 8 || entry.nal_type == 9 || key_packet) {
        entry.flags |= VBI_FLAG_IS_KEYFRAME;
    }
}

bool read_uleb128(const uint8_t* data, int len, int& pos, uint64_t& value) {
    value = 0;
    int shift = 0;
    while (pos < len && shift <= 56) {
        const uint8_t b = data[pos++];
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

void append_av1_obus(const uint8_t* data, int len, bool key_packet, BitstreamIndex& index) {
    int pos = 0;
    while (pos < len) {
        const int start = pos;
        const uint8_t header = data[pos++];
        if (header & 0x80) break;

        const uint8_t obu_type = (header >> 3) & 0x0F;
        const bool has_extension = (header & 0x04) != 0;
        const bool has_size = (header & 0x02) != 0;
        uint8_t temporal_id = 0;
        uint8_t spatial_id = 0;
        if (has_extension) {
            if (pos >= len) break;
            const uint8_t ext = data[pos++];
            temporal_id = (ext >> 5) & 0x07;
            spatial_id = (ext >> 3) & 0x03;
        }

        uint64_t payload_size = static_cast<uint64_t>(len - pos);
        if (has_size && !read_uleb128(data, len, pos, payload_size)) break;
        if (payload_size > static_cast<uint64_t>(len - pos)) break;

        VbiEntry entry{};
        entry.offset = index.source_size;
        entry.size = static_cast<uint32_t>((pos - start) + payload_size);
        entry.nal_type = obu_type;
        entry.temporal_id = temporal_id;
        entry.layer_id = spatial_id;
        if (obu_type == 3 || obu_type == 4 || obu_type == 6) {
            entry.flags |= VBI_FLAG_IS_VCL;
        }
        if (obu_type == 6 || obu_type == 4) {
            entry.flags |= VBI_FLAG_IS_SLICE;
        }
        if (key_packet && (entry.flags & VBI_FLAG_IS_VCL)) {
            entry.flags |= VBI_FLAG_IS_KEYFRAME;
        }
        index.entries.push_back(entry);
        index.source_size += entry.size;
        pos += static_cast<int>(payload_size);
    }
}

void append_mpeg2_units(const uint8_t* data, int len, bool key_packet, BitstreamIndex& index) {
    auto parse = [](const uint8_t* unit, int unit_len, bool packet_key, VbiEntry& entry) {
        if (unit_len <= 0) return;
        const uint8_t code = unit[0];
        entry.nal_type = code;
        if (code == 0x00 || (code >= 0x01 && code <= 0xAF)) {
            entry.flags |= VBI_FLAG_IS_VCL;
        }
        if (code >= 0x01 && code <= 0xAF) {
            entry.flags |= VBI_FLAG_IS_SLICE;
        }
        if (code == 0x00 && unit_len >= 3) {
            const uint16_t bits = static_cast<uint16_t>((unit[1] << 8) | unit[2]);
            const uint8_t picture_coding_type = (bits >> 3) & 0x07;
            if (picture_coding_type == 1) {
                entry.flags |= VBI_FLAG_IS_KEYFRAME;
            }
        }
        if (packet_key && (entry.flags & VBI_FLAG_IS_VCL)) {
            entry.flags |= VBI_FLAG_IS_KEYFRAME;
        }
    };
    append_annex_b_units(data, len, key_packet, index, parse);
}

void append_packet_unit(VbiCodec codec,
                        const uint8_t* data,
                        int len,
                        bool key_packet,
                        BitstreamIndex& index) {
    if (!data || len <= 0) return;
    VbiEntry entry{};
    entry.offset = index.source_size;
    entry.size = static_cast<uint32_t>(len);
    entry.nal_type = 0;
    entry.flags = VBI_FLAG_IS_VCL | VBI_FLAG_IS_SLICE;
    if (key_packet) entry.flags |= VBI_FLAG_IS_KEYFRAME;
    if (codec == VbiCodec::VP9 && len > 0) {
        const bool vp9_key = (data[0] & 0x01) == 0;
        if (vp9_key) entry.flags |= VBI_FLAG_IS_KEYFRAME;
    }
    index.entries.push_back(entry);
    index.source_size += entry.size;
}

std::string lowercase_extension(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

void parse_unit_header(VbiCodec codec,
                       const uint8_t* data,
                       int len,
                       bool key_packet,
                       VbiEntry& entry) {
    switch (codec) {
    case VbiCodec::H264:
        parse_h264(data, len, key_packet, entry);
        break;
    case VbiCodec::HEVC:
        parse_hevc(data, len, key_packet, entry);
        break;
    case VbiCodec::VVC:
        parse_vvc(data, len, key_packet, entry);
        break;
    case VbiCodec::MPEG2:
        if (len > 0) {
            entry.nal_type = data[0];
            if (data[0] == 0x00 || (data[0] >= 0x01 && data[0] <= 0xAF)) {
                entry.flags |= VBI_FLAG_IS_VCL;
            }
            if (data[0] >= 0x01 && data[0] <= 0xAF) {
                entry.flags |= VBI_FLAG_IS_SLICE;
            }
            if (data[0] == 0x00 && len >= 3) {
                const uint16_t bits = static_cast<uint16_t>((data[1] << 8) | data[2]);
                const uint8_t picture_coding_type = (bits >> 3) & 0x07;
                if (picture_coding_type == 1) {
                    entry.flags |= VBI_FLAG_IS_KEYFRAME;
                }
            }
        }
        break;
    default:
        break;
    }
}

void append_streamed_unit(VbiCodec codec,
                          const std::vector<uint8_t>& header,
                          uint64_t unit_size,
                          BitstreamIndex& index) {
    if (unit_size == 0 || unit_size > std::numeric_limits<uint32_t>::max()) {
        return;
    }

    VbiEntry entry{};
    entry.offset = index.source_size;
    entry.size = static_cast<uint32_t>(unit_size);
    parse_unit_header(codec,
                      header.data(),
                      static_cast<int>(header.size()),
                      false,
                      entry);
    index.entries.push_back(entry);
    index.source_size += entry.size;
}

bool emit_streamed_unit(VbiCodec codec,
                        const std::vector<uint8_t>& header,
                        uint64_t unit_size,
                        uint64_t& source_size,
                        const BitstreamIndexer::VbiEntrySink& sink) {
    if (!sink || unit_size == 0 || unit_size > std::numeric_limits<uint32_t>::max()) {
        return true;
    }

    VbiEntry entry{};
    entry.offset = source_size;
    entry.size = static_cast<uint32_t>(unit_size);
    parse_unit_header(codec,
                      header.data(),
                      static_cast<int>(header.size()),
                      false,
                      entry);
    source_size += entry.size;
    return sink(entry);
}

bool index_annex_b_stream_to_sink(std::ifstream& in,
                                  VbiCodec codec,
                                  const BitstreamIndexer::VbiEntrySink& sink,
                                  uint64_t& source_size);

bool index_length_prefixed_stream_to_sink(std::ifstream& in,
                                          VbiCodec codec,
                                          const BitstreamIndexer::VbiEntrySink& sink,
                                          uint64_t& source_size);

bool index_annex_b_stream(std::ifstream& in, VbiCodec codec, BitstreamIndex& index) {
    uint64_t source_size = index.source_size;
    const bool ok = index_annex_b_stream_to_sink(
        in,
        codec,
        [&index](const VbiEntry& entry) {
            index.entries.push_back(entry);
            return true;
        },
        source_size);
    index.source_size = source_size;
    return ok;
}

bool index_annex_b_stream_to_sink(std::ifstream& in,
                                  VbiCodec codec,
                                  const BitstreamIndexer::VbiEntrySink& sink,
                                  uint64_t& source_size) {
    constexpr size_t kChunkSize = 64 * 1024;
    constexpr size_t kHeaderBytes = 16;
    std::array<uint8_t, kChunkSize> chunk{};

    bool has_current = false;
    bool emitted_any = false;
    int current_prefix = 0;
    uint64_t current_size = 0;
    int zero_count = 0;
    std::vector<uint8_t> header;
    header.reserve(kHeaderBytes);

    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto read = in.gcount();
        if (read <= 0) break;

        for (std::streamsize i = 0; i < read; ++i) {
            const uint8_t byte = chunk[static_cast<size_t>(i)];
            const int zeros_before = zero_count;
            if (has_current) {
                ++current_size;
            }

            const bool start_code = byte == 1 && zeros_before >= 2;
            if (start_code) {
                const int prefix = zeros_before >= 3 ? 4 : 3;
                if (has_current && current_size >= static_cast<uint64_t>(prefix)) {
                    current_size -= static_cast<uint64_t>(prefix);
                    if (current_size > static_cast<uint64_t>(current_prefix)) {
                        if (!emit_streamed_unit(codec, header, current_size, source_size, sink)) {
                            return false;
                        }
                        emitted_any = true;
                    }
                }

                has_current = true;
                current_prefix = prefix;
                current_size = static_cast<uint64_t>(prefix);
                header.clear();
                zero_count = 0;
                continue;
            }

            if (byte == 0) {
                ++zero_count;
            } else {
                zero_count = 0;
            }

            if (has_current && current_size > static_cast<uint64_t>(current_prefix) &&
                header.size() < kHeaderBytes) {
                header.push_back(byte);
            }
        }
    }

    if (has_current && current_size > static_cast<uint64_t>(current_prefix)) {
        if (!emit_streamed_unit(codec, header, current_size, source_size, sink)) {
            return false;
        }
        emitted_any = true;
    }
    return emitted_any;
}

bool read_exact(std::istream& in, void* data, std::streamsize size) {
    in.read(static_cast<char*>(data), size);
    return in.gcount() == size;
}

bool skip_exact(std::istream& in, uint64_t size) {
    constexpr size_t kChunkSize = 64 * 1024;
    std::array<char, kChunkSize> scratch{};
    while (size > 0) {
        const auto to_read = static_cast<std::streamsize>(
            std::min<uint64_t>(size, scratch.size()));
        if (!read_exact(in, scratch.data(), to_read)) {
            return false;
        }
        size -= static_cast<uint64_t>(to_read);
    }
    return true;
}

std::vector<uint8_t> read_prefix(std::istream& in, size_t max_bytes) {
    std::vector<uint8_t> prefix(max_bytes);
    in.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    prefix.resize(static_cast<size_t>(std::max<std::streamsize>(0, in.gcount())));
    return prefix;
}

uint32_t read_be32(const uint8_t bytes[4]) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

bool index_length_prefixed_stream(std::ifstream& in, VbiCodec codec, BitstreamIndex& index) {
    uint64_t source_size = index.source_size;
    const bool ok = index_length_prefixed_stream_to_sink(
        in,
        codec,
        [&index](const VbiEntry& entry) {
            index.entries.push_back(entry);
            return true;
        },
        source_size);
    index.source_size = source_size;
    return ok;
}

bool index_length_prefixed_stream_to_sink(std::ifstream& in,
                                          VbiCodec codec,
                                          const BitstreamIndexer::VbiEntrySink& sink,
                                          uint64_t& source_size) {
    constexpr size_t kHeaderBytes = 16;
    bool emitted_any = false;
    uint8_t len_bytes[4] = {};
    while (read_exact(in, len_bytes, sizeof(len_bytes))) {
        const uint32_t unit_len = read_be32(len_bytes);
        if (unit_len == 0) break;

        std::vector<uint8_t> header(std::min<size_t>(unit_len, kHeaderBytes));
        if (!header.empty() &&
            !read_exact(in, header.data(), static_cast<std::streamsize>(header.size()))) {
            return false;
        }

        const uint64_t remaining = static_cast<uint64_t>(unit_len) - header.size();
        if (!skip_exact(in, remaining)) return false;

        if (!emit_streamed_unit(
                codec, header, static_cast<uint64_t>(unit_len) + 4, source_size, sink)) {
            return false;
        }
        emitted_any = true;
    }

    return emitted_any;
}

bool copy_stream(std::istream& in, std::ostream& out, const uint8_t* prefix, size_t prefix_size) {
    constexpr size_t kChunkSize = 64 * 1024;
    std::array<char, kChunkSize> chunk{};
    uint64_t total = 0;

    if (prefix && prefix_size > 0) {
        out.write(reinterpret_cast<const char*>(prefix), static_cast<std::streamsize>(prefix_size));
        total += prefix_size;
    }

    while (in) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto read = in.gcount();
        if (read <= 0) break;
        out.write(chunk.data(), read);
        total += static_cast<uint64_t>(read);
        if (!out) return false;
    }
    return total > 0 && out.good();
}

bool write_length_prefixed_as_annex_b(std::ifstream& in, std::ofstream& out) {
    constexpr size_t kChunkSize = 64 * 1024;
    static constexpr uint8_t kStartCode4[] = {0, 0, 0, 1};
    std::array<char, kChunkSize> chunk{};
    uint8_t len_bytes[4] = {};
    uint64_t total_written = 0;

    while (read_exact(in, len_bytes, sizeof(len_bytes))) {
        uint64_t remaining = read_be32(len_bytes);
        if (remaining == 0) break;

        out.write(reinterpret_cast<const char*>(kStartCode4), sizeof(kStartCode4));
        total_written += sizeof(kStartCode4);

        while (remaining > 0) {
            const auto to_read = static_cast<std::streamsize>(
                std::min<uint64_t>(remaining, chunk.size()));
            if (!read_exact(in, chunk.data(), to_read)) {
                return false;
            }
            out.write(chunk.data(), to_read);
            if (!out) return false;
            remaining -= static_cast<uint64_t>(to_read);
            total_written += static_cast<uint64_t>(to_read);
        }
    }

    return total_written > 0 && out.good();
}

} // namespace

VbiCodec BitstreamIndexer::codec_from_ffmpeg_id(int codec_id) {
    switch (codec_id) {
    case AV_CODEC_ID_H264:       return VbiCodec::H264;
    case AV_CODEC_ID_HEVC:       return VbiCodec::HEVC;
    case AV_CODEC_ID_VVC:        return VbiCodec::VVC;
    case AV_CODEC_ID_AV1:        return VbiCodec::AV1;
    case AV_CODEC_ID_VP9:        return VbiCodec::VP9;
    case AV_CODEC_ID_MPEG2VIDEO: return VbiCodec::MPEG2;
    default:                     return VbiCodec::Unknown;
    }
}

VbiCodec BitstreamIndexer::codec_from_path(const std::string& path) {
    const auto ext = lowercase_extension(path);
    if (ext == ".vvc" || ext == ".266" || ext == ".h266") return VbiCodec::VVC;
    if (ext == ".h265" || ext == ".hevc" || ext == ".265") return VbiCodec::HEVC;
    if (ext == ".h264" || ext == ".avc" || ext == ".264") return VbiCodec::H264;
    return VbiCodec::Unknown;
}

VbiUnitKind BitstreamIndexer::unit_kind_for_codec(VbiCodec codec) {
    switch (codec) {
    case VbiCodec::H264:
    case VbiCodec::HEVC:
    case VbiCodec::VVC:
        return VbiUnitKind::Nalu;
    case VbiCodec::AV1:
        return VbiUnitKind::Obu;
    case VbiCodec::MPEG2:
        return VbiUnitKind::StartCode;
    case VbiCodec::VP9:
        return VbiUnitKind::Packet;
    default:
        return VbiUnitKind::Unknown;
    }
}

void BitstreamIndexer::append_packet(VbiCodec codec,
                                     const uint8_t* data,
                                     int data_len,
                                     bool key_packet,
                                     BitstreamIndex& index) {
    if (!data || data_len <= 0) return;
    index.codec = codec;
    index.unit_kind = unit_kind_for_codec(codec);

    switch (codec) {
    case VbiCodec::H264:
        if (is_annex_b(data, data_len)) {
            append_annex_b_units(data, data_len, key_packet, index, parse_h264);
        } else {
            append_length_prefixed_units(data, data_len, key_packet, index, parse_h264);
        }
        break;
    case VbiCodec::HEVC:
        if (is_annex_b(data, data_len)) {
            append_annex_b_units(data, data_len, key_packet, index, parse_hevc);
        } else {
            append_length_prefixed_units(data, data_len, key_packet, index, parse_hevc);
        }
        break;
    case VbiCodec::VVC:
        if (is_annex_b(data, data_len)) {
            append_annex_b_units(data, data_len, key_packet, index, parse_vvc);
        } else {
            append_length_prefixed_units(data, data_len, key_packet, index, parse_vvc);
        }
        break;
    case VbiCodec::AV1:
        append_av1_obus(data, data_len, key_packet, index);
        break;
    case VbiCodec::MPEG2:
        append_mpeg2_units(data, data_len, key_packet, index);
        break;
    case VbiCodec::VP9:
        append_packet_unit(codec, data, data_len, key_packet, index);
        break;
    default:
        append_packet_unit(codec, data, data_len, key_packet, index);
        break;
    }
}

bool BitstreamIndexer::index_raw_file(const std::string& path,
                                      VbiCodec codec,
                                      BitstreamIndex& index) {
    index = {};
    VbiCodec resolved = VbiCodec::Unknown;
    const bool ok = index_raw_file_streaming(
        path,
        codec,
        [&index](const VbiEntry& entry) {
            index.entries.push_back(entry);
            return true;
        },
        &resolved,
        &index.source_size);
    if (ok) {
        index.codec = resolved;
        index.unit_kind = unit_kind_for_codec(resolved);
    }
    return ok;
}

bool BitstreamIndexer::index_raw_file_streaming(const std::string& path,
                                                VbiCodec codec,
                                                const VbiEntrySink& sink,
                                                VbiCodec* resolved_codec,
                                                uint64_t* source_size) {
    std::ifstream in(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!in) return false;

    if (codec == VbiCodec::Unknown) {
        codec = codec_from_path(path);
    }
    if (codec == VbiCodec::Unknown) {
        return false;
    }

    const auto prefix = read_prefix(in, 4);
    if (prefix.empty()) return false;
    in.clear();
    in.seekg(0, std::ios::beg);

    uint64_t local_source_size = 0;
    uint64_t& total_size = source_size ? *source_size : local_source_size;
    total_size = 0;

    const bool ok = is_annex_b(prefix.data(), static_cast<int>(prefix.size()))
        ? index_annex_b_stream_to_sink(in, codec, sink, total_size)
        : index_length_prefixed_stream_to_sink(in, codec, sink, total_size);
    if (ok && resolved_codec) {
        *resolved_codec = codec;
    }
    return ok;
}

bool BitstreamIndexer::write_annex_b_file(const std::string& path,
                                          VbiCodec codec,
                                          const std::string& output_path) {
    std::ifstream in(win_utf8::path_from_utf8(path), std::ios::binary);
    if (!in) return false;

    if (codec == VbiCodec::Unknown) {
        codec = codec_from_path(path);
    }
    if (codec != VbiCodec::H264 && codec != VbiCodec::HEVC && codec != VbiCodec::VVC) {
        return false;
    }

    std::ofstream out(win_utf8::path_from_utf8(output_path), std::ios::binary);
    if (!out) return false;

    const auto prefix = read_prefix(in, 4);
    if (prefix.empty()) return false;
    if (is_annex_b(prefix.data(), static_cast<int>(prefix.size()))) {
        return copy_stream(in, out, prefix.data(), prefix.size());
    }

    in.clear();
    in.seekg(0, std::ios::beg);
    return write_length_prefixed_as_annex_b(in, out);
}

} // namespace vr::analysis
