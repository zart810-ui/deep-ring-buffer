#include <iostream>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <memory>
#include <limits>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

// ============================================================
// Configuration
// ============================================================

constexpr size_t CACHE_LINE = 64;
constexpr size_t BUFFER_SIZE = 131072;
constexpr int ITERATIONS = 5;
constexpr uint64_t TOTAL_ITEMS = 10000000ULL;

// ============================================================
// CPU Pause (Backoff)
// ============================================================

inline void cpu_pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// ============================================================
// Thread Affinity
// ============================================================

bool pin_thread_to_core(std::thread& t,
                        unsigned int core_id,
                        unsigned int max_cpus)
{
    if (max_cpus == 0 || !t.joinable())
        return false;

    const unsigned int target_core = core_id % max_cpus;

#ifdef __linux__

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(target_core, &cpuset);

    const int rc = pthread_setaffinity_np(
        t.native_handle(),
        sizeof(cpu_set_t),
        &cpuset
    );

    if (rc != 0) {
        std::cerr
            << "[Warning] Linux affinity failed for CPU "
            << target_core
            << " (error "
            << rc
            << ")\n";
        return false;
    }

    return true;

#elif defined(_WIN32) || defined(_WIN64)

    GROUP_AFFINITY affinity{};
    WORD group = 0;
    WORD subgroupCore = static_cast<WORD>(target_core);

#if defined(NTDDI_WIN10) && (NTDDI_VERSION >= NTDDI_WIN10)
    USHORT groupCount = GetActiveProcessorGroupCount();
    if (groupCount > 1) {
        unsigned int accumulated = 0;
        for (WORD g = 0; g < groupCount; ++g) {
            unsigned int countInGroup = GetActiveProcessorCount(g);
            if (target_core < accumulated + countInGroup) {
                group = g;
                subgroupCore = static_cast<WORD>(target_core - accumulated);
                break;
            }
            accumulated += countInGroup;
        }
    } else {
        group = static_cast<WORD>(target_core / 64);
        subgroupCore = static_cast<WORD>(target_core % 64);
    }
#else
    group = static_cast<WORD>(target_core / 64);
    subgroupCore = static_cast<WORD>(target_core % 64);
#endif

    subgroupCore %= 64;

    affinity.Group = group;
    affinity.Mask = static_cast<KAFFINITY>(1ULL << subgroupCore);

    if (SetThreadGroupAffinity(
            t.native_handle(),
            &affinity,
            nullptr))
    {
        return true;
    }

    if (target_core < sizeof(DWORD_PTR) * 8) {
        DWORD_PTR mask =
            (DWORD_PTR(1) << target_core);

        if (SetThreadAffinityMask(
                t.native_handle(),
                mask) != 0)
        {
            return true;
        }
    }

    std::cerr
        << "[Warning] Windows affinity failed for CPU "
        << target_core
        << "\n";

    return false;

#else

    (void)t;
    (void)core_id;
    (void)max_cpus;

    return false;

#endif
}

// ============================================================
// SPSC Ring Buffer
// ============================================================

template <typename T, size_t Size>
class SPSCRingBuffer
{
    static_assert(
        (Size & (Size - 1)) == 0,
        "Size must be a power of two"
    );

    alignas(CACHE_LINE)
    std::atomic<size_t> head_{0};

    alignas(CACHE_LINE)
    std::atomic<size_t> tail_{0};

    alignas(CACHE_LINE)
    T buffer_[Size];

public:

    bool push(const T& item)
    {
        const size_t tail =
            tail_.load(std::memory_order_relaxed);

        const size_t next =
            (tail + 1) & (Size - 1);

        if (next ==
            head_.load(std::memory_order_acquire))
        {
            return false;
        }

        buffer_[tail] = item;

        tail_.store(
            next,
            std::memory_order_release
        );

        return true;
    }

    bool pop(T& item)
    {
        const size_t head =
            head_.load(std::memory_order_relaxed);

        if (head ==
            tail_.load(std::memory_order_acquire))
        {
            return false;
        }

        item = buffer_[head];

        head_.store(
            (head + 1) & (Size - 1),
            std::memory_order_release
        );

        return true;
    }
};

// ============================================================
// Bounded MPMC Ring Buffer (Dmitry Vyukov Algorithm)
// ============================================================

template <typename T, size_t Size>
class MPMCRingBuffer
{
    static_assert(
        (Size & (Size - 1)) == 0,
        "Size must be a power of two"
    );

    struct Cell
    {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(CACHE_LINE)
    std::atomic<size_t> enqueue_pos_{0};

    alignas(CACHE_LINE)
    std::atomic<size_t> dequeue_pos_{0};

    alignas(CACHE_LINE)
    Cell buffer_[Size];

public:

    MPMCRingBuffer()
    {
        for (size_t i = 0; i < Size; ++i) {
            buffer_[i].sequence.store(
                i,
                std::memory_order_relaxed
            );
        }
    }

    bool push(const T& item)
    {
        size_t pos =
            enqueue_pos_.load(
                std::memory_order_relaxed
            );

        for (;;) {

            Cell& cell =
                buffer_[pos & (Size - 1)];

            const size_t seq =
                cell.sequence.load(
                    std::memory_order_acquire
                );

            const intptr_t dif =
                static_cast<intptr_t>(seq) -
                static_cast<intptr_t>(pos);

            if (dif == 0) {

                if (enqueue_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed))
                {
                    cell.data = item;

                    cell.sequence.store(
                        pos + 1,
                        std::memory_order_release
                    );

                    return true;
                }

            }
            else if (dif < 0) {

                return false;

            }
            else {

                pos =
                    enqueue_pos_.load(
                        std::memory_order_relaxed
                    );
            }
        }
    }

    bool pop(T& item)
    {
        size_t pos =
            dequeue_pos_.load(
                std::memory_order_relaxed
            );

        for (;;) {

            Cell& cell =
                buffer_[pos & (Size - 1)];

            const size_t seq =
                cell.sequence.load(
                    std::memory_order_acquire
                );

            const intptr_t dif =
                static_cast<intptr_t>(seq) -
                static_cast<intptr_t>(pos + 1);

            if (dif == 0) {

                if (dequeue_pos_.compare_exchange_weak(
                        pos,
                        pos + 1,
                        std::memory_order_relaxed))
                {
                    item = cell.data;

                    cell.sequence.store(
                        pos + Size,
                        std::memory_order_release
                    );

                    return true;
                }

            }
            else if (dif < 0) {

                return false;

            }
            else {

                pos =
                    dequeue_pos_.load(
                        std::memory_order_relaxed
                    );
            }
        }
    }
};

// ============================================================
// Start Barrier
// ============================================================

class StartBarrier
{
    std::atomic<bool> ready_{false};

public:

    void wait()
    {
        while (!ready_.load(
            std::memory_order_acquire))
        {
            cpu_pause();
        }
    }

    void start()
    {
        ready_.store(
            true,
            std::memory_order_release
        );
    }
};

// ============================================================
// Statistics
// ============================================================

void print_stats(
    const std::string& name,
    std::vector<double> results)
{
    if (results.empty())
        return;

    std::sort(
        results.begin(),
        results.end()
    );

    const double min_val = results.front();
    const double max_val = results.back();
    const double sum = std::accumulate(results.begin(), results.end(), 0.0);
    const double avg = sum / results.size();

    const double median =
        results.size() % 2 == 0
        ? (results[results.size() / 2 - 1] + results[results.size() / 2]) / 2.0
        : results[results.size() / 2];

    std::cout
        << "\n[" << name << "]\n"
        << "  Min:    " << static_cast<uint64_t>(min_val) << " ops/sec\n"
        << "  Avg:    " << static_cast<uint64_t>(avg) << " ops/sec\n"
        << "  Median: " << static_cast<uint64_t>(median) << " ops/sec\n"
        << "  Max:    " << static_cast<uint64_t>(max_val) << " ops/sec\n";
}

// ============================================================
// SPSC Benchmark (Optimized with cpu_pause)
// ============================================================

void run_spsc_benchmark(
    uint64_t total_items,
    unsigned int hw)
{
    std::cout
        << "\n=== SPSC 1P/1C ===\n"
        << "Logical CPUs: " << hw << "\n";

    // Warm-up
    {
        auto queue = std::unique_ptr<SPSCRingBuffer<int, BUFFER_SIZE>>(new SPSCRingBuffer<int, BUFFER_SIZE>());
        StartBarrier barrier;

        std::thread producer([&]() {
            barrier.wait();
            for (uint64_t i = 0; i < total_items; ++i) {
                while (!queue->push(1)) { cpu_pause(); }
            }
        });

        std::thread consumer([&]() {
            barrier.wait();
            int item;
            uint64_t count = 0;
            while (count < total_items) {
                if (queue->pop(item)) {
                    ++count;
                } else {
                    cpu_pause();
                }
            }
        });

        barrier.start();
        producer.join();
        consumer.join();
    }

    std::vector<double> results;
    results.reserve(ITERATIONS);

    for (int iteration = 0; iteration < ITERATIONS; ++iteration)
    {
        auto queue = std::unique_ptr<SPSCRingBuffer<int, BUFFER_SIZE>>(new SPSCRingBuffer<int, BUFFER_SIZE>());
        StartBarrier barrier;
        std::atomic<uint64_t> consumed{0};

        std::thread producer([&]() {
            barrier.wait();
            for (uint64_t i = 0; i < total_items; ++i) {
                while (!queue->push(1)) { cpu_pause(); }
            }
        });

        std::thread consumer([&]() {
            barrier.wait();
            int item;
            uint64_t count = 0;
            while (count < total_items) {
                if (queue->pop(item)) {
                    ++count;
                } else {
                    cpu_pause();
                }
            }
            consumed.store(count, std::memory_order_release);
        });

        pin_thread_to_core(producer, 0, hw);
        pin_thread_to_core(consumer, 1, hw);

        const auto start = std::chrono::steady_clock::now();
        barrier.start();

        producer.join();
        consumer.join();

        const auto end = std::chrono::steady_clock::now();

        if (consumed.load(std::memory_order_acquire) != total_items) {
            throw std::runtime_error("SPSC data loss detected");
        }

        const double elapsed = std::chrono::duration<double>(end - start).count();
        results.push_back(static_cast<double>(total_items) / elapsed);
    }

    print_stats("SPSC 1P/1C", results);
}

// ============================================================
// MPMC Benchmark
// ============================================================

void run_mpmc_benchmark(
    int producers,
    int consumers,
    uint64_t total_items,
    unsigned int hw)
{
    if (producers <= 0 || consumers <= 0) {
        throw std::invalid_argument("Producer/consumer count must be > 0");
    }

    if (total_items % producers != 0) {
        throw std::invalid_argument("TOTAL_ITEMS must be divisible by producers");
    }

    const uint64_t items_per_producer = total_items / producers;
    const std::string label = "MPMC " + std::to_string(producers) + "P/" + std::to_string(consumers) + "C";

    std::cout
        << "\n=== " << label << " ===\n"
        << "Logical CPUs: " << hw << "\n";

    // Warm-up
    {
        auto queue = std::unique_ptr<MPMCRingBuffer<int, BUFFER_SIZE>>(new MPMCRingBuffer<int, BUFFER_SIZE>());
        StartBarrier barrier;
        std::atomic<int> active_producers{producers};
        std::vector<std::thread> producer_threads;
        std::vector<std::thread> consumer_threads;

        for (int p = 0; p < producers; ++p) {
            producer_threads.emplace_back([&]() {
                barrier.wait();
                for (uint64_t i = 0; i < items_per_producer; ++i) {
                    while (!queue->push(1)) { cpu_pause(); }
                }
                active_producers.fetch_sub(1, std::memory_order_release);
            });
        }

        for (int c = 0; c < consumers; ++c) {
            consumer_threads.emplace_back([&]() {
                barrier.wait();
                int item;
                for (;;) {
                    if (queue->pop(item)) continue;
                    if (active_producers.load(std::memory_order_acquire) == 0) {
                        if (!queue->pop(item)) break;
                    } else {
                        cpu_pause();
                    }
                }
            });
        }

        barrier.start();
        for (auto& t : producer_threads) t.join();
        for (auto& t : consumer_threads) t.join();
    }

    std::vector<double> results;
    results.reserve(ITERATIONS);

    for (int iteration = 0; iteration < ITERATIONS; ++iteration)
    {
        auto queue = std::unique_ptr<MPMCRingBuffer<int, BUFFER_SIZE>>(new MPMCRingBuffer<int, BUFFER_SIZE>());
        StartBarrier barrier;
        std::atomic<int> active_producers{producers};

        struct alignas(CACHE_LINE) Counter {
            uint64_t value = 0;
        };
        std::vector<Counter> consumer_counts(consumers);
        std::vector<std::thread> producer_threads;
        std::vector<std::thread> consumer_threads;

        for (int p = 0; p < producers; ++p) {
            producer_threads.emplace_back([&]() {
                barrier.wait();
                for (uint64_t i = 0; i < items_per_producer; ++i) {
                    while (!queue->push(1)) { cpu_pause(); }
                }
                active_producers.fetch_sub(1, std::memory_order_release);
            });
            pin_thread_to_core(producer_threads.back(), static_cast<unsigned int>(p), hw);
        }

        for (int c = 0; c < consumers; ++c) {
            consumer_threads.emplace_back([&, c]() {
                barrier.wait();
                int item;
                uint64_t local_count = 0;
                for (;;) {
                    if (queue->pop(item)) {
                        ++local_count;
                        continue;
                    }
                    if (active_producers.load(std::memory_order_acquire) == 0) {
                        if (!queue->pop(item)) break;
                        ++local_count;
                    } else {
                        cpu_pause();
                    }
                }
                consumer_counts[c].value = local_count;
            });
            pin_thread_to_core(consumer_threads.back(), static_cast<unsigned int>(producers + c), hw);
        }

        const auto start = std::chrono::steady_clock::now();
        barrier.start();

        for (auto& t : producer_threads) t.join();
        for (auto& t : consumer_threads) t.join();

        const auto end = std::chrono::steady_clock::now();

        uint64_t total_consumed = 0;
        for (const auto& counter : consumer_counts) {
            total_consumed += counter.value;
        }

        if (total_consumed != total_items) {
            throw std::runtime_error(label + ": data loss detected");
        }

        const double elapsed = std::chrono::duration<double>(end - start).count();
        results.push_back(static_cast<double>(total_items) / elapsed);
    }

    print_stats(label, results);
}

// ============================================================
// Strict Integrity Test
// ============================================================

void run_integrity_check(
    int producers,
    int consumers,
    uint64_t total_items)
{
    if (total_items % producers != 0) {
        throw std::invalid_argument("TOTAL_ITEMS must be divisible by producers");
    }

    std::cout << "\n=== Strict Integrity Test " << producers << "P/" << consumers << "C ===\n";

    auto queue = std::unique_ptr<MPMCRingBuffer<int, BUFFER_SIZE>>(new MPMCRingBuffer<int, BUFFER_SIZE>());
    const uint64_t items_per_producer = total_items / producers;
    std::atomic<int> active_producers{producers};
    StartBarrier barrier;

    std::vector<std::thread> producer_threads;
    std::vector<std::thread> consumer_threads;
    std::vector<std::vector<int>> consumer_bags(consumers);
    std::vector<uint64_t> consumer_counts(consumers, 0);

    for (auto& bag : consumer_bags) {
        bag.reserve(static_cast<size_t>(total_items / consumers));
    }

    for (int p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&, p]() {
            barrier.wait();
            for (uint64_t i = 0; i < items_per_producer; ++i) {
                const int value = static_cast<int>(p * items_per_producer + i + 1);
                while (!queue->push(value)) { cpu_pause(); }
            }
            active_producers.fetch_sub(1, std::memory_order_release);
        });
    }

    for (int c = 0; c < consumers; ++c) {
        consumer_threads.emplace_back([&, c]() {
            barrier.wait();
            int item;
            uint64_t count = 0;
            auto& bag = consumer_bags[c];
            for (;;) {
                if (queue->pop(item)) {
                    bag.push_back(item);
                    ++count;
                    continue;
                }
                if (active_producers.load(std::memory_order_acquire) == 0) {
                    if (!queue->pop(item)) break;
                    bag.push_back(item);
                    ++count;
                } else {
                    cpu_pause();
                }
            }
            consumer_counts[c] = count;
        });
    }

    barrier.start();
    for (auto& t : producer_threads) t.join();
    for (auto& t : consumer_threads) t.join();

    uint64_t total_consumed = 0;
    for (uint64_t count : consumer_counts) {
        total_consumed += count;
    }

    if (total_consumed != total_items) {
        std::cout << "[FAILED] Count mismatch: " << total_consumed << " / " << total_items << "\n";
        return;
    }

    std::vector<bool> seen(static_cast<size_t>(total_items + 1), false);
    for (const auto& bag : consumer_bags) {
        for (int value : bag) {
            if (value < 1 || static_cast<uint64_t>(value) > total_items) {
                std::cout << "[FAILED] Invalid value: " << value << "\n";
                return;
            }
            if (seen[value]) {
                std::cout << "[FAILED] Duplicate value: " << value << "\n";
                return;
            }
            seen[value] = true;
        }
    }

    for (uint64_t i = 1; i <= total_items; ++i) {
        if (!seen[i]) {
            std::cout << "[FAILED] Missing value: " << i << "\n";
            return;
        }
    }

    std::cout << "[PASSED] " << total_items << " items verified without loss or duplication.\n";
}

// ============================================================
// Main
// ============================================================

int main()
{
    std::cout
        << "==================================================\n"
        << "Ultimate Concurrency Benchmark\n"
        << "==================================================\n";

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;

    std::cout
        << "\nLogical CPUs detected: " << hw << "\n"
        << "Total items per benchmark: " << TOTAL_ITEMS << "\n"
        << "Iterations: " << ITERATIONS << "\n"
        << "Buffer size: " << BUFFER_SIZE << "\n";

    try
    {
        std::cout << "\n=== Part 1 ===\n";
        run_spsc_benchmark(TOTAL_ITEMS, hw);
        run_mpmc_benchmark(1, 1, TOTAL_ITEMS, hw);

        std::cout << "\n=== Part 2 ===\n";
        run_mpmc_benchmark(2, 2, TOTAL_ITEMS, hw);
        run_mpmc_benchmark(3, 3, TOTAL_ITEMS, hw);

        std::cout << "\n=== Part 3 ===\n";
        run_integrity_check(3, 3, TOTAL_ITEMS);
    }
    catch (const std::exception& e)
    {
        std::cerr << "\nERROR: " << e.what() << "\n";
        return 1;
    }

    std::cout
        << "\n==================================================\n"
        << "All benchmark tests completed.\n"
        << "==================================================\n";

    return 0;
}
