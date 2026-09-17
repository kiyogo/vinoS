# N8 — Tải file và filesystem

## Đã triển khai: RAM volume D:

- `curl -o D:\\index.html http://example.com` ghi body vào RAMFS;
- ghi mới hoặc ghi đè file, tối đa 8 file động, mỗi file tối đa 8192 byte;
- `touch D:\\empty.txt` tạo file rỗng;
- `ls D:` liệt kê file trong volume;
- `cat D:\\index.html` đọc nội dung;
- `sha256 D:\\index.html` (alias `checksum`) kiểm tra SHA-256;
- `curl -o` in checksum sau khi ghi;
- dữ liệu bị mất khi reboot vì D: hiện là RAM volume;
- đường dẫn được chuẩn hóa, chặn `..`, ký tự volume lồng nhau và ghi vượt
  giới hạn.

## FAT32 — bước tiếp theo, chưa bật mặc định

FAT32 cần block-device driver (ổ đĩa QEMU/virtio hoặc AHCI), đọc MBR/GPT,
FAT table, cluster chain, long filename và cơ chế flush an toàn. Phần này sẽ
được tách khỏi RAMFS qua cùng `vfs_backend`, kiểm thử trên image riêng trước
khi cho phép ghi thật. Không giả lập FAT32 bằng RAMFS vì như vậy sẽ làm mất dữ
liệu sau reboot mà không báo cho người dùng.

## Kiểm thử N8

```text
KOS> curl -o D:\\index.html http://example.com
curl: response body saved to:
D:\\index.html
curl: SHA-256: <64 hex digits>
KOS> ls D:
README.TXT
index.html
KOS> sha256 D:\\index.html
SHA-256: <64 hex digits>
KOS> cat D:\\index.html
```
