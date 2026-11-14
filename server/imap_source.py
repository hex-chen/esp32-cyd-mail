"""Gmail IMAP 收件源。

服务器在国内，直连 imap.gmail.com 不通，走本机代理的 HTTP CONNECT 隧道。
Python 3.14 的 imaplib 把 internal socket 创建收进了 _create_socket()，
覆盖它即可复用标准库的 IMAP 状态机，不用自己实现 IMAP 协议。
只读：select(readonly=True) + BODY.PEEK，不把邮件标成已读。
"""

import email
import imaplib
import socket
import ssl
from email.header import decode_header, make_header
from email.utils import parseaddr, parsedate_to_datetime


def _decode(value: str) -> str:
    if not value:
        return ""
    try:
        return str(make_header(decode_header(value))).strip()
    except Exception:
        return value.strip()


class _ProxiedIMAP(imaplib.IMAP4):
    def __init__(self, host, port, proxy, timeout=15):
        self._proxy = (proxy["host"], proxy["port"])
        self._peer = (host, port)
        self._deadline = timeout
        super().__init__(host, port, timeout)

    def _create_socket(self, timeout=None):
        sock = socket.create_connection(
            self._proxy, timeout=timeout or self._deadline
        )
        req = (
            f"CONNECT {self._peer[0]}:{self._peer[1]} HTTP/1.1\r\n"
            f"Host: {self._peer[0]}:{self._peer[1]}\r\n\r\n"
        ).encode()
        sock.sendall(req)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        if b" 200 " not in buf.split(b"\r\n", 1)[0]:
            sock.close()
            raise RuntimeError("proxy CONNECT 失败")
        ctx = ssl.create_default_context()
        return ctx.wrap_socket(sock, server_hostname=self._peer[0])


class GmailSource:
    def __init__(self, cfg):
        self.host = cfg.get("imap_host", "imap.gmail.com")
        self.port = int(cfg.get("imap_port", 993))
        self.user = cfg["user"]
        self.password = cfg["app_password"]
        p = cfg.get("proxy", "127.0.0.1:7890").replace("http://", "").rstrip("/")
        ph, _, pp = p.partition(":")
        self.proxy = {"host": ph or "127.0.0.1", "port": int(pp or 7890)}

    def check(self):
        """返回最新一封邮件的摘要，或 None（收件箱为空）。"""
        m = _ProxiedIMAP(self.host, self.port, self.proxy)
        try:
            m.login(self.user, self.password)
            status, data = m.select("INBOX", readonly=True)
            if status != "OK":
                raise RuntimeError("INBOX select 失败")

            _, data = m.search(None, "ALL")
            ids = data[0].split()
            if not ids:
                return {"mail_id": 0, "unread": 0, "subject": "",
                        "sender": "", "date": ""}
            last = max(int(i) for i in ids)

            _, data = m.search(None, "UNSEEN")
            unread = len(data[0].split())

            _, raw = m.fetch(
                str(last), "(BODY.PEEK[HEADER.FIELDS (SUBJECT FROM DATE)])"
            )
            msg = email.message_from_bytes(raw[0][1])
            name, addr = parseaddr(_decode(msg.get("From", "")))
            try:
                date = parsedate_to_datetime(msg.get("Date", "")).strftime(
                    "%m-%d %H:%M"
                )
            except Exception:
                date = ""
            return {
                "mail_id": last,
                "unread": unread,
                "subject": _decode(msg.get("Subject", "")) or "(无主题)",
                "sender": name or addr or "未知发件人",
                "date": date,
            }
        finally:
            try:
                m.logout()
            except Exception:
                pass
