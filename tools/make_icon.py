# Generates assets/app.ico (microphone on a blue circle) using only the stdlib.
import struct, zlib, math, pathlib

def render(n):
    ss = 4  # supersampling
    px = bytearray()
    for y in range(n):
        px.append(0)  # PNG filter: none
        for x in range(n):
            cov_bg = cov_fg = 0
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + .5) / ss) / n  # 0..1
                    v = (y + (sy + .5) / ss) / n
                    if math.hypot(u - .5, v - .5) <= .5:
                        cov_bg += 1
                    if is_mic(u, v):
                        cov_fg += 1
            a_bg, a_fg = cov_bg / ss**2, cov_fg / ss**2
            bg = (37, 99, 235)
            r, g, b = (int(c * (1 - a_fg) + 255 * a_fg) for c in bg)
            px += bytes((r, g, b, int(255 * max(a_bg, a_fg))))
    return png(n, bytes(px))

def is_mic(u, v):
    # capsule
    if .40 <= u <= .60 and .22 <= v <= .55: return True
    if math.hypot(u - .5, v - .22) <= .10 or math.hypot(u - .5, v - .55) <= .10: return True
    # holder arc (lower half ring)
    d = math.hypot(u - .5, v - .52)
    if .17 <= d <= .23 and v >= .52: return True
    # stem + base
    if .47 <= u <= .53 and .72 <= v <= .82: return True
    if .36 <= u <= .64 and .80 <= v <= .85: return True
    return False

def png(n, raw):
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

sizes = [16, 20, 24, 32, 48]
imgs = [render(s) for s in sizes]
out = struct.pack("<HHH", 0, 1, len(imgs))
off = 6 + 16 * len(imgs)
for s, d in zip(sizes, imgs):
    out += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(d), off)
    off += len(d)
out += b"".join(imgs)
p = pathlib.Path(__file__).parent.parent / "assets" / "app.ico"
p.parent.mkdir(exist_ok=True)
p.write_bytes(out)
print("wrote", p, len(out), "bytes")
