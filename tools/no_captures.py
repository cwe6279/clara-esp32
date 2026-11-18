#!/usr/bin/env python3
"""no_captures.py: refuse to commit a capture (a photo, a scan, a recording, a session log).

    python3 tools/no_captures.py            check what is staged (the pre-commit hook runs this)
    python3 tools/no_captures.py --tracked  check every file git already tracks

Run as .githooks/pre-commit (git config core.hooksPath .githooks). A commit is refused when it
adds or changes:
  * anything under captures/, snapshots/, recordings/, emergencies/, logs/ or dreams/;
  * a file named like a capture: photo-*, scan-*, snapshot*, last-look*, mic-*, note.json;
  * any image, audio or video file outside the folders allowed for art and docs: by default
    faces/ and docs/, or the globs in .media-allow at the repo root, one per line.
Its own exit code is the verdict, so `git commit --no-verify` is the only way past it.
"""
import fnmatch
import subprocess
import sys
from pathlib import Path

CAPTURE_DIRS = ("captures", "snapshots", "recordings", "emergencies", "logs", "dreams")
CAPTURE_NAMES = ("photo-*", "scan-*", "snapshot*", "last-look*", "mic-*", "note.json")
MEDIA = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".heic", ".heif", ".tif", ".tiff",
         ".wav", ".mp3", ".ogg", ".oga", ".flac", ".m4a", ".aac", ".opus", ".webm",
         ".mp4", ".mov", ".avi", ".mkv", ".mjpeg", ".mjpg", ".h264", ".raw", ".yuv", ".pcm"}
DEFAULT_ALLOW = ("faces/*", "docs/*")


def allowed_globs(root: Path) -> list:
    f = root / ".media-allow"
    if not f.exists():
        return list(DEFAULT_ALLOW)
    return [ln.strip() for ln in f.read_text().splitlines() if ln.strip() and not ln.startswith("#")]


def why_refused(path: str, allow: list) -> str:
    p = Path(path)
    parts = [x.lower() for x in p.parts]
    if any(d in parts[:-1] for d in CAPTURE_DIRS):
        return "it is in a captures folder"
    name = p.name.lower()
    if any(fnmatch.fnmatch(name, pat) for pat in CAPTURE_NAMES):
        return "it is named like a capture"
    if p.suffix.lower() in MEDIA and not any(fnmatch.fnmatch(path, g) for g in allow):
        return "it is an image or recording outside the art and docs folders (.media-allow)"
    return ""


def main(argv: list) -> int:
    root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                               text=True, check=True).stdout.strip())
    if "--tracked" in argv:
        files = subprocess.run(["git", "ls-files"], capture_output=True, text=True, cwd=root).stdout.split("\n")
    else:
        files = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
                               capture_output=True, text=True, cwd=root).stdout.split("\n")
    allow = allowed_globs(root)
    bad = [(f, why_refused(f, allow)) for f in files if f]
    bad = [(f, w) for f, w in bad if w]
    if not bad:
        return 0
    print("Refusing to commit what looks like a capture (photos, scans, recordings and logs stay out of git):")
    for f, w in bad:
        print(f"  {f}: {w}")
    print("Unstage it with `git restore --staged <file>`. If it really is art or documentation, "
          "add its folder to .media-allow.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
