#!/usr/bin/env python3
"""按固件坐标渲染定版图（640×384）。

文字用系统字体近似（固件里中文是 U8g2 的 WQY 点阵、数字是烘出的 1bit 字模），
但每一处坐标、线宽、行高都与 epaper_week.ino 保持一致，可用于版式核对与文档配图。

用法: python3 tools/render_final.py [输出路径]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 640, 384
MARGIN = 10
RULE_Y = 76
ROW_TOP, ROW_H = 76, 43
COL_DIV_X, EV_X, EV_GAP = 56, 64, 14
WX_DIV_X, WX_X = 557, 566
LINE_W = WX_DIV_X - 14 - EV_X
MINI_X, MINI_Y, MINI_COL_W, MINI_ROW_H = 327, 8, 18, 10

BLACK, RED, WHITE = (0, 0, 0), (190, 30, 40), (255, 255, 255)
FONT_PATH = "/System/Library/Fonts/Hiragino Sans GB.ttc"

def f(size, bold=False):
    return ImageFont.truetype(FONT_PATH, size, index=1 if bold else 0)

F13B, F13, F12B, F12, F16B, F18B, F64, F8 = (
    f(13, True), f(13), f(12, True), f(12), f(16, True), f(18, True), f(64, True), f(8))

# 示例数据，全部为占位内容（不要放真实日程，这张图会进仓库）
DAYS = [
    # 周几, 日期, 天气, 低/高, 事件
    ("周一", "11/16", "晴", "12/20", ["09:30-10:30 项目周会", "14:00-15:00 需求评审"]),
    ("周二", "11/17", "多云", "10/17", ["10:00-11:00 客户沟通", "16:00-17:00 方案讨论"]),
    ("周三", "11/18", "阴", "9/16", ["11:00-12:00 设计评审", "13:30-14:30 技术分享",
                                    "16:00-17:00 方案讨论"]),
    ("周四", "11/19", "晴", "11/18", ["10:30-11:15 一对一沟通", "13:30-15:00 内部培训",
                                     "16:00-17:00 方案讨论"]),
    ("周五", "11/20", "", "", ["10:30-11:00 周报整理", "16:00-17:00 方案讨论"]),
    ("周六", "11/21", "", "", []),
    ("周日", "11/22", "", "", []),
]
TODAY = 3
UPDATED = "更新 11-19 21:43 · 天气 21:24"
BAT_PCT = 100

img = Image.new("RGB", (W, H), WHITE)
d = ImageDraw.Draw(img)


def text(x, base, s, font, color=BLACK, bold=False):
    d.text((x, base), s, font=font, fill=color, anchor="ls")
    if bold:
        d.text((x + 1, base), s, font=font, fill=color, anchor="ls")


def icon(kind, x, y):
    if kind == "晴":
        d.ellipse([x + 4, y + 4, x + 10, y + 10], fill=RED)
        for dx, dy in ((7, 0), (7, 14), (0, 7), (14, 7), (2, 2), (12, 2), (2, 12), (12, 12)):
            d.line([x + 7, y + 7, x + dx, y + dy], fill=RED)
    elif kind == "雨":
        d.ellipse([x + 1, y + 3, x + 7, y + 9], outline=BLACK)
        d.ellipse([x + 6, y + 2, x + 12, y + 8], outline=BLACK)
        d.line([x + 2, y + 10, x + 12, y + 10], fill=BLACK)
        for i in range(3):
            d.line([x + 3 + i * 4, y + 11, x + 2 + i * 4, y + 14], fill=BLACK)
    else:
        d.ellipse([x + 1, y + 6, x + 7, y + 12], outline=BLACK)
        d.ellipse([x + 6, y + 4, x + 12, y + 11], outline=BLACK)
        d.line([x + 2, y + 13, x + 12, y + 13], fill=BLACK)


# ---- 表头
num_w = d.textlength("11", font=F64)
d.text((14, 60), "11", font=F64, fill=RED, anchor="ls")
info_x = 14 + num_w + 12
text(info_x, 34, "NOV", F18B, RED, bold=True)
mon_w = d.textlength("NOV", font=F18B)
text(info_x + mon_w + 14, 34, "第 47 周", F16B)
text(info_x, 58, UPDATED, F12)

# ---- 电量条
if BAT_PCT >= 100:
    d.rectangle([MARGIN, 72, MARGIN + 620 - 1, 75], fill=BLACK)
else:
    solid = 620 * BAT_PCT // 100
    d.rectangle([MARGIN, 72, MARGIN + solid - 1, 75], fill=BLACK)
    d.rectangle([MARGIN + solid, 72, MARGIN + 619, 75], fill=WHITE, outline=BLACK)

# ---- 小月历
def month(ndays, first_wd, label, x0):
    d.rectangle([x0 - 4, MINI_Y - 3, x0 - 4 + 132, MINI_Y - 3 + 64], outline=BLACK)
    d.text((x0 - 32, MINI_Y + 7), label, font=F12, fill=BLACK, anchor="ls")
    col, row = first_wd, 0
    for day in range(1, ndays + 1):
        cx = x0 + col * MINI_COL_W + MINI_COL_W // 2
        cy = MINI_Y + 1 + row * MINI_ROW_H
        s = str(day)
        wpx = d.textlength(s, font=F8)
        d.text((cx - wpx / 2, cy + 8), s, font=F8, fill=BLACK, anchor="ls")
        col += 1
        if col == 7:
            col, row = 0, row + 1


month(30, 6, "11月", MINI_X)
month(31, 1, "12月", MINI_X + 7 * MINI_COL_W + 36 + 12)

# ---- 7 行
for i, (wd, date, cond, temp, events) in enumerate(DAYS):
    y = ROW_TOP + i * ROW_H
    today = (i == TODAY)
    if not today and i:
        for x in range(MARGIN, W - MARGIN, 8):
            d.line([x, y, x + 1, y], fill=BLACK)

    col = RED if today else BLACK
    text(18, y + 19, wd, F13B, col, bold=True)
    text(18, y + 36, date, F12B, col, bold=True)
    d.line([COL_DIV_X, y + 8, COL_DIV_X, y + 35], fill=BLACK)

    # 事件贪心装两行
    if events:
        placed, used, lines = [], [0, 0], [0, 0]
        for s in events:
            wpx = d.textlength(s, font=F12)
            for ln in range(2):
                need = used[ln] + (EV_GAP if lines[ln] else 0) + wpx
                if need <= LINE_W:
                    placed.append((ln, s, wpx))
                    used[ln] = need
                    lines[ln] += 1
                    break
        n_lines = 2 if (lines[0] and lines[1]) else 1
        top = y + 6 if n_lines > 1 else y + 15
        for ln in (0, 1):
            if not lines[ln]:
                continue
            ly = top + ln * 18
            x = EV_X
            first = True
            for pln, s, wpx in placed:
                if pln != ln:
                    continue
                if not first:
                    d.line([x - EV_GAP // 2, ly - 1, x - EV_GAP // 2, ly + 16], fill=BLACK)
                d.text((x, ly + 12), s, font=F12, fill=BLACK, anchor="ls")
                x += wpx + EV_GAP
                first = False
    else:
        d.text((EV_X + 2, y + 27), "--", font=F12, fill=BLACK, anchor="ls")

    d.line([WX_DIV_X, y + 8, WX_DIV_X, y + 35], fill=BLACK)
    if cond:
        icon(cond, WX_X, y + 7)
        d.text((WX_X + 18, y + 19), cond, font=F12, fill=BLACK, anchor="ls")
        d.text((WX_X + 18, y + 36), temp + "°", font=F12, fill=BLACK, anchor="ls")
    else:
        d.text((WX_X + 6, y + 27), "--", font=F12, fill=BLACK, anchor="ls")

# ---- 今天红框（最后画，压在虚线之上）
ty = ROW_TOP + TODAY * ROW_H
d.rounded_rectangle([4, ty, 4 + 631, ty + 43], radius=10, outline=RED)
d.rounded_rectangle([5, ty + 1, 5 + 629, ty + 42], radius=10, outline=RED)

out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).parent.parent / "preview" / "week_view_final.png"
out.parent.mkdir(parents=True, exist_ok=True)
img.save(out)
print(f"已生成 {out}  {img.size[0]}x{img.size[1]}")
