#include "analysis/cache/overlay_text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace vr::analysis {
namespace {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = 6;
constexpr int kAtlasGlyphCell = 8;
constexpr int kAtlasColumns = 16;
constexpr int kAtlasRows = 6;
constexpr int kFirstAtlasChar = 32;
constexpr int kLastAtlasChar = 126;

using GlyphRows = std::array<uint8_t, kGlyphHeight>;

constexpr GlyphRows glyph_blank() {
    return {0, 0, 0, 0, 0, 0, 0};
}

GlyphRows glyph_for_ascii(char ch) {
    switch (ch) {
    case ' ': return glyph_blank();
    case '!': return {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
    case '"': return {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00};
    case '#': return {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A};
    case '$': return {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04};
    case '%': return {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03};
    case '&': return {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D};
    case '\'': return {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00};
    case '(': return {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02};
    case ')': return {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08};
    case '*': return {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00};
    case '+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    case ',': return {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08};
    case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case '/': return {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    case ':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    case ';': return {0x00, 0x0C, 0x0C, 0x00, 0x04, 0x04, 0x08};
    case '<': return {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02};
    case '=': return {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00};
    case '>': return {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08};
    case '?': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
    case '@': return {0x0E, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0E};
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case 'J': return {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    default: return glyph_for_ascii('?');
    }
}

int text_cell_width(std::string_view text) {
    if (text.empty()) return 0;
    return static_cast<int>(text.size() - 1) * kGlyphAdvance + kGlyphWidth;
}

char normalize_ascii_glyph_char(char ch) {
    if (ch < kFirstAtlasChar || ch > kLastAtlasChar) return '?';
    return ch;
}

void atlas_rect_for_char(char ch, int& x0, int& y0, int& x1, int& y1) {
    const int index = static_cast<int>(normalize_ascii_glyph_char(ch)) - kFirstAtlasChar;
    const int col = index % kAtlasColumns;
    const int row = index / kAtlasColumns;
    x0 = col * kAtlasGlyphCell;
    y0 = row * kAtlasGlyphCell;
    x1 = x0 + kAtlasGlyphCell;
    y1 = y0 + kAtlasGlyphCell;
}

void append_glyph_quads(std::vector<OverlayGlyphQuad>& out,
                        std::string_view text,
                        float origin_x,
                        float origin_y,
                        float cell,
                        int surface_width,
                        int surface_height,
                        OverlayColor color) {
    float cursor_x = origin_x;
    for (const char raw_ch : text) {
        const char ch = normalize_ascii_glyph_char(raw_ch);
        if (ch != ' ') {
            int atlas_x0 = 0;
            int atlas_y0 = 0;
            int atlas_x1 = 0;
            int atlas_y1 = 0;
            atlas_rect_for_char(ch, atlas_x0, atlas_y0, atlas_x1, atlas_y1);
            OverlayGlyphQuad quad;
            quad.x0 = std::clamp(cursor_x, 0.0f, static_cast<float>(surface_width));
            quad.y0 = std::clamp(origin_y, 0.0f, static_cast<float>(surface_height));
            quad.x1 = std::clamp(
                cursor_x + static_cast<float>(kAtlasGlyphCell) * cell,
                0.0f,
                static_cast<float>(surface_width));
            quad.y1 = std::clamp(
                origin_y + static_cast<float>(kAtlasGlyphCell) * cell,
                0.0f,
                static_cast<float>(surface_height));
            quad.atlas_x0 = atlas_x0;
            quad.atlas_y0 = atlas_y0;
            quad.atlas_x1 = atlas_x1;
            quad.atlas_y1 = atlas_y1;
            quad.color = color;
            if (quad.x1 > quad.x0 && quad.y1 > quad.y0) out.push_back(quad);
        }
        cursor_x += static_cast<float>(kGlyphAdvance) * cell;
    }
}

} // namespace

std::string overlay_cu_label_text(const VachunkCuRecord& cu,
                                  OverlayCuLabelMode mode) {
    char buffer[32] = {};
    const auto& c = cu.common;
    switch (mode) {
    case OverlayCuLabelMode::Qp:
        std::snprintf(buffer, sizeof(buffer), "QP %u", static_cast<unsigned>(c.qp));
        break;
    case OverlayCuLabelMode::BitCost:
        if (c.bit_count >= 1000000u) {
            std::snprintf(buffer, sizeof(buffer), "%uM",
                          static_cast<unsigned>((c.bit_count + 500000u) / 1000000u));
        } else if (c.bit_count >= 10000u) {
            std::snprintf(buffer, sizeof(buffer), "%uK",
                          static_cast<unsigned>((c.bit_count + 500u) / 1000u));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%uB",
                          static_cast<unsigned>(c.bit_count));
        }
        break;
    case OverlayCuLabelMode::Prediction:
        if (c.pred_mode == 1) {
            std::snprintf(buffer, sizeof(buffer), "INTRA");
        } else if (cu.inter.skip != 0) {
            std::snprintf(buffer, sizeof(buffer), "SKIP");
        } else if (cu.inter.merge_flag != 0) {
            std::snprintf(buffer, sizeof(buffer), "MERGE");
        } else {
            std::snprintf(buffer, sizeof(buffer), "INTER");
        }
        break;
    }
    return buffer;
}

bool append_ascii_overlay_glyph_quads(std::vector<OverlayGlyphQuad>& out,
                                      std::string_view text,
                                      const OverlayTextLayout& layout,
                                      OverlayColor foreground,
                                      OverlayColor shadow) {
    if (text.empty() ||
        layout.surface_width <= 0 ||
        layout.surface_height <= 0 ||
        layout.rect_x1 <= layout.rect_x0 ||
        layout.rect_y1 <= layout.rect_y0 ||
        layout.pixel_scale_x <= 0.0f ||
        layout.pixel_scale_y <= 0.0f ||
        !std::isfinite(layout.pixel_scale_x) ||
        !std::isfinite(layout.pixel_scale_y)) {
        return false;
    }

    const float min_scale = std::min(layout.pixel_scale_x, layout.pixel_scale_y);
    const float cell =
        static_cast<float>(std::max(1, layout.target_cell_pixels)) / min_scale;
    const float text_width =
        static_cast<float>(text_cell_width(text) + (kAtlasGlyphCell - kGlyphWidth)) * cell;
    const float text_height = static_cast<float>(kAtlasGlyphCell) * cell;
    const float padding_x = std::max(
        cell,
        static_cast<float>(std::max(0, layout.padding_pixels)) /
            layout.pixel_scale_x);
    const float padding_y = std::max(
        cell,
        static_cast<float>(std::max(0, layout.padding_pixels)) /
            layout.pixel_scale_y);
    const float rect_width = layout.rect_x1 - layout.rect_x0;
    const float rect_height = layout.rect_y1 - layout.rect_y0;
    if (rect_width < text_width + padding_x * 2 ||
        rect_height < text_height + padding_y * 2) {
        return false;
    }

    const float origin_x = layout.rect_x0 + (rect_width - text_width) * 0.5f;
    const float origin_y = layout.rect_y0 + (rect_height - text_height) * 0.5f;
    const float shadow_offset = std::max(cell * 0.5f, 1.0f / min_scale);
    const size_t before = out.size();
    if (shadow.a > 0) {
        append_glyph_quads(
            out,
            text,
            origin_x + shadow_offset,
            origin_y + shadow_offset,
            cell,
            layout.surface_width,
            layout.surface_height,
            shadow);
    }
    append_glyph_quads(
        out,
        text,
        origin_x,
        origin_y,
        cell,
        layout.surface_width,
        layout.surface_height,
        foreground);
    return out.size() > before;
}

bool build_ascii_overlay_glyph_atlas(std::vector<uint8_t>& alpha,
                                     int& width,
                                     int& height) {
    width = kAtlasColumns * kAtlasGlyphCell;
    height = kAtlasRows * kAtlasGlyphCell;
    if (width <= 0 || height <= 0) return false;
    alpha.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    for (int ch = kFirstAtlasChar; ch <= kLastAtlasChar; ++ch) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        atlas_rect_for_char(static_cast<char>(ch), x0, y0, x1, y1);
        (void)x1;
        (void)y1;
        const GlyphRows glyph = glyph_for_ascii(static_cast<char>(ch));
        for (int row = 0; row < kGlyphHeight; ++row) {
            const uint8_t bits = glyph[static_cast<size_t>(row)];
            for (int col = 0; col < kGlyphWidth; ++col) {
                const uint8_t mask =
                    static_cast<uint8_t>(1u << (kGlyphWidth - 1 - col));
                if ((bits & mask) == 0) continue;
                const int px = x0 + col;
                const int py = y0 + row;
                alpha[static_cast<size_t>(py) * static_cast<size_t>(width) +
                      static_cast<size_t>(px)] = 255;
            }
        }
    }
    return true;
}

} // namespace vr::analysis
