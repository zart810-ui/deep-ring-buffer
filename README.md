⚡ Ultimate Enterprise Concurrency Benchmark Suite
Comprehensive Technical Documentation
1. Project Overview

The Ultimate Enterprise Concurrency Benchmark Suite is a high-performance C++ microbenchmark and concurrency validation framework designed to evaluate lock-free bounded ring buffers under realistic multi-threaded workloads.

The framework focuses on two synchronization models:

SPSC — Single-Producer / Single-Consumer
MPMC — Multi-Producer / Multi-Consumer

The benchmark is designed not only to measure raw throughput, but also to verify data integrity under heavy concurrent contention.

To improve benchmark consistency and reduce measurement noise, the framework incorporates:

CPU spin-wait instructions (_mm_pause)
Explicit thread affinity
Windows Processor Group support
Cache-line alignment
Acquire/release memory ordering
Warm-up runs
Start synchronization
Multiple benchmark iterations
Full payload integrity verification
2. Architecture

The benchmark consists of three major components:

                 Ultimate Concurrency Benchmark
                              │
             ┌────────────────┼────────────────┐
             │                │                │
             ▼                ▼                ▼
        SPSC Queue       MPMC Queue       Integrity Audit
             │                │                │
          1P / 1C        1P / 1C           3P / 3C
                           2P / 2C
                           3P / 3C
Execution Pipeline
Part 1
 ├── SPSC 1P/1C
 └── MPMC 1P/1C

Part 2
 ├── MPMC 2P/2C
 └── MPMC 3P/3C

Part 3
 └── Strict Integrity Test
      └── 3 Producers / 3 Consumers

Each performance benchmark processes:

10,000,000 items × 5 iterations

3. SPSC Ring Buffer

The SPSC implementation is specialized for exactly one producer and one consumer.

Because only one thread modifies each index, the queue does not require CAS operations for normal enqueue/dequeue operations.

Design
Producer
   │
   ▼
tail ───────► [ Ring Buffer ]
                 │
                 ▼
              head
                 │
                 ▼
             Consumer

The implementation uses:

std::atomic<size_t>
memory_order_relaxed
memory_order_acquire
memory_order_release
Power-of-two buffer sizing
Cache-line alignment

The index calculation uses:

index & (Size - 1)

instead of the more expensive modulo operation.

With:

BUFFER_SIZE = 131072

the buffer size is a power of two, allowing efficient index wrapping.

4. MPMC Ring Buffer

The MPMC implementation is based on the bounded sequence-number ring-buffer algorithm commonly associated with Dmitry Vyukov.

Unlike the SPSC queue, multiple producers and consumers can concurrently access the queue.

Each cell contains:

struct Cell
{
    std::atomic<size_t> sequence;
    T data;
};

The sequence number determines whether a cell is available for enqueue or dequeue.

Enqueue

A producer checks whether:

sequence == enqueue_position

If the condition is satisfied, the producer attempts to claim the position using:

compare_exchange_weak()

After writing the payload, the producer publishes the cell using:

memory_order_release
Dequeue

A consumer checks whether:

sequence == dequeue_position + 1

After retrieving the payload, the consumer advances the cell sequence to make the slot available for future producers.

5. Memory Ordering

The benchmark deliberately avoids unnecessarily strong sequential consistency.

The primary synchronization model is:

relaxed
   ↓
acquire / release
   ↓
payload visibility

For example, a producer writes the payload first:

buffer_[tail] = item;

and then publishes the new position:

tail_.store(
    next,
    std::memory_order_release
);

The consumer obtains the corresponding position using:

tail_.load(
    std::memory_order_acquire
);

This establishes the required visibility relationship between the payload and queue metadata.

6. Cache-Line Optimization

The queue uses:

alignas(CACHE_LINE)

with:

constexpr size_t CACHE_LINE = 64;

The objective is to reduce false sharing between frequently modified atomic variables.

For example:

Cache Line A
┌──────────────────────────────┐
│ enqueue_pos_                 │
└──────────────────────────────┘

Cache Line B
┌──────────────────────────────┐
│ dequeue_pos_                 │
└──────────────────────────────┘

Separating these frequently modified variables can reduce unnecessary cache-coherence traffic.

7. CPU Spin-Wait Optimization

The benchmark uses a dedicated CPU pause function:

inline void cpu_pause() noexcept

On supported x86/x64 MSVC builds it uses:

_mm_pause();

and GCC/Clang-compatible x86 builds use:

__builtin_ia32_pause();

This is used while waiting for queue availability:

while (!queue->push(1))
{
    cpu_pause();
}

and:

while (!queue->pop(item))
{
    cpu_pause();
}

This is preferable to immediately performing an operating-system context switch during short spin-wait periods.

8. Thread Affinity

The benchmark supports explicit CPU affinity.

Linux

Uses:

pthread_setaffinity_np()

with:

cpu_set_t
Windows

Uses:

SetThreadGroupAffinity()

with Processor Group discovery through:

GetActiveProcessorGroupCount()
GetActiveProcessorCount()

This allows the benchmark to explicitly assign worker threads to logical CPUs.

For example:

CPU 0 → Producer 0
CPU 1 → Producer 1
CPU 2 → Consumer 0
CPU 3 → Consumer 1

The benchmark also contains a fallback to SetThreadAffinityMask() where applicable.

9. Start Synchronization

The benchmark uses a lightweight custom start barrier.

Workers first enter:

barrier.wait();

and remain in a spin-wait state until:

barrier.start();

is called.

This allows the benchmark timer to begin immediately before the workers are released.

Conceptually:

Producer ─────┐
              │
Consumer ─────┼──► WAIT
              │
Consumer ─────┘

                 │
                 ▼
          barrier.start()

                 │
                 ▼

        ┌─────────────────┐
        │ Benchmark START │
        └─────────────────┘
10. Benchmark Statistics

Each performance configuration executes:

ITERATIONS = 5
TOTAL_ITEMS = 10,000,000

Throughput is calculated as:

throughput = total_items / elapsed_time

The benchmark reports:

Minimum
Average
Median
Maximum

Example:

[SPSC 1P/1C]
  Min:    X ops/sec
  Avg:    X ops/sec
  Median: X ops/sec
  Max:    X ops/sec

Using multiple iterations makes it possible to observe run-to-run variability instead of relying on a single measurement.

11. Warm-Up Phase

Before collecting performance measurements, each benchmark configuration performs a complete warm-up run.

The warm-up is intentionally excluded from the reported statistics.

This helps reduce the influence of initial execution effects such as:

Thread startup
Initial cache state
Memory allocation
CPU frequency transitions
Runtime initialization
12. Strict Integrity Verification

Performance alone is not sufficient for a concurrency benchmark.

The framework therefore includes a dedicated integrity test.

Configuration:

3 Producers
3 Consumers
10,000,000 Items

Each producer generates a globally unique integer:

p * items_per_producer + i + 1

This produces the expected range:

1 ... 10,000,000

The consumer side stores every received value.

The final audit checks:

1. Count
Consumed == Produced
2. Range

Every value must satisfy:

1 <= value <= TOTAL_ITEMS
3. Duplicate Detection

Every value is checked against:

std::vector<bool> seen
4. Missing Value Detection

Every expected value must appear exactly once.

Therefore, a successful test establishes:

No loss
No duplication
No invalid values
No missing values

with respect to the test's verification criteria.

13. Benchmark Configuration

Current configuration:

CACHE_LINE  = 64
BUFFER_SIZE = 131072
ITERATIONS  = 5
TOTAL_ITEMS = 10000000

The benchmark evaluates:

Test	Producers	Consumers	Items
SPSC	1	1	10,000,000
MPMC	1	1	10,000,000
MPMC	2	2	10,000,000
MPMC	3	3	10,000,000
Integrity	3	3	10,000,000
14. Build Requirements

The project requires a modern C++ compiler with C++11-or-newer threading and atomic support.

Linux — GCC
g++ -O3 -pthread main.cpp -o concurrency_benchmark
Linux — Clang
clang++ -O3 -pthread main.cpp -o concurrency_benchmark
Windows — MSVC

From the Developer Command Prompt for Visual Studio:

cl /O2 /EHsc main.cpp /Fe:concurrency_benchmark.exe
15. Execution

Linux:

./concurrency_benchmark

Windows:

concurrency_benchmark.exe

The program automatically detects the number of logical CPUs using:

std::thread::hardware_concurrency()

and reports the detected CPU count before beginning the benchmark.

16. Design Goals

The benchmark is designed around four primary goals:

⚡ Performance

Measure high-throughput lock-free queue operations under different levels of contention.

🧵 Scalability

Compare single-threaded ownership against multiple producers and consumers.

🛡️ Correctness

Verify that concurrency optimizations do not result in lost, duplicated, corrupted, or missing data.

🔬 Reproducibility

Reduce benchmark noise through warm-up, CPU affinity, synchronization, cache-aware layout, and repeated measurements.

17. Final Summary

The Ultimate Enterprise Concurrency Benchmark Suite combines low-level C++ atomic programming, cache-aware data structures, CPU affinity, spin-wait optimization, statistical benchmarking, and exhaustive integrity verification into a single test framework.

Its architecture separates the two major concerns of concurrent queue evaluation:

              ┌───────────────────────┐
              │   Performance         │
              │   Benchmarking        │
              └───────────┬───────────┘
                          │
                          ▼
                 Throughput Analysis
                          │
                          │
              ┌───────────▼───────────┐
              │   Correctness         │
              │   Verification        │
              └───────────┬───────────┘
                          │
                          ▼
                 Full Data Audit

The result is a high-throughput, contention-focused concurrency benchmark that measures both how fast the queue operates and whether it maintains correctness under heavy concurrent workloads.
