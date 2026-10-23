"""Assemble a tour GIF from frames written by a renderer: <dir>/frames.txt lists
"frame_NNN.bmp duration_ms" per line.

    python tools/make_gif.py <frames dir> <out.gif> [--scale 2]
"""
import argparse
import os

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames")
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args()

    entries = []
    with open(os.path.join(args.frames, "frames.txt"), encoding="utf-8") as f:
        for line in f:
            name, ms = line.split()
            entries.append((os.path.join(args.frames, name), int(ms)))

    frames = [Image.open(p).convert("RGB") for p, _ in entries]
    # One palette for the whole GIF, so colors don't shift between scenes.
    sheet = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
    for i, fr in enumerate(frames):
        sheet.paste(fr, (0, i * fr.height))
    palette = sheet.quantize(colors=256, method=Image.Quantize.MEDIANCUT)

    out = []
    for fr in frames:
        if args.scale != 1:
            fr = fr.resize((fr.width * args.scale, fr.height * args.scale), Image.Resampling.NEAREST)
        out.append(fr.quantize(palette=palette, dither=Image.Dither.NONE))
    out[0].save(args.out, save_all=True, append_images=out[1:], loop=0,
                duration=[ms for _, ms in entries], optimize=True, disposal=1)
    print(f"{args.out}: {len(out)} frames, {sum(ms for _, ms in entries) / 1000:.1f} s, "
          f"{os.path.getsize(args.out) / 1024:.0f} KB")


if __name__ == "__main__":
    main()
