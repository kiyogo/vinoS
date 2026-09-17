# N7 — Terminal web reader

N7 không phải browser. `curl --text` biến body HTML thành văn bản giới hạn
an toàn để đọc trong console; `curl` không có cờ này giữ nguyên body như Windows.

## Đã triển khai

- tokenizer tuyến tính, bỏ qua tag và toàn bộ nội dung `script`, `style`,
  `svg`, `head`;
- giữ text node, giải mã `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`, `&nbsp;`;
- xuống dòng cho `br`, `p`, `div`, `li`, `h1`–`h6`, `section`, `article`;
- word-wrap theo số cột thật của framebuffer console;
- giới hạn phần HTML đem đi hiển thị ở 8192 byte và tối đa 200 dòng, sau đó in thông báo
  `[output truncated by KOS safety limit]`;
- hỗ trợ `curl URL`, `curl --text URL` và `curl --text -L URL`.

## Kiểm thử QEMU

Bootstrap chạy `curl --text example.com` và phải cho kết quả dạng:

```text
Example Domain
This domain is for use in documentation examples without needing permission.
```

N7 không thực thi JavaScript, không tải CSS/ảnh và không cố biến terminal thành
trình duyệt đồ họa.
