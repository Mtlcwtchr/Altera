#!/usr/bin/env python3
"""Download every free upload of an itch.io page (no login; skips paid/PWYW-with-minimum pages).

    python tools/fetch_itch.py https://author.itch.io/slug [--dest assets/downloaded/itch] [--max-mb 600]

Writes <dest>/<author>__<slug>/<files> and a source.json (url, title, author, license hint, files).
"""
import argparse, html, http.cookiejar, json, os, re, sys, urllib.parse, urllib.request

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) campfire-asset-fetch"}


def opener():
    return urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))


def req(op, url, data=None, headers=None):
    h = dict(UA)
    h.update(headers or {})
    if data is not None:
        data = urllib.parse.urlencode(data).encode()
    return op.open(urllib.request.Request(url, data=data, headers=h), timeout=120)


def fetch(url, dest, max_mb):
    url = url.split("?")[0].rstrip("/")
    m = re.match(r"https://([^.]+)\.itch\.io/([^/]+)$", url)
    if not m:
        raise SystemExit("bad itch url: " + url)
    author, slug = m.groups()
    op = opener()
    page = req(op, url).read().decode("utf8", "replace")
    title = html.unescape((re.search(r"<title>(.*?)</title>", page, re.S) or [0, slug])[1]).strip()
    csrf = re.search(r'csrf_token" value="([^"]+)', page)
    if not csrf:
        raise SystemExit("no csrf (page unavailable): " + url)
    csrf = csrf.group(1)
    lic = sorted(set(re.findall(r"CC0|CC-BY[- ]?(?:NC)?[- ]?(?:SA)?[- 0-9.]*|Creative Commons[^<.]{0,40}|public domain", page, re.I)))[:6]
    try:
        r = json.loads(req(op, url + "/download_url", {"csrf_token": csrf}, {"X-Requested-With": "XMLHttpRequest"}).read())
    except Exception as e:
        raise SystemExit(f"not freely downloadable ({e}): {url}")
    dl = r["url"]
    dpage = req(op, dl).read().decode("utf8", "replace")
    csrf2 = (re.search(r'csrf_token" value="([^"]+)', dpage) or [0, csrf])[1]
    uploads = re.findall(r'data-upload_id="(\d+)"', dpage)
    names = re.findall(r'class="upload_name[^>]*>.*?<strong[^>]*title="([^"]*)"', dpage, re.S)
    out = os.path.join(dest, f"{author}__{slug}")
    os.makedirs(out, exist_ok=True)
    files = []
    key = urllib.parse.parse_qs(urllib.parse.urlparse(dl).query).get("key", [""])[0]
    for i, uid in enumerate(uploads):
        u = f"{url}/file/{uid}?source=view_game&as_props=1&after_download_lightbox=true"
        rr = json.loads(req(op, u, {"csrf_token": csrf2}, {"X-Requested-With": "XMLHttpRequest", "Referer": dl}).read())
        cdn = rr["url"]
        resp = req(op, cdn)
        cd = resp.headers.get("Content-Disposition") or ""
        mm = re.search(r"filename\*?=(?:UTF-8'')?\"?([^\";]+)", cd)
        name = urllib.parse.unquote(mm.group(1)) if mm else (urllib.parse.unquote(os.path.basename(urllib.parse.urlparse(resp.geturl()).path)) or f"{uid}.bin")
        name = re.sub(r'[\/:*?"<>|]', "_", name)
        path = os.path.join(out, name)
        size = int(resp.headers.get("Content-Length") or 0)
        if size > max_mb * 1048576:
            print(f"  skip {name} ({size>>20} MB > {max_mb})")
            continue
        with open(path, "wb") as f:
            while chunk := resp.read(1 << 20):
                f.write(chunk)
        files.append({"name": name, "bytes": os.path.getsize(path)})
        print(f"  ok {name} {os.path.getsize(path)>>10} KB")
    json.dump({"url": url, "title": title, "author": author, "license_hint": lic, "files": files},
              open(os.path.join(out, "source.json"), "w", encoding="utf8"), ensure_ascii=False, indent=1)
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("urls", nargs="+")
    ap.add_argument("--dest", default="assets/downloaded/itch")
    ap.add_argument("--max-mb", type=int, default=600)
    a = ap.parse_args()
    for u in a.urls:
        print(u)
        try:
            fetch(u, a.dest, a.max_mb)
        except SystemExit as e:
            print("  FAIL", e)
        except Exception as e:
            print("  ERR", repr(e))
