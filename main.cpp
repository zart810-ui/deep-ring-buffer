#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

// Ultra-fast MPMC-ready style Lock-Free Ring Buffer (Busy-waiting Version)
template <typename T, size_t Size>
class LockFreeRingBuffer {
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Size];

public:
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % Size;
        
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; 
        }
        
        buffer_[current_tail] = item;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; 
        }
        
        item = buffer_[current_head];
        head_.store((current_head + 1) % Size, std::memory_order_release);
        return true;
    }
};

int main() {
    std::cout << "[⚡] Deep-Ring Extreme Spin-Lock Benchmark Started...\n";
    
    LockFreeRingBuffer<int, 100000> ring_buffer;
    const int TOTAL_ITEMS = 10000000; 
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Producer: No yield, pure busy-waiting for max throughput
    std::thread producer([&]() {
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            while (!ring_buffer.push(i)) {
                // Busy-waiting: spin continuously without yielding
            }
        }
    });

    // Consumer: No yield, pure busy-waiting
    std::thread consumer([&]() {
        int item;
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            while (!ring_buffer.pop(item)) {
                // Busy-waiting
            }
        }
    });

    producer.join();
    consumer.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    double tps = TOTAL_ITEMS / elapsed.count();

    std::cout << "[✔] 10,000,000 Items Processed (Spin-Lock)!\n";
    std::cout << "⏱️ Elapsed Time: " << elapsed.count() << " seconds\n";
    std::cout << "🚀 TPS: " << (long long)tps << " ops/sec\n";

    return 0;
}
