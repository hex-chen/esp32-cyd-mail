"""把邮件渲染成 240x160 的 1bit 点阵。

格式：MSB first（bit7 = 最左像素）、行优先、每行 30 字节、共 4800 字节。
1 = 前景（板子画白），0 = 背景（板子不画）。
中文由服务器上的 Noto Sans CJK 渲染，板子不需要任何字库。
"""

from PIL import Image, ImageDraw, ImageFont

W, H = 240, 160
FONT_PATH = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
FONT_INDEX = 2  # TTC 内简体中文字形

_fcache = {}


def _font(size):
    if size not in _fcache:
        _fcache[size] = ImageFont.truetype(FONT_PATH, size, index=FONT_INDEX)
    return _fcache[size]


def _clip(text, font, max_w):
    """超宽就截掉尾部加省略号，按字符逐个量宽度（中文等宽近似不可靠）。"""
    if not text:
        return ""
    d = ImageDraw.Draw(Image.new("1", (1, 1)))
    if d.textlength(text, font=font) <= max_w:
        return text
    out = ""
    for ch in text:
        if d.textlength(out + ch + "…", font=font) > max_w:
            break
        out += ch
    return out + "…"


def _wrap(text, font, max_w, max_lines):
    """按字符折行，中文没有空格可断。"""
    d = ImageDraw.Draw(Image.new("1", (1, 1)))
    lines, cur = [], ""
    for ch in text or "":
        if ch == "\n":
            lines.append(cur)
            cur = ""
            continue
        if d.textlength(cur + ch, font=font) > max_w:
            lines.append(cur)
            cur = ch
        else:
            cur += ch
        if len(lines) >= max_lines:
            break
    if cur and len(lines) < max_lines:
        lines.append(cur)
    if len(lines) == max_lines and d.textlength(lines[-1], font=font) > max_w:
        lines[-1] = _clip(lines[-1], font, max_w)
    return lines[:max_lines]


def render_card(mail, unread=0, note="BOOT 查看 · 10 秒后熄屏"):
    img = Image.new("1", (W, H), 0)
    d = ImageDraw.Draw(img)

    # 顶栏反白
    d.rectangle((0, 0, W - 1, 26), fill=1)
    d.text((8, 3), "新邮件 NEW MAIL", font=_font(16), fill=0)
    badge = f"{unread} 封未读"
    bw = d.textlength(badge, font=_font(12))
    d.text((W - 8 - bw, 6), badge, font=_font(12), fill=0)

    # 发件人
    d.text((8, 34), "发件人", font=_font(12), fill=1)
    d.text((8, 50), _clip(mail.get("sender", ""), _font(16), W - 16),
           font=_font(16), fill=1)

    # 主题，两行
    d.text((8, 74), "主题", font=_font(12), fill=1)
    lines = _wrap(mail.get("subject", ""), _font(17), W - 16, 2)
    for i, line in enumerate(lines):
        d.text((8, 90 + i * 21), _clip(line, _font(17), W - 16),
               font=_font(17), fill=1)

    # 时间 + 提示
    d.line((8, 136, W - 9, 136), fill=1, width=1)
    left = mail.get("date", "") or ""
    d.text((8, 141), left, font=_font(12), fill=1)
    nw = d.textlength(note, font=_font(11))
    d.text((W - 8 - nw, 142), note, font=_font(11), fill=1)

    data = img.tobytes()
    assert len(data) == W * H // 8, f"位图尺寸异常: {len(data)}"
    return data


def render_empty(note="等待新邮件 · BOOT 查看"):
    return render_card(
        {"sender": "—", "subject": "暂无邮件", "date": ""}, 0, note
    )
