#pragma once
#include <atomic>
#include <vector>
#include <cstdint>

enum class SyntheticEventType { MOUSE, KEYBOARD };

struct SyntheticEvent {
    SyntheticEventType type = SyntheticEventType::MOUSE;
    int buttons = 0;
    float dx = 0.0f;
    float dy = 0.0f;
    float scroll = 0.0f;
    int keyCode = 0;
    bool isDown = false;
};

class SPSCRingBuffer {
private:
    std::vector<SyntheticEvent> buffer;
    const size_t capacity;

    // Aligned to 64 bytes to prevent cache-line False Sharing
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};

public:
    explicit SPSCRingBuffer(size_t cap) : capacity(cap), buffer(cap) {}

    bool push(const SyntheticEvent& event) {
        size_t currentTail = tail.load(std::memory_order_relaxed);
        size_t nextTail = (currentTail + 1) % capacity;

        if (nextTail == head.load(std::memory_order_acquire)) {
            return false; // Buffer full (dropped packet)
        }

        buffer[currentTail] = event;
        tail.store(nextTail, std::memory_order_release);
        return true;
    }

    bool pop(SyntheticEvent& event) {
        size_t currentHead = head.load(std::memory_order_relaxed);

        if (currentHead == tail.load(std::memory_order_acquire)) {
            return false; // Buffer empty
        }

        event = buffer[currentHead];
        head.store((currentHead + 1) % capacity, std::memory_order_release);
        return true;
    }
};
