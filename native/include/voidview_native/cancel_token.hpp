#pragma once

#include <atomic>

namespace voidview {

/**
 * CancelToken - 取消令牌
 *
 * 用于异步操作的取消控制。线程安全的原子布尔标志。
 */
class CancelToken {
public:
    CancelToken() = default;
    ~CancelToken() = default;

    // 禁止拷贝
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    // 允许移动
    CancelToken(CancelToken&&) noexcept = default;
    CancelToken& operator=(CancelToken&&) noexcept = default;

    /**
     * 请求取消
     */
    void cancel() {
        cancelled_.store(true, std::memory_order_release);
    }

    /**
     * 检查是否已取消
     */
    bool is_cancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    /**
     * 重置取消状态 (用于复用)
     */
    void reset() {
        cancelled_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> cancelled_{false};
};

} // namespace voidview
