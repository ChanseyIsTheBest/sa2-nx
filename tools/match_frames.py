#!/usr/bin/env python3
"""banjo-nx: pairs moments of two recordings of the same scripted run (emulated reference vs native renderer) by
content, not by time: the two renderers load at different speeds and drift apart during cutscenes.

Both videos are sampled at 2 fps as 64x36 grayscale thumbnails; each native sample is matched to the most similar
emulated sample, searching forward from the previous match (the order of scenes is the same). N well-matched
moments spread over the run are written as side-by-side pairs (emulated left, native right) plus a summary sheet.

Usage: python tools/match_frames.py <emulated.mp4> <native.mp4> <output folder> [pairs, default 12]
"""
import pathlib
import subprocess
import sys

import numpy as np

FFMPEG = "C:/ffmpeg/ffmpeg.exe"
W, H, FPS = 64, 36, 2


def thumbs(video):
    raw = subprocess.run([FFMPEG, "-loglevel", "error", "-i", video, "-vf", "fps=%d,scale=%d:%d,format=gray" % (FPS, W, H),
                          "-f", "rawvideo", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, H, W).astype(np.float32)


def main():
    emu_video, nat_video, out = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 12
    out.mkdir(parents=True, exist_ok=True)
    emu, nat = thumbs(emu_video), thumbs(nat_video)
    # Normalized correlation of the thumbnails, then the monotonic alignment (scene order is the same) with the
    # largest total similarity: score[i][j] = sim[i][j] + best score of row i-1 up to column j.
    def normalize(frames):
        flat = frames.reshape(len(frames), -1)
        flat = flat - flat.mean(axis=1, keepdims=True)
        norm = np.linalg.norm(flat, axis=1, keepdims=True)
        return flat / np.maximum(norm, 1e-3), norm[:, 0]
    a, a_norm = normalize(nat)
    b, b_norm = normalize(emu)
    sim = a @ b.T  # native x emulated
    sim[a_norm < 50, :] = 0.0  # flat (black) frames match anything
    score = np.zeros_like(sim)
    back = np.zeros(sim.shape, dtype=np.int64)
    best_prev = np.zeros(sim.shape[1])
    best_prev_at = np.zeros(sim.shape[1], dtype=np.int64)
    for i in range(sim.shape[0]):
        score[i] = sim[i] + best_prev
        back[i] = best_prev_at
        running = np.maximum.accumulate(score[i])
        arg = np.zeros(sim.shape[1], dtype=np.int64)
        current = 0
        for j in range(sim.shape[1]):
            if score[i, j] >= score[i, current]:
                current = j
            arg[j] = current
        best_prev, best_prev_at = running, arg
    j = int(score[-1].argmax())
    path = []
    for i in range(sim.shape[0] - 1, -1, -1):
        path.append((i, j, float(sim[i, j])))
        j = int(back[i, j])
    path.reverse()
    matches = [(i, j, 1.0 - s_) for i, j, s_ in path if a_norm[i] >= 50]
    good = [m for m in matches if m[2] < 0.25]  # correlation above 0.75
    picks = [good[int(k * (len(good) - 1) / max(count - 1, 1))] for k in range(count)]
    tiles = []
    for k, (i, j, error) in enumerate(picks):
        t_nat, t_emu = i / FPS, j / FPS
        pair = out / ("pair_%02d.png" % k)
        subprocess.run([FFMPEG, "-loglevel", "error", "-y", "-ss", "%.2f" % t_emu, "-i", emu_video, "-ss", "%.2f" % t_nat,
                        "-i", nat_video, "-filter_complex",
                        "[0]scale=640:360,trim=end_frame=1[a];[1]scale=640:360,trim=end_frame=1[b];[a][b]hstack",
                        "-frames:v", "1", str(pair)], check=True)
        tiles.append(pair)
        print("pair %02d: native %.1f s <-> emulated %.1f s (1 - correlation %.2f)" % (k, t_nat, t_emu, error))
    # Summary sheet: pairs scaled down, 2 per row.
    inputs = []
    for t in tiles:
        inputs += ["-i", str(t)]
    rows = (len(tiles) + 1) // 2
    graph = "".join("[%d]scale=640:180[s%d];" % (k, k) for k in range(len(tiles)))
    if len(tiles) % 2:
        graph += "[s%d]pad=640:180[s%d];" % (len(tiles) - 1, len(tiles) - 1)
        graph += "color=black:640x180:d=1[blank];"
    row_names = []
    for r in range(rows):
        a = "s%d" % (2 * r)
        b = "s%d" % (2 * r + 1) if 2 * r + 1 < len(tiles) else "blank"
        graph += "[%s][%s]hstack[r%d];" % (a, b, r)
        row_names.append("[r%d]" % r)
    graph += "".join(row_names) + "vstack=inputs=%d" % rows if rows > 1 else "[r0]copy"
    subprocess.run([FFMPEG, "-loglevel", "error", "-y"] + inputs + ["-filter_complex", graph, "-frames:v", "1",
                                                                     str(out / "pairs_sheet.png")], check=True)
    print(out / "pairs_sheet.png")


if __name__ == "__main__":
    main()
