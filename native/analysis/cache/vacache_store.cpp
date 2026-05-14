#include "analysis/cache/vacache_store.h"
#include "common/win_utf8.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <spdlog/spdlog.h>

namespace vr::analysis {
namespace {

std::filesystem::path path_from_utf8_join(const std::string& lhs,
                                          const std::string& rhs) {
    return win_utf8::path_from_utf8(lhs) / win_utf8::path_from_utf8(rhs);
}

std::string path_to_utf8_string(const std::filesystem::path& path) {
    return win_utf8::path_to_utf8(path);
}

bool create_dir_utf8(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(win_utf8::path_from_utf8(path), ec);
    return !ec;
}

std::string hex_u64(uint64_t value) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << value;
    return ss.str();
}

uint64_t fnv1a64(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : value) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t current_process_id_token() {
#ifdef _WIN32
    return static_cast<uint64_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

std::string unique_tmp_path_for(const std::string& tmp_dir,
                                const std::string& final_path,
                                const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t thread_id =
        static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const uint64_t time_token = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    std::ostringstream name;
    name << prefix
         << "_" << hex_u64(fnv1a64(final_path))
         << "_p" << hex_u64(current_process_id_token())
         << "_t" << hex_u64(thread_id)
         << "_c" << hex_u64(sequence)
         << "_n" << hex_u64(time_token)
         << ".tmp";
    return path_to_utf8_string(
        win_utf8::path_from_utf8(tmp_dir) / win_utf8::path_from_utf8(name.str()));
}

bool replace_file_utf8(const std::string& tmp_path, const std::string& final_path) {
#ifdef _WIN32
    const auto tmp_wide = win_utf8::utf16_from_utf8(tmp_path);
    const auto final_wide = win_utf8::utf16_from_utf8(final_path);
    if (tmp_wide.empty() || final_wide.empty()) {
        win_utf8::delete_file_utf8(tmp_path);
        return false;
    }
    const DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    if (MoveFileExW(tmp_wide.c_str(), final_wide.c_str(), flags)) {
        return true;
    }
    const DWORD error = GetLastError();
    spdlog::warn(
        "[VACache] atomic replace failed: {} -> {}, error={}",
        tmp_path,
        final_path,
        error);
    win_utf8::delete_file_utf8(tmp_path);
    return false;
#else
    std::error_code ec;
    std::filesystem::rename(win_utf8::path_from_utf8(tmp_path),
                            win_utf8::path_from_utf8(final_path),
                            ec);
    if (!ec) return true;
    std::filesystem::remove(win_utf8::path_from_utf8(tmp_path));
    return false;
#endif
}

std::string frame_range_name(uint32_t start, uint32_t end) {
    if (start == UINT32_MAX && end == UINT32_MAX) {
        return "all";
    }
    std::ostringstream ss;
    ss << std::setw(8) << std::setfill('0') << start
       << "_"
       << std::setw(8) << std::setfill('0') << end;
    return ss.str();
}

bool chunk_matches_key(const VachunkFile& chunk, const VachunkKey& key) {
    const auto& h = chunk.header();
    return h.kind == static_cast<uint16_t>(key.kind) &&
           h.codec == static_cast<uint16_t>(key.codec) &&
           h.feature_flags == key.feature_flags &&
           h.base_content_revision == key.base_content_revision &&
           h.generator_revision == key.generator_revision &&
           h.start_frame == key.start_frame &&
           h.end_frame == key.end_frame &&
           h.start_packet == key.start_packet &&
           h.end_packet == key.end_packet &&
           h.start_unit == key.start_unit &&
           h.end_unit == key.end_unit;
}

} // namespace

const char* vachunk_kind_name(VachunkKind kind) {
    switch (kind) {
    case VachunkKind::NaluDetail: return "nalu_detail";
    case VachunkKind::FrameSummaryExact: return "frame_summary_exact";
    case VachunkKind::Overlay: return "overlay";
    case VachunkKind::HitTest: return "hit_test";
    case VachunkKind::Export: return "export";
    default: return "unknown";
    }
}

VacacheStore::VacacheStore(std::string cache_root, std::string hash)
    : cache_root_(std::move(cache_root)), hash_(std::move(hash)) {}

std::string VacacheStore::hash_dir() const {
    return path_to_utf8_string(path_from_utf8_join(cache_root_, hash_));
}

std::string VacacheStore::base_path() const {
    return path_to_utf8_string(win_utf8::path_from_utf8(hash_dir()) / L"base.vac");
}

std::string VacacheStore::tmp_dir() const {
    return path_to_utf8_string(win_utf8::path_from_utf8(hash_dir()) / L"tmp");
}

std::string VacacheStore::chunks_root() const {
    return path_to_utf8_string(win_utf8::path_from_utf8(hash_dir()) / L"chunks");
}

std::string VacacheStore::chunks_dir(VachunkKind kind) const {
    return path_to_utf8_string(
        win_utf8::path_from_utf8(chunks_root()) /
        win_utf8::path_from_utf8(vachunk_kind_name(kind)));
}

std::string VacacheStore::chunk_path(const VachunkKey& key) const {
    std::ostringstream name;
    name << static_cast<int>(key.codec)
         << "_f" << hex_u64(key.feature_flags)
         << "_b" << hex_u64(key.base_content_revision)
         << "_g" << hex_u64(key.generator_revision)
         << "_" << frame_range_name(key.start_frame, key.end_frame)
         << ".vck";
    return path_to_utf8_string(
        win_utf8::path_from_utf8(chunks_dir(key.kind)) /
        win_utf8::path_from_utf8(name.str()));
}

bool VacacheStore::ensure_layout() const {
    return create_dir_utf8(hash_dir()) &&
           create_dir_utf8(tmp_dir()) &&
           create_dir_utf8(chunks_root());
}

bool VacacheStore::write_base_atomic(const Vac2BaseData& data,
                                     uint64_t max_output_bytes) const {
    if (!ensure_layout()) return false;
    const std::string tmp_path =
        unique_tmp_path_for(tmp_dir(), base_path(), "base_vac");
    if (!write_vac2_base_container(tmp_path, data, max_output_bytes)) {
        win_utf8::delete_file_utf8(tmp_path);
        return false;
    }
    return replace_file_utf8(tmp_path, base_path());
}

bool VacacheStore::open_base(Vac2BaseFile& out) const {
    return out.open(base_path());
}

bool VacacheStore::write_chunk_atomic(const VachunkKey& key,
                                      VachunkData data,
                                      uint64_t max_output_bytes) const {
    if (!ensure_layout()) {
        spdlog::error("[VACache] failed to create cache layout: {}", hash_dir());
        return false;
    }
    const std::string chunk_dir = chunks_dir(key.kind);
    if (!create_dir_utf8(chunk_dir)) {
        spdlog::error("[VACache] failed to create chunk directory: {}", chunk_dir);
        return false;
    }

    data.kind = key.kind;
    data.codec = key.codec;
    data.feature_flags = key.feature_flags;
    data.base_content_revision = key.base_content_revision;
    data.generator_revision = key.generator_revision;
    data.start_frame = key.start_frame;
    data.end_frame = key.end_frame;
    data.start_packet = key.start_packet;
    data.end_packet = key.end_packet;
    data.start_unit = key.start_unit;
    data.end_unit = key.end_unit;

    const std::string final_path = chunk_path(key);
    const std::string tmp_path =
        unique_tmp_path_for(tmp_dir(), final_path, "chunk_vck");
    if (!write_vachunk_file(tmp_path, data, max_output_bytes)) {
        spdlog::error("[VACache] failed to write VACHUNK temp file: {}", tmp_path);
        win_utf8::delete_file_utf8(tmp_path);
        return false;
    }
    if (!replace_file_utf8(tmp_path, final_path)) {
        spdlog::error("[VACache] failed to publish VACHUNK: {} -> {}",
                      tmp_path, final_path);
        return false;
    }
    return true;
}

bool VacacheStore::open_chunk(const VachunkKey& key, VachunkFile& out) const {
    if (!out.open(chunk_path(key))) return false;
    if (!chunk_matches_key(out, key)) {
        out.close();
        return false;
    }
    return true;
}

} // namespace vr::analysis
