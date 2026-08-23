#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstdint>

// True Bounded MPMC Queue (Dmitry Vyukov Algorithm)
template <typename T, size_t Size>
class MPMCRingBuffer {
    // Size must be a power of two for fast bitwise masking
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of two");

    struct Cell {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};
    Cell buffer_[Size];

public:
    MPMCRingBuffer() {
        for (size_t i = 0; i < Size; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool push(const T& item) {
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & (Size - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.data = item;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false; // Buffer is full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    bool pop(T& item) {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & (Size - 1)];
            size_t seq = cell.sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    item = cell.data;
                    cell.sequence.store(pos + Size, std::memory_order_release);
                    return true;
                }
            } else if (dif < 0) {
                return false; // Buffer is empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
    }
};

int main() {
    constexpr size_t BUFFER_SIZE = 131072; // Must be power of 2
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 2500000;
    constexpr long long TOTAL_ITEMS = static_cast<long long>(PRODUCERS) * ITEMS_PER_PRODUCER;

    std::cout << "[⚡] True MPMC Benchmark Started (4P / 4C)...\n";
    std::cout << "Target Total Items: " << TOTAL_ITEMS << "\n";

    MPMCRingBuffer<int, BUFFER_SIZE> queue;

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(PRODUCERS);
    consumers.reserve(CONSUMERS);

    std::atomic<long long> consumed_count{0};

    // Spawn Producer Threads
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&queue]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                while (!queue.push(i)) {
                    // Spin-lock backpressure
                }
            }
        });
    }

    // Spawn Consumer Threads
    for (int c = 0; c < CONSUMERS; ++c) {
        consumers.emplace_back([&queue, &consumed_count]() {
            int item;
            for (;;) {
                long long current = consumed_count.load(std::memory_order_relaxed);
                if (current >= TOTAL_ITEMS) break;

                if (queue.pop(item)) {
                    consumed_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Wait for all threads to finish
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    double tps = static_cast<double>(TOTAL_ITEMS) / elapsed.count();

    std::cout << "[✔] 10,000,000 Items Processed (True MPMC)!\n";
    std::cout << "⏱️ Elapsed Time: " << elapsed.count() << " seconds\n";
    std::cout << "🚀 TPS: " << static_cast<long long>(tps) << " ops/sec\n";

    return 0;
}
