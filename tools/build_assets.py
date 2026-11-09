"""Build the Dial pet asset blob from dsh-pet webm animations.

Pipeline: webm -> 10fps RGBA frames (crop+scale) -> pick best window ->
perceptual dedupe -> composite on background -> JPEG -> packed binary.

The blob also carries pre-rendered 1bpp text (status phrases and animation
labels) drawn with the pet's own font, so the firmware needs no CJK font engine.
"""
import os, sys, glob, json, struct, subprocess, io
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = r"C:\Users\satan\.dsh\profiles\desktop\node_modules\dsh-pet\assets"
WEBM = os.path.join(ASSETS, "webm")
FONT = os.path.join(ASSETS, "fonts", "上首软糖体.ttf")
FRAMES = os.environ.get("PET_FRAMES", os.path.join(ROOT, "work", "frames"))
OUT = os.environ.get("PET_OUT", os.path.join(ROOT, "firmware", "data"))
FPS = 10
CROP = tuple(int(v) for v in os.environ.get("PET_CROP", "146,2,340,340").split(","))
SIZE = int(os.environ.get("PET_SIZE", "240"))
BG = (10, 14, 26)

# name, ascii label, target seconds, loop, role
ANIMS = [
    ("待机呼吸休闲", "IDLE", 8.0, True, "idle"),
    ("东张西望", "LOOK AROUND", 2.5, False, "fun"),
    ("点击回应-开心跃动", "HAPPY HOP", 2.0, False, "click"),
    ("点击回应-害羞惊讶", "SHY", 2.0, False, "click"),
    ("点击回应-傲娇生气", "TSUNDERE", 2.0, False, "click"),
    ("点击回应-元气挥手", "WAVE", 2.0, False, "click"),
    ("点击回应-挠痒咯咯笑", "GIGGLE", 2.0, False, "click"),
    ("睡觉-开始", "SLEEP", 2.0, False, "sleep_start"),
    ("睡觉-循环", "SLEEPING", 4.0, True, "sleep_loop"),
    ("睡觉-起来", "WAKE UP", 2.0, False, "sleep_end"),
    ("工作状态-思考冒泡", "THINKING", 2.5, True, "thinking"),
    ("工作状态-忙碌点按", "WORKING", 2.5, True, "working"),
    ("工作状态-清点归档", "FILING", 2.5, True, "result"),
    ("工作状态-原地踱步张望", "WAITING", 2.5, True, "waiting"),
    ("工作状态-雀跃庆祝", "DONE", 2.5, False, "success"),
    ("工作状态-垂头叹气冒汗", "OOPS", 2.5, False, "error"),
    ("原地专心玩魔方", "RUBIK", 3.0, False, "fun"),
    ("吃白饭", "RICE", 3.0, False, "fun"),
    ("超大伸懒腰", "STRETCH", 2.5, False, "fun"),
    ("摇扇纳凉", "FAN", 3.0, False, "fun"),
    ("写代码", "CODING", 3.0, False, "fun"),
    ("可爱宅舞", "DANCE", 3.0, False, "fun"),
    ("蓝鲸现世", "WHALE", 3.0, False, "fun"),
    ("悠闲哼歌", "HUMMING", 3.0, False, "fun"),
    ("照镜子", "MIRROR", 3.0, False, "fun"),
    ("原地小憩沉眠", "NAPPING", 3.0, False, "fun"),
]

# Copy taken verbatim from dsh-pet's config.jsonc `workStatusTexts`
# (thinking / working / result / waiting / success / error).
WORK_TEXTS = [
    ["正在认真想下一步呢", "让我整理一下思路~", "正在梳理接下来要做什么"],
    ["正在处理任务呢", "这一步正在进行中哦", "正在继续干活呢，别打扰我~"],
    ["这一步处理好啦，接着看下一步", "搞定一步，继续看看哦~"],
    ["需要你确认一下呢", "这里要等你看一下哦", "轮到你来决定啦，等你~"],
    ["这一轮搞定啦，真棒！", "任务完成咯，可以摸鱼啦~"],
    ["这一步好像没跑通呢", "刚才的操作遇到点问题哦", "这里卡了一下，再等等你呢"],
]

TEXT_MAX_W = int(185 * SIZE / 240)
ID_ANIM = 100
ID_MODE = 200      # 200 AUTO, 201 MANUAL, 202 SLEEP, 203 DSH
ID_PHRASE = 300


def extract(name):
    d = os.path.join(FRAMES, name)
    files = sorted(glob.glob(os.path.join(d, "*.png")))
    if len(files) >= 10:
        return files
    os.makedirs(d, exist_ok=True)
    x, y, w, h = CROP
    vf = f"fps={FPS},crop={w}:{h}:{x}:{y},scale={SIZE}:{SIZE}:flags=lanczos,format=rgba"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-c:v", "libvpx-vp9",
                    "-i", os.path.join(WEBM, name + ".webm"), "-vf", vf,
                    os.path.join(d, "%04d.png")], check=True)
    return sorted(glob.glob(os.path.join(d, "*.png")))


def load_stack(files):
    return np.stack([np.array(Image.open(f).convert("RGBA")) for f in files])


def pick_window(arr, seconds, loop):
    n = len(arr)
    want = max(4, int(round(seconds * FPS)))
    want = min(want, n)
    if loop:
        best, best_score = 0, 1e18
        for s in range(0, max(1, n - want) + 1, 2):
            e = s + want - 1
            gap = np.abs(arr[s].astype(np.int16) - arr[e].astype(np.int16)).mean()
            best_score, best = (gap, s) if gap < best_score else (best_score, best)
        return best, want
    return 0, want


def dedupe(arr, thresh=1.2):
    keep = [0]
    for i in range(1, len(arr)):
        if np.abs(arr[i].astype(np.int16) - arr[keep[-1]].astype(np.int16)).mean() > thresh:
            keep.append(i)
    return keep


def encode_one(item):
    name, label, seconds, loop, role = item
    files = extract(name)
    arr = load_stack(files)
    s, cnt = pick_window(arr, seconds, loop)
    seg = arr[s:s + cnt]
    keep = dedupe(seg)
    bg = Image.new("RGBA", (SIZE, SIZE), BG + (255,))
    blobs, durs = [], []
    for idx, i in enumerate(keep):
        im = Image.fromarray(seg[i], "RGBA")
        comp = bg.copy()
        comp.alpha_composite(im)
        b = io.BytesIO()
        comp.convert("RGB").save(b, "JPEG", quality=80, optimize=True, subsampling=2)
        blobs.append(b.getvalue())
        nxt = keep[idx + 1] if idx + 1 < len(keep) else cnt
        durs.append(int(round((nxt - i) * 1000 / FPS)))
    return dict(label=label, role=role, loop=loop, fps=FPS, start=s,
                frames=len(blobs), blobs=blobs, durs=durs,
                bytes=sum(len(b) for b in blobs))


def render_text(text, max_w=TEXT_MAX_W):
    """Render one string to a trimmed 1bpp bitmap (MSB first, row padded to bytes)."""
    for size in (18, 17, 16, 15, 14, 13, 12, 11, 10, 9):
        font = ImageFont.truetype(FONT, size)
        img = Image.new("L", (max_w + 60, 34), 0)
        ImageDraw.Draw(img).text((4, 4), text, font=font, fill=255)
        bb = img.getbbox()
        if not bb:
            continue
        w, h = bb[2] - bb[0], bb[3] - bb[1]
        if w <= max_w or size == 9:
            crop = img.crop(bb).point(lambda v: 255 if v >= 110 else 0)
            row_bytes = (w + 7) // 8
            buf = bytearray(row_bytes * h)
            px = crop.load()
            for y in range(h):
                for x in range(w):
                    if px[x, y]:
                        buf[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)
            return dict(w=w, h=h, data=bytes(buf), size=size)
    return None


def main():
    os.makedirs(OUT, exist_ok=True)
    with ProcessPoolExecutor(max_workers=3) as ex:
        results = list(ex.map(encode_one, ANIMS))

    # ---- text bitmaps
    texts = []   # (id, dict)
    for i, a in enumerate(results):
        t = render_text(a["label"])
        if t:
            texts.append((ID_ANIM + i, t))
    for j, s in enumerate(("AUTO", "MANUAL", "SLEEP", "DSH")):
        t = render_text(s)
        if t:
            texts.append((ID_MODE + j, t))
    n = 0
    for group in WORK_TEXTS:
        for s in group:
            t = render_text(s)
            if t:
                texts.append((ID_PHRASE + n, t))
            n += 1

    # ---- layout
    anim_off = 40
    anim_size = 48
    frame_off = anim_off + anim_size * len(results)
    frame_size = 8
    nframes = sum(r["frames"] for r in results)
    text_off = frame_off + frame_size * nframes
    text_size = 12
    data_off = text_off + text_size * len(texts)

    out = bytearray()
    out += b"PETD" + struct.pack("<HHI", 2, len(results), nframes)
    out += struct.pack("<IIII", anim_off, frame_off, data_off, text_off)
    out += struct.pack("<H", len(texts))
    out += b"\x00" * 10
    assert len(out) == anim_off, (len(out), anim_off)

    role_ids = ["idle", "fun", "click", "sleep_start", "sleep_loop", "sleep_end",
                "thinking", "working", "result", "waiting", "success", "error"]
    frame_entries = []
    data = bytearray()
    total_frames = 0
    for r in results:
        first = total_frames
        for blob, dur in zip(r["blobs"], r["durs"]):
            frame_entries.append((len(data), len(blob), dur))
            data += blob
            total_frames += 1
        label = r["label"].encode("ascii")[:31].ljust(32, b"\x00")
        out += label
        out += bytes([role_ids.index(r["role"]), 1 if r["loop"] else 0, r["fps"], 0])
        out += struct.pack("<HH", first, r["frames"])
        out += b"\x00" * 8
        print(f"{r['label']:14s} start={r['start']:3d} frames={r['frames']:3d} "
              f"{r['bytes']/1024:7.1f} KB")
    assert len(out) == frame_off, (len(out), frame_off)

    for (off, size, dur) in frame_entries:
        out += struct.pack("<IHH", data_off + off, size, min(dur, 65535))
    assert len(out) == text_off, (len(out), text_off)

    text_data = bytearray()
    text_base = data_off + len(data)
    for tid, t in texts:
        out += struct.pack("<HHHHI", tid, t["w"], t["h"], 0, text_base + len(text_data))
        text_data += t["data"]
    assert len(out) == data_off, (len(out), data_off)

    out += data
    out += text_data

    path = os.path.join(OUT, "pet.bin")
    with open(path, "wb") as f:
        f.write(out)
    print(f"\npacked {len(out)/1024/1024:.2f} MB -> {path}")
    print(f"animations={len(results)} frames={nframes} texts={len(texts)}")
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(dict(animations=[dict(label=r["label"], role=r["role"], loop=r["loop"],
                                        frames=r["frames"]) for r in results],
                       frames=nframes, texts=len(texts), bytes=len(out)),
                  f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()



