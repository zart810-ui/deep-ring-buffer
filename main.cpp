#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

// Ultra-fast lock-free ring buffer to prevent CPU cache contention
template <typename T, size_t Size>
class LockFreeRingBuffer {
private:
    // alignas(64): Aligns memory to the CPU cache line (64 bytes) to prevent False Sharing
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Size];

public:
    // Push data into the buffer (Producer)
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) % Size;
        
        // Check if buffer is full (Acquire memory barrier)
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false; 
        }
        
        buffer_[current_tail] = item;
        // Release barrier: Update tail after data is fully written
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    // Pop data from the buffer (Consumer)
    bool pop(T& item) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        
        // Check if buffer is empty (Acquire memory barrier)
        if (current_head == tail_.load(std::memory_order_acquire)) {
            return false; 
        }
        
        item = buffer_[current_head];
        // Release barrier: Update head after data is fully read
        head_.store((current_head + 1) % Size, std::memory_order_release);
        return true;
    }
};

int main() {
    std::cout << "[⚡] Deep-Ring Benchmark Started...\n";
    
    LockFreeRingBuffer<int, 100000> ring_buffer;
    const int TOTAL_ITEMS = 10000000; // 10 million items
    
    auto start_time = std::chrono::high_resolution_clock::now();

    // Core 1: Producer Thread
    std::thread producer([&]() {
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            while (!ring_buffer.push(i)) {
                // Backpressure: Yield if buffer is full
                std::this_thread::yield(); 
            }
        }
    });

    // Core 2: Consumer Thread
    std::thread consumer([&]() {
        int item;
        for (int i = 0; i < TOTAL_ITEMS; ++i) {
            while (!ring_buffer.pop(item)) {
                // Yield if buffer is empty
                std::this_thread::yield(); 
            }
        }
    });

    producer.join();
    consumer.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    double tps = TOTAL_ITEMS / elapsed.count();

    std::cout << "[✔] 10,000,000 Items Processed!\n";
    std::cout << "⏱️ Elapsed Time: " << elapsed.count() << " seconds\n";
    std::cout << "🚀 TPS: " << (long long)tps << " ops/sec\n";

    return 0;
}
