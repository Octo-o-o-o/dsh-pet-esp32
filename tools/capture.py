"""Capture the Dial's canvas over serial (V command) and save a PNG."""
import os, sys, time, serial
from PIL import Image

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "docs", "screen.png")

ser = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.4)
ser.reset_input_buffer()
ser.write(b"V\n")

rows = {}
buf = b""
deadline = time.time() + 25
done = False
while time.time() < deadline and not done:
    chunk = ser.read(4096)
    if not chunk:
        continue
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        s = line.decode("ascii", "replace").strip()
        if s == "END":
            done = True
            break
        if s.startswith("P") and len(s) > 5:
            y = int(s[1:4])
            hexs = s[5:]
            px = [int(hexs[i:i + 4], 16) for i in range(0, len(hexs) - 3, 4)]
            # the panel buffer stores RGB565 byte-swapped
            px = [((v & 0xFF) << 8) | (v >> 8) for v in px]
            rows[y] = px
ser.close()
print("rows:", len(rows), "widths:", sorted({len(v) for v in rows.values()}))

W = max(len(v) for v in rows.values()) * 2
H = max(rows) + 2
img = Image.new("RGB", (W, H))
pix = img.load()
for y, px in rows.items():
    for i, v in enumerate(px):
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31
        for dx in (0, 1):
            for dy in (0, 1):
                if i * 2 + dx < W and y + dy < H:
                    pix[i * 2 + dx, y + dy] = (r, g, b)
img.save(out)
print("saved", out, img.size)
