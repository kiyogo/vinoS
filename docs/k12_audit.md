# Báo cáo Kiểm toán Kiến trúc Toàn diện Kernel KOS (Milestone K12)

Tài liệu này ghi nhận kết quả rà soát toàn bộ repository `d:\DATA\source\OS\KOS\OS2` trước khi triển khai chi tiết các phase của milestone K12.

---

## 1. Kiến trúc Scheduler Hiện tại
* **Cơ chế**: Cooperative Round-Robin (`kernel/task.c`).
* **Số lượng tác vụ**: Tối đa 8 task (`TASK_MAX_COUNT = 8`), lưu trong mảng tĩnh `tasks[8]`.
* **Task bootstrap**: `tasks[0]` là tác vụ khởi động (`"bootstrap"`), chạy chuỗi kiểm thử và shell tương tác (`terminal_run()`).
* **Chuyển ngữ cảnh**: Chỉ diễn ra khi tác vụ tự nguyện gọi `task_yield()` hoặc `task_exit()`.
* **Timer tick**: Hàm `task_timer_tick()` chỉ đặt cờ `reschedule_requested = true`. Cờ này không tự động ngắt luồng đang chạy; nó chỉ được kiểm tra khi tác vụ chủ động gọi `task_reschedule_if_needed()`. Nếu một tác vụ rơi vào vòng lặp vô tận hoặc tính toán CPU-bound mà không yield, toàn bộ kernel bị chiếm dụng hoàn toàn.

---

## 2. Phân tích Lưu trữ Ngữ cảnh (Context Save/Restore)
* **File hiện tại**: `arch/x86_64/task_switch.asm`.
* **Các thanh ghi được lưu**:
  * Chỉ lưu 6 thanh ghi Callee-saved theo System V AMD64 ABI: `rbx`, `rbp`, `r12`, `r13`, `r14`, `r15`.
  * Lưu thanh ghi con trỏ đỉnh stack: `rsp`.
  * Lưu địa chỉ trở về: `rip` (trỏ tới nhãn cục bộ `.resume`).
* **Các thanh ghi CHƯA được lưu**:
  * Các thanh ghi Caller-saved: `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`, `r9`, `r10`, `r11`.
  * Thanh ghi cờ: `rflags`.
  * Các thanh ghi đoạn: `cs`, `ss`, `ds`, `es`.
  * Trạng thái dấu phẩy động và SIMD: `xmm0`–`xmm15`, `mxcsr`.
* **Hệ quả đối với Preemption**: Khi chuyển sang Preemptive Scheduler (ngắt bất kỳ lệnh nào từ timer IRQ), việc chỉ lưu callee-saved registers sẽ làm hỏng dữ liệu của luồng bị ngắt. Cần lưu toàn bộ 15 thanh ghi đa dụng và `rflags` (tương tự như `struct interrupt_context` đã chuẩn hóa ở Bước K12.1).

---

## 3. Hoạt động của Timer IRQ Hiện tại
* **Phần cứng**: PIT 8254 (Programmable Interval Timer) tại I/O port 0x40 và 0x43.
* **Tần số**: Đang khởi tạo ở 100 Hz trong `kernel/main.c` (divisor ~11931).
* **Đường đi của ngắt**:
  1. PIT phát tín hiệu -> PIC Master IRQ 0 (Vector 32 trong IDT).
  2. CPU kích hoạt `isr_stub_32` (`arch/x86_64/interrupt_stubs.asm`), đẩy interrupt context 176 byte lên stack, căn chỉnh 16-byte stack, gọi `interrupt_dispatch()`.
  3. `interrupt_dispatch()` nhận diện vector 32 thuộc dải PIC IRQ (32..47), tăng `irq_counts[0]`, gọi `timer_irq_handler()`.
  4. `timer_irq_handler()` gọi `timer_interrupt()` (tăng biến nguyên tử `ticks`) và gọi `task_timer_tick()`.
  5. `interrupt_dispatch()` gửi EOI tới PIC (`pic_send_eoi(0)`).
* **Điểm cần nâng cấp**: Chuẩn hóa tần số 1000 Hz (1 tick = 1 ms), xây dựng timer abstraction đa tầng (hardware source, monotonic time 64-bit nanoseconds/ticks, software timer queue/deadline) thay vì đọc trực tiếp biến toàn cục.

---

## 4. Danh sách Exception Vector Hiện tại
* **Bảng IDT**: 256 vector đầy đủ, Gate 0..31 dành cho CPU Exceptions.
* **Các vector được định danh trong `interrupts.c`**:
  * Vector 0: Divide-by-zero (`#DE`)
  * Vector 1: Debug exception (`#DB`)
  * Vector 2: NMI interrupt
  * Vector 3: Breakpoint (`#BP`)
  * Vector 4: Overflow (`#OF`)
  * Vector 5: BOUND range exceeded (`#BR`)
  * Vector 6: Invalid opcode (`#UD`)
  * Vector 7: Device not available (`#NM`)
  * Vector 8: Double fault (`#DF`) — Đã cấu hình IST1 với stack riêng biệt 16 KiB
  * Vector 9: Coprocessor segment overrun
  * Vector 10: Invalid TSS (`#TS`)
  * Vector 11: Segment not present (`#NP`)
  * Vector 12: Stack-segment fault (`#SS`)
  * Vector 13: General protection fault (`#GP`)
  * Vector 14: Page fault (`#PF`)
  * Vector 16: x87 FPU error (`#MF`)
  * Vector 17: Alignment check (`#AC`)
  * Vector 18: Machine check (`#MC`)
  * Vector 19: SIMD floating-point (`#XM`)
  * Vector 20: Virtualization exception (`#VE`)
  * Vector 21: Control protection (`#CP`)
  * Vector 29: VMM communication (`#VC`)
  * Vector 30: Security exception (`#SX`)
* **Đăng ký động**: Có hàm `exception_register_handler()` và `exception_unregister_handler()`.

---

## 5. Xử lý Page Fault Hiện tại
* **Hàm hiện có**: `report_page_fault()` trong `arch/x86_64/interrupts.c`.
* Đọc thanh ghi `CR2` qua `cpu_read_cr2()`.
* Giải mã bit error code: P (bit 0), W/R (bit 1), U/S (bit 2), RSVD (bit 3), I/D (bit 4), PK (bit 5), SS (bit 6), SGX (bit 15).
* **Thiếu sót**:
  * Chưa có handler chuyên biệt tách rời.
  * Chưa có guard page cho kernel stack.
  * Chưa có command kiểm thử `pftest` cố tình gây page fault có compile-guard.
  * Địa chỉ virtual 0 (Null pointer) cần đảm bảo luôn unmapped.

---

## 6. Trạng thái Kernel Panic Hiện tại
* **Hiện trạng**: Chưa có hệ thống panic tập trung (`panic.h` / `panic.c`).
* Khi gặp lỗi không xử lý được, kernel chỉ gọi `default_exception_panic()` trong `interrupts.c` hoặc gọi `log_error()` kèm `cpu_halt()`.
* Chưa có hàm `panic(msg)`, `panicf(fmt, ...)`, `panic_with_frame(...)`.
* Chưa có recursion guard toàn cục chống đệ quy lặp vô hạn nếu panic xảy ra bên trong panic handler.
* Chưa xuất uptime hệ thống, CPU id, Thread/Process id, giá trị CR3, và chưa có stack trace.

---

## 7. An toàn Đa luồng của Kernel Heap
* **File**: `mm/heap.c`.
* Có `heap_lock` kiểu `struct kos_spinlock`.
* **Lỗ hổng nghiêm trọng**: Khóa bằng `spinlock_lock()` thay vì `spinlock_lock_irqsave()`. Nếu interrupt xảy ra trong khi một luồng đang giữ `heap_lock`, và handler ngắt lại gọi `kmalloc`/`kfree`, hệ thống sẽ deadlock vĩnh viễn trên cùng CPU.
* Chưa có metadata thống kê allocation/free bytes, chưa có canary phát hiện buffer overflow và chưa có poison memory để phát hiện double-free / use-after-free.

---

## 8. An toàn Đa luồng của PMM (Physical Memory Manager)
* **File**: `mm/pmm.c`.
* Có `pmm_lock` kiểu `struct kos_spinlock`.
* Tương tự heap, dùng spinlock thông thường không có `irqsave`, tiềm ẩn nguy cơ deadlock với ISR.

---

## 9. An toàn Đa luồng của VMM (Virtual Memory Manager)
* **File**: `mm/vmm.c`.
* Có `vmm_lock` kiểu `struct kos_spinlock`.
* Tương tự, chưa có `irqsave`. Bảng trang cấp 4 (PML4) cần chiến lược đồng bộ an toàn khi có nhiều luồng cùng thao tác map/unmap.

---

## 10. An toàn Đa luồng của Hệ thống Tệp VFS & RAMFS
* **File**: `fs/vfs.c`, `fs/ramfs.c`.
* **Hiện trạng**: Hoàn toàn **KHÔNG CÓ LOCK**.
* Mảng `volumes[VFS_MAX_VOLUMES]` và danh sách inode/file trong `ramfs.c` được đọc/ghi trực tiếp mà không có bất kỳ cơ chế đồng bộ nào. Khi chuyển sang preemptive multitasking, các tác vụ cạnh tranh đọc/ghi tệp sẽ gây hỏng dữ liệu.

---

## 11. Dữ liệu Toàn cục trong Ngăn xếp Mạng (Network Stack)
* **File**: `net/arp.c`, `net/dhcp.c`, `net/dns.c`, `net/socket.c`, `net/tcp.c`, `drivers/e1000.c`.
* **Hiện trạng**:
  * `arp_table` không có lock bảo vệ giữa luồng phân giải và ngắt mạng.
  * Bảng socket UDP/RAW `sockets[]` trong `socket.c` không có lock.
  * Bộ giải mã DNS client sử dụng bộ đệm tĩnh toàn cục không có đồng bộ.
  * `net/tcp.c` có `tcp_lock`, nhưng cần kiểm tra tính an toàn khi timer tick và tác vụ cùng can thiệp vào máy trạng thái TCP (TIME_WAIT, retransmission).
  * Vòng đệm e1000 RX/TX DMA descriptors cần bảo vệ chống race condition giữa ISR/polling và transmission.

---

## 12. Cấu trúc Tác vụ Hiện tại (`struct task`)
* **File**: `kernel/task.c`.
* Các trường hiện có:
  ```c
  struct task {
      struct task_context context;
      task_entry entry;
      void *argument;
      uint8_t *stack;
      const char *name;
      enum task_state state;
      uint64_t id;
  };
  ```
* **Thiếu sót**: Thiếu con trỏ trỏ tới process cha, thiếu quantum/time slice còn lại, wake deadline cho sleep, đối tượng chờ (wait object), mã thoát (exit code), biến đếm lồng nhau `preempt_count`, reference count, và danh sách liên kết runqueue/waitqueue.

---

## 13. Khái niệm Process vs Task
* Kernel KOS hiện tại **CHỈ CÓ TÁC VỤ (TASK)** phẳng, **CHƯA CÓ PROCESS**.
* Chưa có cấu trúc đại diện cho vùng chứa tài nguyên (`struct process`), chưa có bảng handle, chưa có danh sách thread thuộc process.

---

## 14. Bộ đếm Tham chiếu (Refcount)
* Kernel hiện tại **CHƯA CÓ** bất kỳ bộ đếm tham chiếu (refcount primitive) nào.

---

## 15. Cơ chế Trừu tượng hóa Handle (Handle Table)
* Kernel hiện tại **CHƯA CÓ** Handle Table. Các tài nguyên (socket, file) đang được tham chiếu bằng chỉ số mảng thô (raw integer index) hoặc con trỏ trực tiếp, không có generation check để chống stale handle.

---

## 16. Bảng Ký hiệu (Symbol Table) phục vụ Stack Trace
* Hiện tại kernel ELF chưa có bảng ký hiệu được nhúng để giải mã địa chỉ RIP tại thời điểm chạy (runtime symbol resolution).
* Cần cơ chế duyệt chuỗi frame pointer RBP (`-fno-omit-frame-pointer`) và bảng ký hiệu (function name + start address) để hiển thị tên hàm trong stack trace.

---

## 17. Danh mục TODO / FIXME / Placeholder
* Không có chuỗi `TODO` hay `FIXME` trong codebase.
* Tuy nhiên, các hàm syscall (`kernel/syscall.c`) đang từ chối `write` và `exit` với chú thích chờ process model.
* Lệnh `sysinfo` đang hiển thị: `Process isolation: unavailable`, `Scheduler: cooperative round-robin`.
