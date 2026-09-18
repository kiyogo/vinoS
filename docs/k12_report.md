# Báo cáo Tổng kết Hoàn thành Milestone K12: Ổn định Lõi Kernel KOS (x86_64)

**Dự án:** KOS (Kiến trúc x86_64, Freestanding C17 + NASM Assembly)  
**Mục tiêu:** Milestone K12 — Chuẩn hóa nền tảng lõi kernel, đồng bộ đa luồng, ngắt ngoại lệ, lập lịch ưu tiên, quản lý tiến trình, đối tượng kernel và handle table.  
**Ngày hoàn thành:** 18/09/2026  
**Trạng thái kiểm thử:** 100% PASS trên môi trường UEFI QEMU q35.

---

## I. TỔNG QUAN KIẾN TRÚC TRƯỚC VÀ SAU MILESTONE K12

| Thành phần | Trước K12 (Trạng thái sơ khai) | Sau K12 (Trạng thái chuẩn hóa K12) |
|---|---|---|
| **Logging Subsystem** | Ghi trực tiếp ra serial COM1, không có ring buffer, có thể gây race condition. | Ring buffer 64 KiB trong RAM, non-allocating `ksnprintf`/`kvsnprintf`, timestamp (ms/sec), TID tag, polling emergency bypass cho Exception/Panic. |
| **Interrupt Frame & IDT** | Frame ngắt không đồng nhất giữa ASM stubs và C handler, thiếu trường RSP/SS. | Cấu trúc chuẩn hóa 176-byte `struct interrupt_context` bảo lưu toàn bộ 15 thanh ghi đa năng, RIP, CS, RFLAGS, RSP, SS; `cld` tại `isr_common`; IST1 dedicated stack (16 KiB) cho Double Fault (#DF). |
| **Page Fault Handler** | Handler sơ khai, không phân tích bitflags chi tiết. | Bóc tách CR2 tức thì, giải mã 8 cờ Intel SDM (P, W/R, U/S, RSVD, I/D, PK, SS, SGX), cung cấp chuỗi mô tả human-readable, `page_fault_decode()` diagnostic. |
| **Kernel Panic & Stack Trace** | Không có stack trace, gọi hàm cấp phát trong panic dễ deadlock. | Zero-allocation lockless panic, recursion latch bảo vệ re-entry, RBP frame pointer unwinding (`-fno-omit-frame-pointer`), bảng biểu tượng 2-pass (`symbol_lookup` $O(\log N)$). |
| **Atomic & Synchronization** | Chưa có atomic wrappers, spinlock sơ khai không lưu cờ RFLAGS. | `atomic_t`, `atomic64_t` đầy đủ barrier (`smp_mb`, `cpu_relax`), `kos_spinlock_t` với `irqsave`/`irqrestore` (bảo toàn `RFLAGS.IF`), `kos_mutex_t` hỗ trợ sleeping wait queue. |
| **Timer & Sleeping** | PIT ngắt nhưng không có hàng đợi ngủ dựa trên delta ticks. | Bộ đếm 64-bit đơn điệu (`timer_get_ticks()`), chuyển đổi `ms_to_ticks`/`ticks_to_ms`, `task_sleep()` với hàng đợi `sleep_queue` sắp xếp tăng dần theo delta ticks. |
| **Scheduler** | Cooperative Round-Robin đơn giản (phải tự gọi `task_yield()`). | Preemptive Scheduler với lát cắt thời gian Quantum (20 ms), ngắt PIT kích hoạt `task_timer_tick()` và `task_reschedule_if_needed()`, Idle thread thực thi vòng lặp `hlt`, bộ đếm lồng `preempt_disable()/enable()`. |
| **Process & Thread Model** | Chỉ có khái niệm phẳng `struct task`. | Tách biệt `struct process` và `struct thread`, 6 trạng thái vòng đời (RUNNABLE, RUNNING, BLOCKED, SLEEPING, ZOMBIE, DEAD), tiến trình Reaper chuyên trách dọn dẹp ngăn xếp thread ở ngữ cảnh độc lập. |
| **Kernel Objects & Handles** | Chưa có mô hình quản trị tài nguyên trừu tượng. | `kernel_object` với bộ đếm tham chiếu atomic và destructor callback riêng theo kiểu đối tượng; Bảng Handle per-process có generation counter 16-bit (chống lỗi ABA) và mặt nạ quyền `rights`. |
| **Wait Queues & Events** | Chưa có cơ chế đợi sự kiện tổng quát. | `wait_queue_t` với `wait_event`, `wait_event_timeout`, `wait_queue_wake_one`, `wait_queue_wake_all`, đối tượng Event kernel object, đồng bộ hóa thoát tiến trình `process_wait`. |
| **Subsystem Thread Safety** | PMM, Heap, VMM, VFS, Network có nguy cơ tương tranh dữ liệu. | Toàn bộ PMM allocator, Kernel Heap, VMM page tables, VFS/RAMFS và UDP/TCP Socket đều được đồng bộ hóa với spinlock `irqsave` và non-busy wait queues. |

---

## II. CHI TIẾT KỸ THUẬT CỐT LÕI (CORE IMPLEMENTATIONS)

### 1. Cấu trúc Interrupt Frame & Chuyển ngữ cảnh
- **Interrupt Context Layout (176 bytes):**
  - Offset `0x00 - 0x70`: R15, R14, R13, R12, R11, R10, R9, R8, RBP, RDI, RSI, RDX, RCX, RBX, RAX (được `push` bởi `isr_common`).
  - Offset `0x78`: `vector` (số hiệu ngắt 0..255).
  - Offset `0x80`: `error_code` (do CPU đẩy hoặc stub đẩy giả lập 0).
  - Offset `0x88 - 0xA8`: RIP, CS, RFLAGS, RSP, SS (phần cứng CPU tự động đẩy vào khi chuyển đổi mức đặc quyền hoặc ngắt).
- **Task Switch Context (`struct task_context`):**
  - Bảo lưu: RBX, RSP (offset 48), RBP, R12, R13, R14, R15, RIP (offset 56), RFLAGS (offset 64).
  - Thực thi trong `arch/x86_64/task_switch.asm` đảm bảo atomic lưu/nạp thanh ghi và cờ CPU.

### 2. Nguyên tắc Thứ tự Khóa (Lock Ordering Hierarchy)
Để ngăn chặn hoàn toàn hiện tượng Deadlock lặp vòng, kernel KOS thiết lập quy tắc thứ tự chiếm dụng khóa nghiêm ngặt:
$$\text{Reaper Lock} \rightarrow \text{Process Lock} \rightarrow \text{Thread / Task Lock} \rightarrow \text{Wait Queue / Sleep Lock} \rightarrow \text{PMM / Heap Spinlock}$$
1. Luôn sử dụng `spinlock_lock_irqsave()` khi khóa có khả năng bị truy cập từ ngữ cảnh ngắt (IRQ handler).
2. Không bao giờ thực hiện cấp phát bộ nhớ động (`kmalloc`) hay ngủ (`task_sleep`/`mutex_lock`) khi đang nắm giữ `spinlock`.

### 3. Bảng Trạng thái Vòng đời Luồng & Tiến trình (State Machine)
```
  [CREATED]
     │
     ▼
[RUNNABLE] <─── (Quantum Expired / Preempted / Yield) ─── [RUNNING]
     │                                                        │
     │ (Waiting Event / Sleep)                                │ (Exit / Fault)
     ▼                                                        ▼
 [BLOCKED] / [SLEEPING] ─────────────────────────────────> [ZOMBIE]
                                                              │
                                                              ▼ (Reaper / Parent Wait)
                                                           [DEAD] -> [UNUSED]
```

### 4. Định dạng Handle Table & Cơ chế Chống Lỗi ABA
Handle 32-bit được mã hóa như sau:
$$\text{Handle} = (\text{Generation} \ll 16) \mid (\text{Index} \ \& \ \text{0xFFFF})$$
- Khi một slot trong handle table được giải phóng và tái sử dụng, `generation` tăng thêm 1.
- Mọi truy vấn sử dụng handle cũ bị lệch generation sẽ lập tức bị từ chối với lỗi `KOS_HANDLE_STALE`, ngăn chặn 100% nguy cơ nhầm lẫn đối tượng.

---

## III. KẾT QUẢ TEST SUITE & CHẨN ĐOÁN TRÊN QEMU

Toàn bộ các bộ kiểm thử chẩn đoán và stress test đã được tích hợp trực tiếp vào shell KOS và kiểm chứng thành công trong quá trình khởi động:

1. **`handletest` (K12.14, K12.15)**:
   - Refcounting (1 -> 2 -> 1 -> 0) & Custom Destructor callback: **PASS**
   - Handle creation, rights validation, object type validation: **PASS**
   - Anti-ABA stale handle rejection: **PASS**
   - Handle duplication & process exit automatic table destruction: **PASS**
2. **`waittest` (K12.13, K12.16)**:
   - Single waiter wakeup (`wait_queue_wake_one`): **PASS**
   - Broadcast wakeup (`wait_queue_wake_all` cho 3 luồng đồng thời): **PASS**
   - Timed wait (`wait_event_timeout` 50ms): **PASS**
   - Kernel Object Event handle signal/reset: **PASS**
   - `process_wait` exit synchronization & exit code 77 delivery: **PASS**
3. **`synctest` (K12.17 - K12.21)**:
   - PMM Concurrent Allocation (4 worker threads, 25 vòng lặp, 0 leak): **PASS**
   - Heap Concurrent Allocation & Realloc (4 worker threads, 20 vòng lặp): **PASS**
   - VFS / RAMFS Concurrent Create/Read/Write/Unlink: **PASS**
   - UDP Socket Wait Queue Non-Busy Synchronization & Timeout (30ms): **PASS**
4. **`atomictest` (K12.5)**:
   - 32-bit & 64-bit atomic loads, stores, adds, subs, fetch_add, xchg, cmpxchg: **PASS**
   - Memory barriers (`smp_mb`, `smp_rmb`, `smp_wmb`, `cpu_relax`): **PASS**
5. **`locktest` (K12.6)**:
   - Spinlock unit tests & IRQ-save RFLAGS preservation: **PASS**
   - 4 threads $\times$ 500 iterations $\rightarrow$ counter = 2000 với 0 race conditions: **PASS**
6. **`mutextest` (K12.7)**:
   - Sleeping mutex exclusion: 4 threads $\times$ 100 iters $\rightarrow$ 0 vi phạm độc quyền: **PASS**
7. **`timertest` & `sleeptest` (K12.8)**:
   - Monotonic 64-bit ticks, uptime, tick/ms conversions: **PASS**
   - Delta sleep queue: 3 luồng ngủ 30ms, 60ms, 90ms thức dậy chính xác theo thứ tự sắp xếp: **PASS**
8. **`preempttest` (K12.9)**:
   - 2 luồng tính toán thuần CPU (không gọi `task_yield`) được ngắt timer PIT chia sẻ lát cắt quantum hoàn thành song song: **PASS**
9. **`processtest` (K12.10 - K12.12)**:
   - Tạo tiến trình con, thực thi luồng, nhận exit code 88 qua `process_wait`: **PASS**
10. **`stacktest` (K12.4)**:
    - RBP frame pointer unwinding kết hợp tìm kiếm nhị phân $O(\log N)$ trong bảng 703 biểu tượng nhúng: **PASS**
11. **`pftest` (K12.2)**:
    - Giải mã chính xác 4 ca ngoại lệ Page Fault nhân tạo: **PASS**
12. **`panictest` (K12.3)**:
    - Kiểm tra các đặc tính lockless zero-alloc và cơ chế chống đệ quy của Panic: **PASS**
13. **`kstress` / `taskstress` (K12.22 - K12.26)**:
    - Chạy đồng thời 4 luồng áp lực cao (PMM allocator churn, Heap churn, RAMFS I/O, Sched):
    - Khởi điểm: Free Frames = 50,980, Active Heap Allocs = 37.
    - Kết thúc: Free Frames = 50,980 ($\Delta = 0$), Active Heap Allocs = 37 ($\Delta = 0$).
    - **Kết luận:** Hoàn toàn không rò rỉ bộ nhớ frame hay heap allocs!

---

## IV. BẢO TOÀN VÀ TƯƠNG THÍCH HỆ THỐNG
- Giữ nguyên 100% tính năng ngăn xếp mạng (DHCP, ARP, ICMP Ping, TCP client/server, DNS lookup, HTTP curl, HTTP server mini).
- Giữ nguyên VFS đa volume (`C:` initramfs, `D:` RAMFS).
- Giữ nguyên toàn bộ 40+ lệnh trong KOS Debug Terminal.
- Biên dịch sạch với Clang C17 và LLD: **`-Wall -Wextra -Werror` (0 cảnh báo, 0 lỗi)**.
