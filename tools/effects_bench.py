"""Effects bench: runs the real app (GPU chain, vision worker) against the fake phone streaming a photo,
dumps what the virtual camera would get, and measures each effect against a no-effects baseline.

  python3 tools/effects_bench.py [path/to/rng-lens]     (needs Xvfb or a display, onnxruntime, numpy, PIL)
"""
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

import numpy as np
import onnxruntime as ort
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "desktop/build-linux/rng-lens")
OUT = os.path.join(ROOT, "tools/bench-effects")
MODELS = os.path.join(ROOT, "third_party/vision/models")
PHOTO_URL = "https://raw.githubusercontent.com/opencv/opencv/4.x/samples/data/messi5.jpg"
PORT = 38490

os.makedirs(OUT, exist_ok=True)
photo = os.path.join(OUT, "photo.jpg")
if not os.path.exists(photo):
    urllib.request.urlretrieve(PHOTO_URL, photo)
bgpic = os.path.join(OUT, "bg.png")
grad = np.zeros((360, 640, 3), np.uint8)
grad[..., 0] = 30
grad[..., 1] = np.linspace(60, 200, 640)[None, :].astype(np.uint8)
grad[..., 2] = 220
Image.fromarray(grad).save(bgpic)

seg = ort.InferenceSession(os.path.join(MODELS, "human_segmentation_pphumanseg_2023mar.onnx"))
yunet = ort.InferenceSession(os.path.join(MODELS, "face_detection_yunet_2023mar.onnx"))


def person_mask(img):
    x = np.asarray(img.resize((192, 192)), np.float32) / 255.0
    x = ((x - 0.5) / 0.5).transpose(2, 0, 1)[None]
    p = seg.run(None, {"x": x})[0][0, 1]
    return np.asarray(Image.fromarray((p * 255).astype(np.uint8)).resize(img.size)) / 255.0


def face(img):
    """Largest YuNet face as (cx, cy, h) in 0..1, or None."""
    w, h = img.size
    s = min(640 / w, 640 / h)
    sm = img.resize((int(w * s), int(h * s)))
    canvas = np.zeros((640, 640, 3), np.float32)
    canvas[: sm.size[1], : sm.size[0]] = np.asarray(sm, np.float32)
    x = canvas[..., ::-1].transpose(2, 0, 1)[None].copy()
    names = [o.name for o in yunet.get_outputs()]
    res = dict(zip(names, yunet.run(None, {"input": x})))
    best = None
    for st in (8, 16, 32):
        cls, obj, box = res[f"cls_{st}"][0, :, 0], res[f"obj_{st}"][0, :, 0], res[f"bbox_{st}"][0]
        cols = 640 // st
        score = np.sqrt(np.clip(cls, 0, 1) * np.clip(obj, 0, 1))
        for i in np.where(score > 0.6)[0]:
            r, c = divmod(i, cols)
            cx, cy = (c + box[i, 0]) * st, (r + box[i, 1]) * st
            bh = np.exp(box[i, 3]) * st
            if best is None or bh > best[2]:
                best = (cx / sm.size[0], cy / sm.size[1], bh / sm.size[1])
    return best


def lapvar(a, region=None):
    g = a.astype(np.float32).mean(axis=2)
    lap = np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:])
    if region is not None:
        lap = lap[region[1:-1, 1:-1]]
    return float((lap ** 2).mean())


def mae(a, ref, region=None):
    d = np.abs(a.astype(np.float32) - ref.astype(np.float32)).mean(axis=2)
    return float(d[region].mean() if region is not None else d.mean())


def run(name, filters, source=photo, frames=150):
    home = os.path.join(OUT, "home-" + name)
    shutil.rmtree(home, ignore_errors=True)
    os.makedirs(os.path.join(home, "rng-lens"))
    slot = {"autoRotate": False, "outWidth": 1280, "outHeight": 720, "virtualCamera": False, "autoConnect": False,
            "filters": filters}
    json.dump({"slots": [slot, {}], "audio": False}, open(os.path.join(home, "rng-lens", "rng-lens.json"), "w"))
    phone = subprocess.Popen(["java", "-cp", os.path.join(ROOT, "tools/bench-out/classes"), "FakePhone", str(PORT), "opus",
                              source], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    png = os.path.join(OUT, name + ".png")
    env = dict(os.environ, XDG_CONFIG_HOME=home, HOME=home)
    try:
        r = subprocess.run([BIN, "--connect", f"127.0.0.1:{PORT}", "--dump-output", png, "--dump-after", str(frames)],
                           env=env, capture_output=True, text=True, timeout=90)
        print(f"  {name}: {r.stdout.strip()}")
    finally:
        phone.kill()
        phone.wait()
    return Image.open(png).convert("RGB")


results = []


def check(ok, what):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + what)


base = run("baseline", {})
b = np.asarray(base)
mask = person_mask(base) > 0.5
f0 = face(base)
print(f"  baseline: person {mask.mean():.0%} of frame, face {f0}")

a = np.asarray(run("bw", {"look": "bw"})).astype(np.float32)
check(np.abs(a - a.mean(axis=2, keepdims=True)).mean() < 2, "black & white look has no colour left")

a = np.asarray(run("bright", {"brightness": 0.6})).astype(np.float32)
check(a.mean() > b.mean() + 15, f"brightness +0.6 lifts the picture ({b.mean():.0f} -> {a.mean():.0f})")

a = np.asarray(run("sharpen", {"sharpen": 1.0}))
check(lapvar(a) > lapvar(b) * 1.3, f"sharpen adds detail ({lapvar(b):.0f} -> {lapvar(a):.0f})")

noisy = np.asarray(run("noisy", {}, source=photo + "+noise"))
a = np.asarray(run("smooth", {"smooth": 1.0}, source=photo + "+noise"))
if f0:
    h, w = b.shape[:2]
    fr = np.zeros((h, w), bool)
    cx, cy, fh = f0
    fr[int((cy - fh / 2) * h):int((cy + fh / 2) * h), int((cx - fh / 3) * w):int((cx + fh / 3) * w)] = True
    check(lapvar(a, fr) < lapvar(noisy, fr) * 0.7,
          f"skin smoothing cleans grain off the face (detail {lapvar(noisy, fr):.0f} -> {lapvar(a, fr):.0f}, "
          f"error vs clean {mae(noisy, b, fr):.1f} -> {mae(a, b, fr):.1f})")
    nf = ~fr & ~mask
    check(lapvar(a, nf) > lapvar(noisy, nf) * 0.8, f"smoothing leaves non-skin alone ({lapvar(noisy, nf):.0f} -> {lapvar(a, nf):.0f})")

night = np.asarray(run("night", {"night": 1.0}, source=photo + "+noise"))
check(lapvar(night) < lapvar(noisy) * 0.5 and mae(night, b) < mae(noisy, b) * 0.8,
      f"night mode removes grain (detail {lapvar(noisy):.0f} -> {lapvar(night):.0f}, error vs clean {mae(noisy, b):.1f} -> {mae(night, b):.1f})")

a = np.asarray(run("blur", {"background": "blur", "blur": 0.8}))
bgr = ~person_mask(base).__gt__(0.3)
core = person_mask(base) > 0.9
check(lapvar(a, bgr) < lapvar(b, bgr) * 0.35, f"background blurred ({lapvar(b, bgr):.0f} -> {lapvar(a, bgr):.0f})")
check(lapvar(a, core) > lapvar(b, core) * 0.6, f"person stays sharp ({lapvar(b, core):.0f} -> {lapvar(a, core):.0f})")

a = np.asarray(run("picture", {"background": "image", "backgroundImage": bgpic})).astype(np.float32)
blue = a[..., 2][bgr].mean() - a[..., 0][bgr].mean()
check(blue > 120, f"background replaced by the picture (blue minus red in background: {blue:.0f})")

af = run("autoframe", {"autoFrame": True, "frameTightness": 0.8, "frameSpeed": 1.0}, frames=240)
f1 = face(af)
if f0 and f1:
    check(f1[2] > f0[2] * 1.5, f"auto-frame zooms in on the face (face height {f0[2]:.0%} -> {f1[2]:.0%})")
    check(abs(f1[0] - 0.5) < 0.15, f"auto-frame centres the face horizontally (x = {f1[0]:.2f})")
else:
    check(False, f"faces found for auto-frame check (baseline {f0}, framed {f1})")

print(f"effects bench: {sum(results)} passed, {len(results) - sum(results)} failed")
sys.exit(0 if all(results) else 1)
