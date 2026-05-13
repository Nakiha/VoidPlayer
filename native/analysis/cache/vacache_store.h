#pragma once

#include "analysis/parsers/vac2_parser.h"
#include "analysis/parsers/vachunk_parser.h"

#include <cstdint>
#include <string>

namespace vr::analysis {

struct VachunkKey {
    VachunkKind kind = VachunkKind::Unknown;
    AnalysisCodec codec = AnalysisCodec::Unknown;
    uint64_t feature_flags = 0;
    uint64_t base_content_revision = 0;
    uint64_t generator_revision = 0;
    uint32_t start_frame = UINT32_MAX;
    uint32_t end_frame = UINT32_MAX;
    uint32_t start_packet = UINT32_MAX;
    uint32_t end_packet = UINT32_MAX;
    uint32_t start_unit = UINT32_MAX;
    uint32_t end_unit = UINT32_MAX;
};

class VacacheStore {
public:
    VacacheStore(std::string cache_root, std::string hash);

    const std::string& cache_root() const { return cache_root_; }
    const std::string& hash() const { return hash_; }

    std::string hash_dir() const;
    std::string base_path() const;
    std::string tmp_dir() const;
    std::string chunks_root() const;
    std::string chunks_dir(VachunkKind kind) const;
    std::string chunk_path(const VachunkKey& key) const;

    bool ensure_layout() const;
    bool write_base_atomic(const Vac2BaseData& data,
                           uint64_t max_output_bytes = 0) const;
    bool open_base(Vac2BaseFile& out) const;

    bool write_chunk_atomic(const VachunkKey& key,
                            VachunkData data,
                            uint64_t max_output_bytes = 0) const;
    bool open_chunk(const VachunkKey& key, VachunkFile& out) const;

private:
    std::string cache_root_;
    std::string hash_;
};

const char* vachunk_kind_name(VachunkKind kind);

} // namespace vr::analysis
