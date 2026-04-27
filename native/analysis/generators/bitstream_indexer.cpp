#include "analysis/generators/bitstream_indexer.h"

extern "C" {
#include <libavcodec/codec_id.h>
}

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

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
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;

    if (codec == VbiCodec::Unknown) {
        codec = codec_from_path(path);
    }
    if (codec == VbiCodec::Unknown) {
        return false;
    }

    index = {};
    append_packet(codec, bytes.data(), static_cast<int>(bytes.size()), false, index);
    return !index.entries.empty();
}

bool BitstreamIndexer::write_annex_b_file(const std::string& path,
                                          VbiCodec codec,
                                          const std::string& output_path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;

    if (codec == VbiCodec::Unknown) {
        codec = codec_from_path(path);
    }
    if (codec != VbiCodec::H264 && codec != VbiCodec::HEVC && codec != VbiCodec::VVC) {
        return false;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;

    if (is_annex_b(bytes.data(), static_cast<int>(bytes.size()))) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return out.good();
    }

    static const uint8_t kStartCode4[] = {0, 0, 0, 1};
    int pos = 0;
    uint64_t total_written = 0;
    while (pos + 4 <= static_cast<int>(bytes.size())) {
        const uint32_t unit_len = (static_cast<uint32_t>(bytes[pos]) << 24) |
                                  (static_cast<uint32_t>(bytes[pos + 1]) << 16) |
                                  (static_cast<uint32_t>(bytes[pos + 2]) << 8) |
                                  static_cast<uint32_t>(bytes[pos + 3]);
        if (unit_len == 0 || unit_len > static_cast<uint32_t>(bytes.size() - pos - 4)) {
            break;
        }
        out.write(reinterpret_cast<const char*>(kStartCode4), sizeof(kStartCode4));
        out.write(reinterpret_cast<const char*>(bytes.data() + pos + 4),
                  static_cast<std::streamsize>(unit_len));
        total_written += sizeof(kStartCode4) + unit_len;
        pos += 4 + static_cast<int>(unit_len);
    }

    return total_written > 0 && out.good();
}

} // namespace vr::analysis
