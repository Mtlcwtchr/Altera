#!/usr/bin/env python3
"""Download Sketchfab models listed in a candidates JSON using YOUR API token.

    set SKETCHFAB_TOKEN=<token from sketchfab.com/settings#password>   (PowerShell: $env:SKETCHFAB_TOKEN="...")
    python tools/sketchfab_download.py doc/sketchfab_candidates.json [--format gltf] [--dest assets/downloaded/sketchfab] [--limit N]

Writes <dest>/<uid>/ (archive extracted) + source.json (name, author, license, url) per model.
Progress is saved to <dest>/download_report.json. Completed models are skipped on reruns.
Use --keep-retrying to wait for API throttling to lift during unattended downloads.
"""
import argparse, json, os, sys, time, urllib.request, urllib.error, zipfile
import concurrent.futures
import shutil
import tempfile
import threading
from pathlib import Path
tok = os.environ.get("SKETCHFAB_TOKEN")
if not tok: sys.exit("set SKETCHFAB_TOKEN")
ap = argparse.ArgumentParser(); ap.add_argument("list"); ap.add_argument("--format", default="gltf")
ap.add_argument("--dest", default="assets/downloaded/sketchfab"); ap.add_argument("--limit", type=int, default=10**9)
ap.add_argument("--workers", type=int, default=3)
ap.add_argument("--keep-retrying", action="store_true")
a = ap.parse_args()
H = {"Authorization": "Token " + tok, "User-Agent": "campfire-asset-fetch"}
request_lock = threading.Lock()
next_request = 0.0
dest = Path(a.dest)
dest.mkdir(parents=True, exist_ok=True)
with open(a.list, encoding="utf8") as source:
    models = list({m["uid"]: m for m in json.load(source)[:a.limit]}.values())


def fetch(m):
    d = dest / m["uid"]
    if (d / "source.json").exists():
        return {"uid": m["uid"], "status": "existing"}
    attempt = 0
    while attempt < 6:
        try:
            global next_request
            while True:
                with request_lock:
                    delay = next_request - time.monotonic()
                    if delay <= 0:
                        next_request = time.monotonic() + 3.0
                        break
                time.sleep(min(delay, 1.0))
            request = urllib.request.Request(
                f"https://api.sketchfab.com/v3/models/{m['uid']}/download", headers=H)
            with urllib.request.urlopen(request, timeout=60) as response:
                formats = json.load(response)
            fmt = formats.get(a.format) or formats.get("gltf")
            if not fmt:
                fmt = next((v for v in formats.values()
                            if isinstance(v, dict) and "url" in v), None)
            if not fmt:
                return {"uid": m["uid"], "status": "failed", "error": "No downloadable format"}
            # Keep large archives on disk and never persist temporary signed URLs.
            with tempfile.TemporaryFile(dir=dest) as archive:
                with urllib.request.urlopen(fmt["url"], timeout=600) as response:
                    shutil.copyfileobj(response, archive)
                size = archive.tell()
                archive.seek(0)
                with zipfile.ZipFile(archive) as z:
                    root = d.resolve()
                    for member in z.infolist():
                        if not (d / member.filename).resolve().is_relative_to(root):
                            raise ValueError("Unsafe archive path")
                    z.extractall(d)
            (d / "source.json").write_text(
                json.dumps(m, ensure_ascii=False, indent=1), encoding="utf8")
            return {"uid": m["uid"], "status": "downloaded", "bytes": size}
        except urllib.error.HTTPError as e:
            # HTTPError strings contain URLs; store only the response status.
            error = f"HTTP {e.code}"
            if e.code not in (408, 429, 500, 502, 503, 504):
                break
            delay = e.headers.get("Retry-After", "")
            wait = int(delay) if delay.isdigit() else 15 * (attempt + 1)
            if e.code == 429:
                wait = max(wait, min(900, 120 * (attempt + 1)))
                with request_lock:
                    next_request = max(next_request, time.monotonic() + wait)
                print(f"Throttled: waiting {wait}s before retrying {m['name']}", flush=True)
                if a.keep_retrying:
                    # A service quota must not discard the remaining download queue.
                    attempt = min(attempt + 1, 5)
                    continue
            else:
                time.sleep(wait)
        except Exception as e:
            error = type(e).__name__
            time.sleep(2 * (attempt + 1))
        attempt += 1
    return {"uid": m["uid"], "status": "failed", "error": error}


results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.workers)) as pool:
    futures = {pool.submit(fetch, m): m for m in models}
    for future in concurrent.futures.as_completed(futures):
        m = futures[future]
        result = future.result()
        results.append(result)
        print(f"[{len(results)}/{len(models)}] {result['status']} {m['name']} "
              f"{result.get('error', '')}", flush=True)
        (dest / "download_report.json").write_text(
            json.dumps(results, ensure_ascii=False, indent=2), encoding="utf8")
failed = sum(r["status"] == "failed" for r in results)
print(f"Finished: {len(results) - failed} available, {failed} failed", flush=True)
sys.exit(1 if failed else 0)
