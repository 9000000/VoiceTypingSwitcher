# VoiceTypingSwitcher

**Tiếng Việt** | [English](README.en.md)

> **VoiceTypingSwitcher** là tiện ích chạy khay hệ thống siêu nhẹ cho Windows 11 (C++ Win32 thuần, ~277 KB, ~2,5 MB RAM). Khi bấm phím tắt kích hoạt (`Win+H`, `Win+C` hoặc tổ hợp phím tự gán), app tự chuyển bàn phím sang ngôn ngữ đọc (mặc định Tiếng Việt) rồi mới mở Voice Typing để nhận dạng đúng ngay lập tức; khi kết thúc bằng `Enter`, nhấp chuột ra ngoài, `Esc` hoặc nút `X`, app tự tắt Voice Typing và trả về ngôn ngữ chỉ định hoặc ngôn ngữ trước đó.

- Bấm **phím tắt kích hoạt** (mặc định **Win+H**, có thể đổi sang **Win+C** hoặc tự gán tổ hợp phím bất kỳ): tự đổi bàn phím sang ngôn ngữ đọc (mặc định **Tiếng Việt**) rồi mới mở Voice Typing.
- Khi kết thúc (**Enter**, **nhấp chuột ra ngoài** thanh Voice Typing, **Esc**, nút **X**, hoặc thanh tự đóng): nếu bấm **Enter** hoặc **nhấp chuột ra ngoài**, app tự gửi `Win+H` để **đóng luôn thanh Voice Typing** rồi đổi về
  - **ngôn ngữ cố định** (mặc định English US), hoặc
  - **ngôn ngữ trước khi kích hoạt**.

## Sử dụng
Chạy `bin\VoiceTypingSwitcher.exe`. Nhấp chuột phải (hoặc trái) vào icon micro ở khay hệ thống:

| Mục | Ý nghĩa |
|---|---|
| Bật | Bật/tắt tính năng (khi tắt, phím tắt hoạt động như mặc định) |
| Phím tắt | Chọn nhanh `Win + H`, `Win + C`, hoặc `Gán phím khác...` (vd: `Alt+Q`, `Ctrl+Shift+V`, `F9`) |
| Ngôn ngữ khi Voice Typing | Chọn ngôn ngữ đổi sang khi kích hoạt |
| Ngôn ngữ khi thoát | Chọn ngôn ngữ cố định, hoặc "Ngôn ngữ trước khi kích hoạt" |
| Khởi động cùng Windows | Ghi vào `HKCU\...\Run` |
| Thoát | Đóng app |

Cấu hình lưu tại `HKCU\Software\VoiceTypingSwitcher`. Danh sách ngôn ngữ lấy từ các bàn phím đã cài trong Settings › Time & language.

## Build
Cần MinGW-w64: `winget install BrechtSanders.WinLibs.POSIX.UCRT`

```bat
build.cmd   :: ra bin\VoiceTypingSwitcher.exe
test.cmd    :: test liệt kê bàn phím + đọc cấu hình
```

## Chẩn đoán
- Log phiên gần nhất được ghi tự động tại `%TEMP%\VoiceTypingSwitcher.log` (mỗi lần kích hoạt ghi đè mới).
- `VoiceTypingSwitcher.exe --probe` ghi `probe.log` (cạnh file exe) trong 60 giây, liệt kê các cửa sổ và phần tử UIA của `TextInputHost.exe` mỗi khi thay đổi.

## Giới hạn
- Nếu cửa sổ đang dùng chạy bằng quyền **Administrator**, app (chạy quyền thường) không bắt được phím ở cửa sổ đó.
- Nhấp vào chính thanh Voice Typing (nút mic, cài đặt) sẽ không đổi ngôn ngữ. Đóng bằng nút X thì app nhận ra trong khoảng 1 giây.

## Cấu trúc
```
src/main.cpp      tray, menu, hộp thoại gán phím tắt, máy trạng thái phiên
src/hooks.cpp     hook bàn phím (phím tắt kích hoạt, Enter, Esc) + hook chuột (chỉ bật trong phiên)
src/lang.cpp      liệt kê / đổi bàn phím (WM_INPUTLANGCHANGEREQUEST)
src/voicebar.cpp  nhận biết thanh Voice Typing (TextInputHost.exe) + --probe
src/config.cpp    registry, khởi động cùng Windows
```
