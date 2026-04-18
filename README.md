# Multi-Container Runtime — OS Jackfruit

**Team:** Bhavesh Vellurus & Annamalai N  
**SRNs:** PES1UG24CS115 & PES1UG24CS068  
**Course:** Operating Systems, PES University  

---

## 1. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 VM (no WSL). Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) git
```

### Build

```bash
cd boilerplate
make
```

This builds: `engine`, `memory_hog`, `cpu_hog`, `io_pulse` (user-space), and `monitor.ko` (kernel module).

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Prepare Root Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/aarch64/alpine-minirootfs-3.20.3-aarch64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-aarch64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workload binaries into rootfs before launch
cp ./memory_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/
```

### Start Supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
```

### CLI Commands (Terminal 2)

```bash
# Start containers
sudo ./engine start alpha ./rootfs-alpha "while true; do echo hello; sleep 2; done"
sudo ./engine start beta ./rootfs-beta "while true; do echo beta-running; sleep 3; done"

# List containers
sudo ./engine ps

# View logs
sudo ./engine logs alpha
sudo ./engine logs beta

# Stop a container
sudo ./engine stop alpha

# Run and wait for exit
sudo ./engine run alpha ./rootfs-alpha "/cpu_hog"

# Memory limit test
sudo ./engine start alpha ./rootfs-alpha "/memory_hog" --soft-mib 5 --hard-mib 10

# Scheduling experiment
sudo ./engine start alpha ./rootfs-alpha "/cpu_hog" --nice -5
sudo ./engine start beta ./rootfs-beta "/cpu_hog" --nice 10
```

### Unload and Clean Up

```bash
# Stop supervisor with Ctrl+C, then:
sudo rmmod monitor
make clean
```

### Check Kernel Events

```bash
sudo dmesg | grep container_monitor | tail -20
```

---

## 2. Demo Screenshots

### Screenshot 1 & 2 — Multi-container supervision and metadata tracking

<img width="1472" height="178" alt="image" src="https://github.com/user-attachments/assets/5ae245a5-f136-4947-8514-73c338d652d4" />

<img width="1970" height="316" alt="image" src="https://github.com/user-attachments/assets/48c87a23-620f-4754-bb4d-ca9e9e188db3" />

Two containers (alpha, beta) started under one supervisor. `ps` output shows PID, state, soft/hard memory limits, exit code, and stop reason for each container.

### Screenshot 3 & 4 — Bounded-buffer logging and CLI/IPC

<img width="1568" height="669" alt="image" src="https://github.com/user-attachments/assets/932a114e-b764-4687-8d8e-80b9ed52afac" />

<img width="1568" height="696" alt="image" src="https://github.com/user-attachments/assets/9222e34c-6fef-4e88-be33-afef799006ff" />

`logs alpha` shows captured stdout from the alpha container (hello lines). `logs beta` shows beta-running lines — each container's output is routed through the bounded-buffer pipeline and written to separate log files. The CLI communicates with the supervisor over a UNIX domain socket.

### Screenshot 5 & 6 — Soft-limit warning and hard-limit enforcement

<img width="1946" height="348" alt="image" src="https://github.com/user-attachments/assets/502420e0-b08a-4a0d-8c8e-89781fc54292" />

<img width="1082" height="68" alt="image" src="https://github.com/user-attachments/assets/d11d4c66-1591-4caa-af5d-d5d88f212130" />

`dmesg` shows SOFT LIMIT warning at ~8.7MB (limit 5MB) and HARD LIMIT kill at ~17MB (limit 10MB) for the memory_hog container. `ps` shows state: `killed`, reason: `hard_limit_killed`, exit code 137 (128 + SIGKILL).

### Screenshot 7 — Scheduling experiment

<img width="1525" height="784" alt="image" src="https://github.com/user-attachments/assets/e94e5603-98c6-4708-b9cc-f2515fe4cef2" />

Two cpu_hog containers run simultaneously for 10 seconds:
- alpha (nice -5, high priority): final accumulator = `91191869728987414396`
- beta (nice 10, low priority): final accumulator = `14596888583826408098`

Alpha completed approximately 6x more computation than beta in the same wall-clock time, demonstrating the Linux CFS scheduler's priority-weighted CPU allocation.

### Screenshot 8 — Clean teardown

<img width="1774" height="492" alt="image" src="https://github.com/user-attachments/assets/d2e6a9ca-2cb2-4b33-add0-80546f3cc7eb" />

<img width="1366" height="170" alt="image" src="https://github.com/user-attachments/assets/6a5879f1-cd08-45f3-be89-7fe727345970" />

Both containers running with visible sleep child processes. After supervisor Ctrl+C: `ps aux | grep sleep` returns empty, confirming all children reaped and no zombie processes remain. Supervisor prints "Clean shutdown complete."

---

## 3. Engineering Analysis

### 3.1 Isolation Mechanisms

The runtime achieves isolation using Linux namespaces via `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` flags. Each container gets its own PID namespace (so container processes see themselves as PID 1), its own UTS namespace (independent hostname via `sethostname()`), and its own mount namespace (independent filesystem view).

`chroot()` into the container's assigned rootfs directory restricts the process's view of the filesystem — it cannot access paths outside its rootfs. `/proc` is mounted inside the container so tools like `ps` work correctly within the container context.

The host kernel is still shared entirely. All containers share the same kernel, scheduler, network stack (unless network namespaces are added), and system call interface. The kernel enforces isolation through namespace boundaries, not hardware separation. This is fundamentally different from VMs, where the hypervisor virtualizes hardware. Containers trade stronger isolation for lower overhead.

### 3.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because container processes are children of the runtime process. If the runtime exited immediately after launching a container, the container would be reparented to PID 1 (init), losing the ability to track metadata, receive SIGCHLD, collect exit status, and route logs.

The supervisor uses `clone()` instead of `fork()` to create containers with namespace flags. When a container exits, the kernel sends SIGCHLD to the supervisor. The SIGCHLD handler calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all finished children without blocking. This prevents zombie processes — entries that remain in the process table after exit until their parent calls wait.

Metadata (PID, state, limits, exit code, stop reason) is maintained in a linked list protected by a mutex. The `stop_requested` flag distinguishes a manual stop (SIGTERM from `engine stop`) from a kernel-enforced hard-limit kill (SIGKILL from the monitor module), ensuring `ps` reports the correct reason.

### 3.3 IPC, Threads, and Synchronization

The project uses two distinct IPC mechanisms:

**Path A — Logging (pipes):** Each container's stdout and stderr are connected to the supervisor via a `pipe()`. A dedicated producer thread per container reads from the pipe read-end and inserts log chunks into the bounded buffer. A single consumer thread removes chunks and writes them to per-container log files in the `logs/` directory.

**Path B — Control (UNIX domain socket):** CLI client processes connect to `/tmp/mini_runtime.sock`, send a `control_request_t` struct, and receive a `control_response_t`. This is a separate channel from the logging pipes, as required.

**Bounded buffer synchronization:** The shared buffer uses a `pthread_mutex_t` for mutual exclusion and two `pthread_cond_t` variables (`not_full`, `not_empty`) for producer-consumer coordination.

Without synchronization, concurrent producers could corrupt the buffer's head/tail/count fields, and consumers could read partially written entries or spin indefinitely on an empty buffer. The mutex ensures only one thread modifies buffer state at a time. Condition variables allow threads to sleep instead of busy-waiting, which is important for a long-running supervisor.

The `shutting_down` flag is set under the mutex before broadcasting on both condition variables. This ensures threads waiting on either condition wake up, observe the flag, and exit cleanly. The consumer drains remaining entries after shutdown begins to prevent log data loss.

**Metadata list synchronization:** The container linked list uses a separate `metadata_lock` mutex, independent of the buffer mutex. This prevents the logging pipeline from blocking CLI commands that need to read or update container state.

### 3.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical RAM currently mapped and present in a process's page tables. It does not measure virtual memory that has been allocated but not yet faulted in (lazy allocation), memory-mapped files that are not resident, or shared library pages counted multiple times across processes.

Soft and hard limits represent different enforcement policies. The soft limit is a warning threshold — when RSS first exceeds it, the kernel module logs a warning via `printk` but does not terminate the process. This allows the supervisor or operator to observe memory pressure without immediately disrupting the container. The hard limit is a termination threshold — when RSS exceeds it, the module sends SIGKILL to the process immediately.

Enforcement belongs in kernel space because user-space monitoring is inherently racy and can be bypassed. A user-space monitor reads `/proc/PID/status` periodically, but between reads a process could allocate and free memory without being caught. More critically, a malicious or buggy process cannot prevent the kernel from sending it a signal — the kernel has authority over all processes regardless of their state. The kernel timer fires every second with guaranteed scheduling regardless of what the monitored process is doing.

### 3.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS) with a virtual runtime concept. Each process accumulates virtual runtime as it runs. The scheduler always picks the process with the lowest virtual runtime next. Nice values adjust the weight assigned to each process — a lower nice value means a higher weight, which means the process accumulates virtual runtime more slowly relative to others, so it gets scheduled more often.

In our experiment, alpha (nice -5) and beta (nice 10) both ran cpu_hog for 10 seconds on a single-core VM. Alpha's final accumulator was ~91 trillion vs beta's ~14 trillion — approximately a 6x ratio. This directly reflects the CFS weight ratio between nice -5 and nice 10. According to Linux's weight table, nice -5 has weight 3121 and nice 10 has weight 110, giving a theoretical ratio of ~28x. The observed 6x ratio is lower because both processes were not always competing (one may have been I/O blocked momentarily) and the VM has multiple vCPUs distributing load.

The key insight is that CFS does not give strict time slices — it gives proportional CPU share based on weight. Higher-priority processes are not guaranteed to run faster in absolute terms, but they receive a larger fraction of available CPU time when competing with lower-priority processes.

---

## 4. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` via `clone()`, with `chroot()` for filesystem isolation.  
**Tradeoff:** `chroot` is simpler than `pivot_root` but less secure — a process with root privileges can escape via `chdir("..")` traversal. `pivot_root` would prevent this.  
**Justification:** For a project demonstrating container primitives, `chroot` is sufficient and significantly easier to implement correctly. Production runtimes like Docker use `pivot_root`.

### Supervisor Architecture
**Choice:** Single long-running supervisor process with a `select()`-based event loop accepting one connection at a time.  
**Tradeoff:** Single-threaded control plane means CLI commands are serialized. Two simultaneous `engine ps` calls would queue rather than run in parallel.  
**Justification:** Simplicity and correctness. A multi-threaded control plane would require careful locking around the metadata list for every concurrent request. For a project with a small number of containers, serialization has no observable downside.

### IPC — UNIX Domain Socket
**Choice:** UNIX domain socket for the CLI-to-supervisor control channel.  
**Tradeoff:** Requires the socket file to exist and be cleaned up. If the supervisor crashes without cleanup, the next start fails unless `unlink()` is called first (we handle this with `unlink(CONTROL_PATH)` at startup).  
**Justification:** UNIX sockets support bidirectional communication, work with `select()`, and are the standard mechanism for local IPC in Linux. FIFOs would require two separate files for bidirectional communication.

### Kernel Monitor — Mutex vs Spinlock
**Choice:** `mutex` to protect the monitored process list.  
**Tradeoff:** Mutexes can sleep, which means they cannot be used in interrupt context. Spinlocks can be used in interrupt context but busy-wait, wasting CPU.  
**Justification:** Our list is accessed from the timer callback (softirq context in some kernels) and the ioctl handler (process context). Since the timer callback uses `mod_timer` which runs in a workqueue-like context on newer kernels, a mutex is safe. The list operations (insert, delete, iterate) may take non-trivial time, making a spinlock wasteful.

### Logging — Per-container Producer Threads
**Choice:** One producer thread per container, one shared consumer thread.  
**Tradeoff:** N producer threads for N containers. For large numbers of containers this could create thread overhead. A single producer multiplexing all pipes with `select()` would scale better.  
**Justification:** Simpler to implement correctly. Each producer owns exactly one pipe and one container ID, eliminating the need for complex multiplexing logic. For the scale of this project (2-4 containers), the overhead is negligible.

---

## 5. Scheduler Experiment Results

### Setup
- Two containers running `cpu_hog` simultaneously on the same VM
- alpha: nice value -5 (higher priority, weight ~3121)
- beta: nice value 10 (lower priority, weight ~110)
- Duration: 10 seconds wall clock

### Results

| Container | Nice | Final Accumulator | Relative Work Done |
|-----------|------|-------------------|-------------------|
| alpha | -5 | 91,191,869,728,987,414,396 | ~6.25x |
| beta | 10 | 14,596,888,583,826,408,098 | 1x (baseline) |

### Analysis

Alpha performed approximately 6x more computation than beta in the same 10-second window. This directly demonstrates CFS priority-weighted scheduling. The scheduler allocated more CPU time to alpha because its lower nice value gives it a higher CFS weight, causing it to accumulate virtual runtime more slowly and therefore be selected for execution more frequently.

The observed ratio (6x) is lower than the theoretical weight ratio (~28x) for two reasons: the VM has more than one vCPU so both processes can sometimes run simultaneously, and the processes occasionally yield the CPU for brief periods during output, reducing pure competition time.

This experiment confirms that the Linux scheduler achieves proportional fairness — processes receive CPU share proportional to their weight — rather than strict priority preemption where a high-priority process would completely starve a low-priority one.
