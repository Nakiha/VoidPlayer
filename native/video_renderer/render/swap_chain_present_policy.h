#pragma once

namespace vr {

inline bool should_present_swap_chain_after_draw(bool drew, bool has_device) {
    return drew && has_device;
}

} // namespace vr
