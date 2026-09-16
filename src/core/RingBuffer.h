// Lock-free single-producer / single-consumer ring buffer used for UI<->audio thread messaging.
#pragma once
#include <atomic>
#include <vector>
#include <cstddef>
#include <cstring>

namespace mt {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(size_t capacity = 1024) : m_buf(capacity + 1), m_cap(capacity + 1) {}

    bool push(const T& v) {
        size_t w = m_write.load(std::memory_order_relaxed);
        size_t next = (w + 1) % m_cap;
        if (next == m_read.load(std::memory_order_acquire)) return false; // full
        m_buf[w] = v;
        m_write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        size_t r = m_read.load(std::memory_order_relaxed);
        if (r == m_write.load(std::memory_order_acquire)) return false; // empty
        out = m_buf[r];
        m_read.store((r + 1) % m_cap, std::memory_order_release);
        return true;
    }
    bool empty() const { return m_read.load(std::memory_order_acquire) == m_write.load(std::memory_order_acquire); }
    size_t size() const { size_t w = m_write.load(std::memory_order_acquire), r = m_read.load(std::memory_order_acquire); return (w + m_cap - r) % m_cap; }
private:
    std::vector<T> m_buf;
    size_t m_cap;
    alignas(64) std::atomic<size_t> m_read{0};
    alignas(64) std::atomic<size_t> m_write{0};
};

// SPSC ring of raw floats (used by the recording engine to hand audio to the disk-writer thread).
class FloatRing {
public:
    explicit FloatRing(size_t capacity = 1 << 20) : m_buf(capacity), m_cap(capacity) {}
    size_t writeAvailable() const { size_t w = m_write.load(std::memory_order_acquire), r = m_read.load(std::memory_order_acquire); return m_cap - 1 - ((w + m_cap - r) % m_cap); }
    size_t readAvailable() const { size_t w = m_write.load(std::memory_order_acquire), r = m_read.load(std::memory_order_acquire); return (w + m_cap - r) % m_cap; }
    size_t write(const float* data, size_t n) {
        n = std::min(n, writeAvailable());
        size_t w = m_write.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) { m_buf[w] = data[i]; w = (w + 1) % m_cap; }
        m_write.store(w, std::memory_order_release);
        return n;
    }
    size_t read(float* out, size_t n) {
        n = std::min(n, readAvailable());
        size_t r = m_read.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) { out[i] = m_buf[r]; r = (r + 1) % m_cap; }
        m_read.store(r, std::memory_order_release);
        return n;
    }
    void clear() { m_read.store(m_write.load()); }
private:
    std::vector<float> m_buf;
    size_t m_cap;
    alignas(64) std::atomic<size_t> m_read{0};
    alignas(64) std::atomic<size_t> m_write{0};
};

} // namespace mt
