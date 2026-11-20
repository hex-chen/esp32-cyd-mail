"""邮件提醒服务端。

轮询 Gmail IMAP -> 把最新邮件渲染成 1bit 点阵 -> HTTP 提供给 CYD 板子。
板子每 15 秒 GET /api/card?seen=<seq>，靠 X-New 判断是否亮屏。

启动: python3 mail_notify.py
配置: config.json（不进仓库，权限 600）；没有就按 config.example.json 的假数据模式跑。
"""

import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import card_render
import imap_source

HERE = os.path.dirname(os.path.abspath(__file__))
POLL_HINT = 15  # 告诉板子的轮询间隔（秒）

# OTA 固件托管目录
FW_DIR = os.path.join(HERE, "firmware")
FW_JSON = os.path.join(FW_DIR, "fw.json")
FW_BIN = os.path.join(FW_DIR, "firmware.bin")


def fw_info():
    """读取固件元信息；缺失时返回 version=0，板子不会触发升级。"""
    try:
        with open(FW_JSON, encoding="utf8") as f:
            d = json.load(f)
        d["available"] = os.path.exists(FW_BIN)
        return d
    except Exception:
        return {"version": 0, "available": False}

FAKE_MAILS = [
    {
        "sender": "GitHub",
        "subject": "[hex-chen/esp32-cyd-mail] PR #1 已合并，构建通过",
        "date": "11-14 10:52",
    },
    {
        "sender": "腾讯云",
        "subject": "您的云服务器月度账单已生成，本月用量正常",
        "date": "11-14 11:03",
    },
    {
        "sender": "快递通知",
        "subject": "您的包裹已到达丰巢柜，取件码 8842，请及时领取",
        "date": "11-14 11:20",
    },
]


def load_config():
    path = os.path.join(HERE, "config.json")
    if not os.path.exists(path):
        path = os.path.join(HERE, "config.example.json")
        print("[warn] 没有 config.json，使用示例配置（假数据模式）", flush=True)
    with open(path, encoding="utf8") as f:
        return json.load(f)


class State:
    def __init__(self, cfg):
        self.cfg = cfg
        self.fake_mode = bool(cfg.get("fake_mode", True))
        self.lock = threading.Lock()
        self.seq = 0
        self.unread = 0
        self.card = card_render.render_empty()
        self.last_mail_id = None
        self.last_error = ""
        self.last_check = 0
        self.fake_index = 0
        self.source = None
        if not self.fake_mode:
            self.source = imap_source.GmailSource(cfg)

    def publish(self, mail, unread):
        with self.lock:
            self.seq += 1
            self.unread = unread
            self.card = card_render.render_card(mail, unread)
            self.last_check = time.time()
            print(f"[new] seq={self.seq} 未读={unread} "
                  f"主题={mail.get('subject','')[:40]}", flush=True)

    def push_fake(self):
        mail = FAKE_MAILS[self.fake_index % len(FAKE_MAILS)]
        self.fake_index += 1
        self.publish(mail, self.fake_index)
        return mail

    def poll_loop(self):
        if self.fake_mode:
            print("[poll] 假数据模式：不连 IMAP，用 POST /api/fake 触发新邮件",
                  flush=True)
            return
        interval = int(self.cfg.get("imap_poll_seconds", 20))
        while True:
            try:
                info = self.source.check()
                self.last_check = time.time()
                self.last_error = ""
                mid = info["mail_id"]
                if self.last_mail_id is None:
                    # 首次只记录基线，不把历史邮件当成新邮件
                    self.last_mail_id = mid
                    with self.lock:
                        self.unread = info["unread"]
                        if info["subject"]:
                            self.card = card_render.render_card(
                                info, info["unread"]
                            )
                    print(f"[poll] 基线 mail_id={mid} 未读={info['unread']}",
                          flush=True)
                elif mid > self.last_mail_id:
                    self.last_mail_id = mid
                    self.publish(info, info["unread"])
                else:
                    with self.lock:
                        self.unread = info["unread"]
            except Exception as exc:
                self.last_error = f"{type(exc).__name__}: {exc}"
                print(f"[poll] 检测失败: {self.last_error}", flush=True)
            time.sleep(interval)

    def snapshot(self):
        with self.lock:
            return self.seq, self.unread, self.card


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("[http] " + (fmt % args), flush=True)

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        state = self.server.state
        if u.path == "/api/health":
            seq, unread, _ = state.snapshot()
            self._json({
                "ok": True,
                "fake_mode": state.fake_mode,
                "seq": seq,
                "unread": unread,
                "last_check": state.last_check,
                "last_error": state.last_error,
                "card_bytes": len(state.card),
            })
            return
        if u.path == "/api/card":
            seq, unread, card = state.snapshot()
            q = parse_qs(u.query)
            try:
                seen = int(q.get("seen", ["0"])[0])
            except ValueError:
                seen = 0
            # seen=0 是板子首次同步，不触发亮屏
            is_new = 1 if (seen > 0 and seq > seen) else 0
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(card)))
            self.send_header("X-Seq", str(seq))
            self.send_header("X-New", str(is_new))
            self.send_header("X-Unread", str(unread))
            self.send_header("X-Poll", str(POLL_HINT))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(card)
            return
        if u.path == "/api/fw":
            # OTA: 板子轮询时顺带查版本。用紧凑 JSON（冒号后无空格），
            # 兼容旧版固件的简易 JSON 解析器。
            info = fw_info()
            if info.get("available") and info.get("version", 0) > 0:
                body = ('{"version":%d,"size":%d,"md5":"%s"}' % (
                    int(info["version"]), int(info["size"]),
                    str(info["md5"]))).encode()
            else:
                body = b'{"version":0}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if u.path == "/api/fw.bin":
            # OTA: 下载固件二进制
            if not os.path.exists(FW_BIN):
                self._json({"error": "no firmware"}, 404)
                return
            size = os.path.getsize(FW_BIN)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            with open(FW_BIN, "rb") as f:
                while True:
                    chunk = f.read(8192)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return
        self._json({"error": "not found"}, 404)

    def do_POST(self):
        u = urlparse(self.path)
        state = self.server.state
        if u.path == "/api/fake":
            mail = state.push_fake()
            self._json({"ok": True, "subject": mail["subject"],
                        "seq": state.seq})
            return
        self._json({"error": "not found"}, 404)


def main():
    cfg = load_config()
    state = State(cfg)
    threading.Thread(target=state.poll_loop, daemon=True).start()
    port = int(cfg.get("http_port", 8087))
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.state = state
    srv.daemon_threads = True
    print(f"[ready] 监听 0.0.0.0:{port}  fake_mode={state.fake_mode}",
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
