# N9 — User-space boundary

## Slice implemented now

- GDT có user code/data selectors (`0x2b`/`0x33`, DPL 3).
- IDT vector `0x80` có DPL 3; Ring 3 chỉ được vào kernel qua gate này.
- `kernel/syscall.c` định nghĩa ABI register ổn định:
  `RAX=number`, `RDI..R9=arguments`, kết quả ở `RAX`.
- Syscall numbers đầu tiên: `GETTIME`, `SYSINFO`, `WRITE`, `EXIT`.
- `syscallinfo` hiển thị ABI trong terminal.

Các syscall `WRITE` và `EXIT` hiện trả về lỗi an toàn vì chưa có process object
và kiểm tra con trỏ user. Không cho phép một lệnh thử làm dừng bootstrap kernel.

## Thứ tự tiếp theo

1. Tạo `process`/`address_space` object và page table riêng cho mỗi process.
2. Thêm user-bit mapping, guard page, NX và copy dữ liệu kernel↔user có kiểm tra
   range; không dùng trực tiếp pointer Ring 3.
3. Mở rộng `fs/elf.c` thành ELF loader: PT_LOAD, W^X, relocation tối thiểu,
   user stack và `exec`.
4. Context switch lưu CR3/TSS `rsp0`, định tuyến page fault theo process và
   thu hồi toàn bộ page khi process thoát.
5. Implement `read/write/exit`, pipe và socket syscall sau khi memory check đã
   có; khi đó mới build `curl` thành ELF user-mode.

## Trạng thái kiến trúc

`curl`, DNS và TLS vẫn chạy trong kernel để giữ bản debug hiện tại. N9 chỉ được
đánh dấu hoàn thành khi một ELF user-mode có thể gọi socket syscall và kernel
vẫn sống khi ELF đó bị lỗi.
