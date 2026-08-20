"""生成按键接线图 PNG（面包板俯视图）。运行：
    python tools/draw_button_diagram.py
输出：docs/hardware/figures/button_wiring.png
"""

from PIL import Image, ImageDraw, ImageFont
import os

W, H = 1080, 720
img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)

FONT_PATH = None
for p in (
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "C:/Windows/Fonts/simsun.ttc",
):
    if os.path.exists(p):
        FONT_PATH = p
        break


def font(size):
    if FONT_PATH:
        return ImageFont.truetype(FONT_PATH, size)
    return ImageFont.load_default()


f_title = font(30)
f_sub = font(20)
f_label = font(17)
f_small = font(15)

# ===== 标题 =====
d.text((40, 25), "按键接线图（6×6×5 四脚轻触按键）", font=f_title, fill="black")
d.text((40, 68), "跨凹槽插入：上排两脚是一对、下排两脚是一对；按下时上下导通", font=f_sub, fill="#333333")

# ===== 电源轨（左侧）=====
d.rectangle((50, 150, 105, 320), fill="#f2d7d7", outline="#cc0000")
d.text((57, 130), "3V3", font=f_label, fill="#cc0000")
d.text((57, 323), "+ 轨", font=f_label, fill="#cc0000")
d.rectangle((125, 150, 180, 320), fill="#d7e3f2", outline="#0000cc")
d.text((132, 130), "GND", font=f_label, fill="#0000cc")
d.text((132, 323), "- 轨", font=f_label, fill="#0000cc")

# ===== 面包板中间区（行 19/20/21，列 a-j）=====
COL_W = 48
GROOVE = 26
ROW_H = 44
X0 = 230
Y0 = 170
ROWS = {19: Y0, 20: Y0 + ROW_H, 21: Y0 + 2 * ROW_H}

col_x = {}
for i in range(10):
    c = "abcdefghij"[i]
    x = X0 + i * COL_W + (GROOVE if i >= 5 else 0)
    col_x[c] = x

for row, y in ROWS.items():
    for c, x in col_x.items():
        d.rectangle((x, y, x + COL_W, y + ROW_H), fill="#fbfbf8", outline="#bbbbbb")
    d.text((X0 + 10 * COL_W + GROOVE + 12, y + 10), "第%d行" % row, font=f_label, fill="#555555")

# 凹槽
groove_x = col_x["e"] + COL_W
d.rectangle((groove_x, Y0 - 8, groove_x + GROOVE, Y0 + 3 * ROW_H + 8), fill="#eeeeee", outline="#999999")
d.text((groove_x + 3, Y0 - 30), "凹槽", font=f_small, fill="#777777")

# 列标
for c, x in col_x.items():
    d.text((x + 14, Y0 + 3 * ROW_H + 12), c, font=f_label, fill="#555555")

# ===== 按键（跨凹槽，行 20/21，列 e/f）=====
btn_x0 = col_x["e"] + 4
btn_x1 = col_x["f"] + COL_W - 4
btn_y0 = ROWS[20] + 3
btn_y1 = ROWS[21] + ROW_H - 3
d.rounded_rectangle((btn_x0, btn_y0, btn_x1, btn_y1), radius=8, fill="#dce9ff", outline="#224466")

pin = {}
pin["tl"] = (col_x["e"] + COL_W // 2, ROWS[20] + ROW_H // 2)
pin["tr"] = (col_x["f"] + COL_W // 2, ROWS[20] + ROW_H // 2)
pin["bl"] = (col_x["e"] + COL_W // 2, ROWS[21] + ROW_H // 2)
pin["br"] = (col_x["f"] + COL_W // 2, ROWS[21] + ROW_H // 2)
for name, (px, py) in pin.items():
    d.ellipse((px - 7, py - 7, px + 7, py + 7), fill="#ffd24d", outline="#aa8800")

# 同一对脚：上排（行20）内部常通、下排（行21）内部常通；按下=上下导通
d.line((pin["tl"][0] + 12, pin["tl"][1], pin["tr"][0] - 12, pin["tr"][1]), fill="#224466", width=2)
d.line((pin["bl"][0] + 12, pin["bl"][1], pin["br"][0] - 12, pin["br"][1]), fill="#224466", width=2)
d.text((pin["tl"][0] + 22, pin["tl"][1] - 20), "同一对脚", font=f_small, fill="#224466")
d.text((pin["bl"][0] + 22, pin["bl"][1] - 20), "同一对脚", font=f_small, fill="#224466")
d.text((btn_x0 + 12, btn_y0 + 6), "按键", font=f_label, fill="#224466")

# ===== 电阻 10k（行19到行20，列 e）=====
res_x = col_x["e"] + COL_W // 2
d.rectangle((res_x - 9, ROWS[19] + 12, res_x + 9, ROWS[20] + 32), fill="#ffd9a0", outline="#e8800c")
d.text((res_x + 14, ROWS[19] + 12), "10kΩ", font=f_label, fill="#e8800c")

# ===== 导线 =====
# 蓝：IO23 到 行20 列d（与行20 列e 同节点）
io_x = col_x["d"] + COL_W // 2
d.line((io_x, ROWS[20] + ROW_H // 2, io_x, 96), fill="#0066cc", width=4)
d.text((io_x - 90, 66), "蓝线 接开发板 IO14", font=f_label, fill="#0066cc")

# 黑：GND 到 行21 列g（与行21 列f 同节点），再到 - 轨
gnd_x = col_x["g"] + COL_W // 2
d.line((gnd_x, ROWS[21] + ROW_H // 2, gnd_x, 150), fill="#111111", width=4)
d.line((gnd_x, 150, 152, 150), fill="#111111", width=4)
d.line((152, 150, 152, ROWS[21] + ROW_H // 2), fill="#111111", width=4)
d.text((gnd_x + 10, 124), "黑线 到 - 轨(GND)", font=f_label, fill="#111111")
d.text((gnd_x + 10, 154), "接行21列g", font=f_small, fill="#111111")

# 红：3V3 从电阻上端（行19 列e）向上、再向左到 + 轨
res_top = ROWS[19] + 12
d.line((res_x, res_top, res_x, 130), fill="#dd0000", width=4)
d.line((res_x, 130, 78, 130), fill="#dd0000", width=4)
d.line((78, 130, 78, 174), fill="#dd0000", width=4)
d.text((230, 100), "红线 到 + 轨(3V3)", font=f_label, fill="#dd0000")

# 端点圆点
d.ellipse((io_x - 7, ROWS[20] + ROW_H // 2 - 7, io_x + 7, ROWS[20] + ROW_H // 2 + 7), fill="#0066cc")
d.ellipse((gnd_x - 7, ROWS[20] + ROW_H // 2 - 7, gnd_x + 7, ROWS[20] + ROW_H // 2 + 7), fill="#111111")

# ===== 图例 =====
ly = 430
d.rectangle((40, ly - 10, 1020, ly + 150), fill="#fafafa", outline="#cccccc")
d.text((55, ly), "怎么看这张图：", font=f_sub, fill="black")
items = [
    ("蓝线", "#0066cc", "接到开发板 IO14 引脚（母对母+公对公组合）"),
    ("黑线", "#111111", "接到面包板 - 轨（GND）"),
    ("红线", "#dd0000", "接到面包板 + 轨（3V3）"),
    ("橙色块", "#e8800c", "10kΩ 电阻：一脚插第19行(接3V3)，另一脚插第20行(接IO14)"),
]
for i, (name, colr, desc) in enumerate(items):
    yy = ly + 34 + i * 26
    d.rectangle((55, yy - 6, 90, yy + 10), fill=colr)
    d.text((100, yy - 8), "%s：%s" % (name, desc), font=f_label, fill="black")

ly2 = ly + 150 + 14
d.text((55, ly2), "结果：松开=读到 1（高电平）；按下=读到 0（低电平）", font=f_sub, fill="#224466")

out = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "docs", "hardware", "figures", "button_wiring.png",
)
os.makedirs(os.path.dirname(out), exist_ok=True)
img.save(out)
print("saved:", out)
