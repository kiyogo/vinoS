# N6 — TLS/HTTPS client

## Mục tiêu và nguyên tắc

TLS được tách khỏi TCP/HTTP và không được phép tự hạ xuống plaintext sau khi
người dùng yêu cầu `https://`. Khóa, nonce và `ClientHello.random` chỉ được
sinh từ nguồn entropy mật mã; nếu máy không có nguồn đó, handshake thất bại
với lỗi rõ ràng.

## Đã có trong N6 hiện tại

- `lib/crypto.c`: SHA-256, HMAC-SHA256, HKDF-SHA256 và AES-128-GCM.
- Self-test SHA-256 (RFC 6234), HKDF và vector AES-GCM NIST trước khi kernel
  nhận lệnh.
- Phát hiện `RDRAND` bằng CPUID; không dùng timestamp đơn độc làm khóa.
- `include/kos/tls.h` và `net/tls.c`: session/state/error API. API từ chối
  handshake chưa hoàn chỉnh thay vì gửi HTTP giả trên kết nối TLS.
- `curl https://...` dùng cổng 443, SNI hostname được giữ trong session và
  trả về lỗi `TLS: TLS 1.3 handshake not implemented` hiện tại.
- `curl example.com` dùng HTTP mặc định để hoạt động ổn định trước khi TLS hoàn tất;
  dùng `curl --https example.com` hoặc `curl https://example.com` để yêu cầu TLS.

## Phần bắt buộc tiếp theo

1. Bổ sung ChaCha20-Poly1305 và test vector RFC 8439.
2. Bổ sung X25519 và ECDSA/RSA verify từ thư viện crypto freestanding.
3. Viết TLS 1.3 record codec: transcript hash, HKDF-Expand-Label,
   ClientHello/SNI/ALPN, ServerHello, EncryptedExtensions, Certificate,
   CertificateVerify và Finished.
4. Viết parser X.509 tối giản (DER), root CA store trong initramfs, kiểm tra
   hostname/SAN và thời hạn; `--insecure` chỉ là lựa chọn debug rõ ràng.
5. Nối record codec vào TCP socket với timeout và giới hạn bản ghi.
6. Tích hợp HTTP/1.1 qua TLS, redirect và các test QEMU deterministic.

## Tiêu chí hoàn thành N6

```text
KOS> curl https://example.com
HTTP 200 OK
Example Domain
```

Chỉ đánh dấu hoàn thành khi server thật gửi Certificate/Finished và chuỗi
chứng chỉ được kiểm tra; các dòng mô phỏng hoặc bỏ qua xác thực không được
tính là đạt.
