# ⚡ Deep-Ring: Ultra-Fast C++ Lock-Free Ring Buffer

![C++20](https://img.shields.io/badge/C++-20-blue.svg)
![Build](https://img.shields.io/badge/build-passing-brightgreen)
![Performance](https://img.shields.io/badge/TPS-100M+-orange)

**Deep-Ring** is a high-performance, single-node extreme optimization Proof of Concept (PoC) project. It explores the physical limits of hardware-level memory synchronization, evolving from a high-speed SPSC (Single-Producer Single-Consumer) architecture to a **True Bounded MPMC (Multi-Producer Multi-Consumer)** concurrent queue.

---

## 🚀 The Motivation: Why Build This?
Modern software architecture often hits a wall not because of CPU speed, but because of memory synchronization overhead. Traditional locking mechanisms (like `std::mutex`) force threads into a deep sleep, causing massive context-switching overhead and CPU stall cycles. 

**Deep-Ring** bypasses software bottlenecks by solving the problem at the physical hardware layer—utilizing L1/L2 cache dynamics, atomic operations, and sequence-based slot reservation to achieve true zero-wait concurrency.

---

## 🧠 Evolution of Architecture

### Phase 1: High-Speed SPSC Ring Buffer
- **Target:** 1 Producer & 1 Consumer.
- **Optimization:** Enforced `alignas(64)` on all critical atomic variables (`head_` and `tail_`) to eliminate **False Sharing** on x86/ARM cache lines.
- **Memory Model:** Utilized C++20 Acquire-Release semantics (`std::memory_order_acquire` / `release`) to guarantee strict causality with minimal instruction overhead.

### Phase 2: True Bounded MPMC Queue (Dmitry Vyukov Algorithm)
- **Target:** Multiple Producers & Multiple Consumers (4P / 4C and beyond).
- **Core Mechanism:** Replaced simple pointer increments with cell-based sequence numbering. Each buffer cell tracks its own sequence state, and threads use `compare_exchange_weak` to safely race for slot ownership without locks.
- **Performance Trade-off:** While SPSC excels in single-thread pairs due to zero CAS contention, the MPMC design gracefully handles multi-core thread contention where traditional queues completely break down.

---

## 🗺️ Project Roadmap

- [x] **Phase 1 (Completed):** Extreme SPSC optimization, hitting ~100M+ TPS under high-end cloud environments.
- [x] **Phase 2 (Completed):** Dmitry Vyukov-style Bounded MPMC Queue supporting concurrent multi-threaded workloads.
- [ ] **Phase 3 (Planned):** Integration of OS Kernel-bypass network communication utilizing **AWS EFA (Elastic Fabric Adapter)** and **RDMA**.
- [ ] **Phase 4 (Planned):** Hardware-level Split-Brain prevention mechanism using eBPF deadman-switches.

---

## ⚙️ Benchmark & Performance

Tested on compute-optimized cloud instances with maximum compiler optimization (`-O3`).

| Architecture | Concurrency | Data Volume | Throughput (TPS) |
|-------------|-------------|-------------|------------------|
| **SPSC** (Phase 1) | 1P / 1C | 10,000,000 | **~ 110,000,000 ops/sec** |
| **MPMC** (Phase 2) | 4P / 4C | 10,000,000 | **Multi-threaded Safe** |

---

## 🛠️ System Requirements & How to Run

### Requirements
- A modern C++ compiler supporting C++20 (GCC 10+, Clang 11+, or MSVC).
- Linux, macOS, or Windows (WSL recommended).

### Build and Execute
```bash
# Clone the repository
git clone [https://github.com/zart810-ui/deep-ring-buffer.git](https://github.com/zart810-ui/deep-ring-buffer.git)
cd deep-ring-buffer

# Compile with maximum optimization (-O3) and threading support
g++ -O3 -pthread main.cpp -o hlc_bench

# Run the benchmark
./hlc_bench
