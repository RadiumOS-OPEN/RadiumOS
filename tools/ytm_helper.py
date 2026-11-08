#!/usr/bin/env python3
"""YouTube Music helper for RadiumOS. Run it on the host while the `ytm` command runs in the guest.

  tools/ytm_helper.py [--port 8787] [--cache ~/.cache/radiumos-ytm] [--ytdlp PATH]
                      [--cookies cookies.txt | --cookies-from-browser firefox]

The guest reaches it at http://10.0.2.2:8787 (QEMU user networking). It only listens on 127.0.0.1.

  GET /ping
  GET /search?q=TEXT[&n=20]   -> lines "videoId|seconds|artist|title"
  GET /audio?id=VIDEOID       -> the track as an AAC .m4a (downloaded with yt-dlp, cached)
  GET /video?id=VIDEOID[&q=240] -> a small H.264 + AAC .mp4 (144 to 480 lines; needs ffmpeg to merge the streams)
  GET /info?id=VIDEOID        -> one line "videoId|seconds|artist|title"

Why a helper: YouTube asks clients that are not a real browser to prove they are not a bot before it hands out
audio URLs, and works that out in JavaScript. yt-dlp keeps up with that; RadiumOS cannot. Keep yt-dlp current
(pip install -U yt-dlp) - an old one fails with "Sign in to confirm you're not a bot" or signature errors.
Cookies from a browser you are logged into (--cookies-from-browser) also unlock age-restricted and Premium tracks.
"""
import argparse, glob, http.server, json, os, re, shutil, socketserver, subprocess, sys, threading, urllib.parse, urllib.request

ID_RE = re.compile(r"^[A-Za-z0-9_-]{11}$")
SONGS_FILTER = "EgWKAQIIAWoKEAkQBRAKEAMQBA%3D%3D"   # YouTube Music search "Songs" tab
KEEP_FILES = 40
locks = {}
locks_guard = threading.Lock()
opts = None


def clean(text):
    return re.sub(r"[|\r\n]+", " ", text or "").strip()


def runs_text(cell):
    return [r.get("text", "") for r in (cell or {}).get("musicResponsiveListItemFlexColumnRenderer", {}).get("text", {}).get("runs", [])]


def to_seconds(text):
    parts = text.split(":")
    if not all(p.isdigit() for p in parts) or not 2 <= len(parts) <= 3:
        return None
    total = 0
    for p in parts:
        total = total * 60 + int(p)
    return total


def walk(node, found):
    if isinstance(node, dict):
        if "musicResponsiveListItemRenderer" in node:
            found.append(node["musicResponsiveListItemRenderer"])
        else:
            for v in node.values():
                walk(v, found)
    elif isinstance(node, list):
        for v in node:
            walk(v, found)


def search_music(query, count):
    body = json.dumps({
        "context": {"client": {"clientName": "WEB_REMIX", "clientVersion": "1.20240918.01.00", "hl": "en"}},
        "query": query, "params": urllib.parse.unquote(SONGS_FILTER),
    }).encode()
    req = urllib.request.Request("https://music.youtube.com/youtubei/v1/search?prettyPrint=false", body,
                                 {"Content-Type": "application/json", "Origin": "https://music.youtube.com"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        data = json.load(resp)
    items = []
    walk(data, items)
    rows, seen = [], set()
    for item in items:
        vid = item.get("playlistItemData", {}).get("videoId")
        if not vid or vid in seen or not ID_RE.match(vid):
            continue
        cols = item.get("flexColumns", [])
        title = clean("".join(runs_text(cols[0]))) if cols else ""
        info = runs_text(cols[1]) if len(cols) > 1 else []
        seconds = next((to_seconds(t) for t in reversed(info) if to_seconds(t) is not None), 0)
        parts = [t for t in info if t.strip() not in ("•", "") and to_seconds(t) is None]
        artist = clean(parts[1] if parts and parts[0] in ("Song", "Video") and len(parts) > 1 else (parts[0] if parts else ""))
        if title:
            seen.add(vid)
            rows.append("%s|%d|%s|%s" % (vid, seconds or 0, artist, title))
        if len(rows) >= count:
            break
    return rows


def search_ytdlp(query, count):
    out = subprocess.run([opts.ytdlp, "--flat-playlist", "--no-warnings", "--print", "%(id)s|%(duration)s|%(channel)s|%(title)s",
                          "ytsearch%d:%s" % (count, query)] + opts.cookie_args, capture_output=True, text=True, timeout=60).stdout
    rows = []
    for line in out.splitlines():
        parts = line.split("|", 3)
        if len(parts) == 4 and ID_RE.match(parts[0]):
            rows.append("|".join([parts[0], parts[1] if parts[1].isdigit() else "0", clean(parts[2]).replace("NA", ""), clean(parts[3])]))
    return rows


def fetch_video(video_id, height):
    """Path of the cached H.264/AAC .mp4 (moov atom first, so the guest can start at once). Raises RuntimeError."""
    os.makedirs(opts.cache, exist_ok=True)
    path = os.path.join(opts.cache, "%s_%d.mp4" % (video_id, height))
    key = video_id + "/%d" % height
    with locks_guard:
        lock = locks.setdefault(key, threading.Lock())
    with lock:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path
        fmt = ("bv*[height<=%d][vcodec^=avc1][ext=mp4]+ba[ext=m4a]/b[height<=%d][vcodec^=avc1][ext=mp4]" % (height, height))
        cmd = [opts.ytdlp, "--no-playlist", "--no-warnings", "--no-part", "-f", fmt, "-S", "res,+size",
               "--merge-output-format", "mp4", "--postprocessor-args", "Merger+ffmpeg:-movflags +faststart",
               "-o", os.path.join(opts.cache, "%(id)s_" + str(height) + ".%(ext)s"),
               "https://www.youtube.com/watch?v=" + video_id] + opts.cookie_args
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        if proc.returncode != 0 or not os.path.exists(path):
            raise RuntimeError((proc.stderr.strip().splitlines() or ["yt-dlp failed"])[-1][:200])
    files = sorted(glob.glob(os.path.join(opts.cache, "*.mp4")), key=os.path.getmtime)
    for old in files[:-6]:
        try:
            os.remove(old)
        except OSError:
            pass
    return path


def video_info(video_id):
    out = subprocess.run([opts.ytdlp, "--no-playlist", "--no-warnings", "--skip-download", "--print",
                          "%(id)s|%(duration)s|%(channel)s|%(title)s", "https://www.youtube.com/watch?v=" + video_id]
                         + opts.cookie_args, capture_output=True, text=True, timeout=60).stdout.strip().splitlines()
    for line in out:
        parts = line.split("|", 3)
        if len(parts) == 4 and ID_RE.match(parts[0]):
            return "|".join([parts[0], parts[1] if parts[1].isdigit() else "0", clean(parts[2]).replace("NA", ""), clean(parts[3])])
    raise RuntimeError("no information for that video")


def fetch_audio(video_id):
    """Path of the cached AAC .m4a for the video, downloading it first when needed. Raises RuntimeError."""
    os.makedirs(opts.cache, exist_ok=True)
    path = os.path.join(opts.cache, video_id + ".m4a")
    with locks_guard:
        lock = locks.setdefault(video_id, threading.Lock())
    with lock:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path
        cmd = [opts.ytdlp, "--no-playlist", "--no-warnings", "--no-part", "-f", "140/bestaudio[acodec^=mp4a]",
               "-o", os.path.join(opts.cache, "%(id)s.%(ext)s"), "https://music.youtube.com/watch?v=" + video_id] + opts.cookie_args
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if proc.returncode != 0 or not os.path.exists(path):
            raise RuntimeError((proc.stderr.strip().splitlines() or ["yt-dlp failed"])[-1][:200])
    files = sorted(glob.glob(os.path.join(opts.cache, "*.m4a")), key=os.path.getmtime)
    for old in files[:-KEEP_FILES]:
        try:
            os.remove(old)
        except OSError:
            pass
    return path


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[ytm] %s\n" % (fmt % args))

    def reply(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_HEAD(self):
        self.do_GET(head_only=True)

    def send_audio(self, video_id, head_only):
        self.send_file(fetch_audio(video_id), "audio/mp4", head_only)

    def send_file(self, path, ctype, head_only):
        size = os.path.getsize(path)
        start, end, code = 0, size - 1, 200
        match = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range", ""))
        if match and (match.group(1) or match.group(2)):
            if match.group(1):
                start = int(match.group(1))
                end = min(int(match.group(2)), size - 1) if match.group(2) else size - 1
            else:
                start = max(size - int(match.group(2)), 0)
            if start > end or start >= size:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            code = 206
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if code == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        if head_only:
            return
        with open(path, "rb") as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(64 * 1024, left))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def do_GET(self, head_only=False):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        try:
            if url.path == "/ping":
                return self.reply(200, "ytm-helper 1\n")
            if url.path == "/search":
                text = (q.get("q") or [""])[0].strip()[:200]
                count = max(1, min(int((q.get("n") or ["20"])[0]), 30))
                if not text:
                    return self.reply(400, "missing q\n")
                try:
                    rows = search_music(text, count)
                except Exception as e:
                    sys.stderr.write("[ytm] music search failed (%s), using yt-dlp\n" % e)
                    rows = []
                if not rows:
                    rows = search_ytdlp(text, count)
                return self.reply(200, "\n".join(rows) + "\n")
            if url.path == "/audio":
                vid = (q.get("id") or [""])[0]
                if not ID_RE.match(vid):
                    return self.reply(400, "bad id\n")
                return self.send_audio(vid, head_only)
            if url.path == "/video":
                vid = (q.get("id") or [""])[0]
                if not ID_RE.match(vid):
                    return self.reply(400, "bad id\n")
                height = int((q.get("q") or ["240"])[0])
                height = min((144, 240, 360, 480), key=lambda h: abs(h - height))
                return self.send_file(fetch_video(vid, height), "video/mp4", head_only)
            if url.path == "/info":
                vid = (q.get("id") or [""])[0]
                if not ID_RE.match(vid):
                    return self.reply(400, "bad id\n")
                return self.reply(200, video_info(vid) + "\n")
            self.reply(404, "not found\n")
        except RuntimeError as e:
            self.reply(502, "yt-dlp: %s\n" % e)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            self.reply(500, "error: %s\n" % e)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def main():
    global opts
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/radiumos-ytm"))
    ap.add_argument("--ytdlp", default=os.environ.get("YTM_YTDLP"))
    ap.add_argument("--cookies")
    ap.add_argument("--cookies-from-browser")
    opts = ap.parse_args()
    opts.ytdlp = opts.ytdlp or next((p for p in (os.path.join(here, ".venv", "bin", "yt-dlp"), shutil.which("yt-dlp")) if p and os.path.exists(p)), None)
    if not opts.ytdlp:
        sys.exit("yt-dlp not found: pip install -U yt-dlp (or pass --ytdlp PATH)")
    opts.cookie_args = (["--cookies", opts.cookies] if opts.cookies else []) + \
                       (["--cookies-from-browser", opts.cookies_from_browser] if opts.cookies_from_browser else [])
    version = subprocess.run([opts.ytdlp, "--version"], capture_output=True, text=True).stdout.strip()
    print("yt-dlp %s at %s" % (version, opts.ytdlp))
    print("ytm helper on http://127.0.0.1:%d  (guest: http://10.0.2.2:%d), cache %s" % (opts.port, opts.port, opts.cache))
    Server(("127.0.0.1", opts.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
