#!/usr/bin/env python3
"""Render the Cornix keymap as a draw.io diagram and an SVG.

Reads config/<keyboard>.keymap and boards/jzf/cornix/cornix-layouts.dtsi
(physical key positions, including thumb-key rotation) and writes

    draw/<keyboard>_keymap.drawio.xml   editable in draw.io / diagrams.net
    draw/<keyboard>_keymap.svg          embedded in README.md / README_zh.md

Only the Python standard library is used. Re-run after every keymap change:

    python3 draw/gen_keymap.py

CI (keymap-draw job in build.yml) fails if the committed outputs are stale.
"""
import html
import math
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEYBOARD = sys.argv[1] if len(sys.argv) > 1 else "cornix"
KEYMAP = ROOT / "config" / f"{KEYBOARD}.keymap"
LAYOUT = ROOT / "boards/jzf/cornix/cornix-layouts.dtsi"
OUT_DRAWIO = ROOT / "draw" / f"{KEYBOARD}_keymap.drawio.xml"
OUT_SVG = ROOT / "draw" / f"{KEYBOARD}_keymap.svg"

# ---------------------------------------------------------------- geometry --
S = 0.4  # physical-layout units (100 = 1u) -> px
KEY = 40
X0, Y0 = 130, 80
BLOCK_PITCH = 370
LEFT_HALF_MAX_X = 700  # physical x; keys left of this belong to the left half

# ------------------------------------------------------------------ colours --
KEYFILL, KEYSTROKE, KEYFONT = "#383E47", "#1e2228", "#AFB9C7"
MODFILL, MODSTROKE, MODFONT = "#ffb86c", "#b07a3a", "#1e1e1e"
BTFILL, BTSTROKE = "#5C8CCA", "#2f5f9e"
BOOTFILL, BOOTSTROKE = "#c0392b", "#7b1d12"
LAYER_COLORS = [  # (fill, stroke, font) by layer index
    ("#383E47", "#1e2228", KEYFONT),
    ("#1ba1e2", "#006EAF", "#ffffff"),
    ("#d80073", "#A50040", "#ffffff"),
    ("#a20025", "#6F0000", "#ffffff"),
    ("#60a917", "#2D7600", "#ffffff"),
    ("#6a00ff", "#3700CC", "#ffffff"),
]

# ------------------------------------------------------------------- labels --
MOD_SYM = {
    "LALT": "⌥", "RALT": "⌥", "LSHIFT": "⇧", "RSHIFT": "⇧", "LSHFT": "⇧", "RSHFT": "⇧",
    "LCTRL": "⌃", "RCTRL": "⌃", "LGUI": "⌘", "RGUI": "⌘", "LCMD": "⌘", "RCMD": "⌘",
    "HYPER": "✦",
}
MOD_NAME = {"⌥": "opt", "⇧": "shift", "⌃": "ctrl", "⌘": "cmd", "✦": "hyper"}
MOD_FN = {"LA": "⌥", "RA": "⌥", "LS": "⇧", "RS": "⇧", "LC": "⌃", "RC": "⌃", "LG": "⌘", "RG": "⌘"}
KEY_LABEL = {
    "ESC": "Esc", "BSPC": "⌫", "DEL": "⌦", "TAB": "⇥", "ENTER": "↵", "RET": "↵", "SPACE": "␣",
    "CAPS": "⇪", "UP": "↑", "DOWN": "↓", "LEFT": "←", "RIGHT": "→",
    "PG_UP": "PgUp", "PG_DN": "PgDn", "HOME": "Home", "END": "End",
    "SEMI": ";", "COMMA": ",", "DOT": ".", "FSLH": "/", "BSLH": "\\", "GRAVE": "`", "TILDE": "~",
    "EXCL": "!", "AT": "@", "HASH": "#", "DLLR": "$", "PRCNT": "%", "CARET": "^", "AMPS": "&",
    "ASTRK": "*", "LPAR": "(", "RPAR": ")", "LBRC": "{", "RBRC": "}", "LBKT": "[", "RBKT": "]",
    "MINUS": "-", "UNDER": "_", "EQUAL": "=", "PLUS": "+", "PIPE": "|", "COLON": ":",
    "SQT": "'", "DQT": "\"", "QMARK": "?", "APOS": "'",
    "C_VOL_UP": "Vol+", "C_VOL_DN": "Vol−", "C_BRI_UP": "Bri+", "C_BRI_DN": "Bri−",
    "C_PP": "⏯", "C_NEXT": "⏭", "C_PREV": "⏮", "C_MUTE": "Mute",
    "KP_MINUS": "−", "KP_PLUS": "+", "KP_ENTER": "↵", "KP_EQUAL": "=", "KP_ASTERISK": "*", "KP_SLASH": "/",
    "PSCRN": "PrtSc", "LOCK": "lock",
}
KEY_SUB = {"C_PP": "play", "C_NEXT": "next", "C_PREV": "prev"}
for _n in range(10):
    KEY_LABEL[f"N{_n}"] = str(_n)
    KEY_LABEL[f"KP_N{_n}"] = str(_n)
    KEY_SUB[f"KP_N{_n}"] = "kp"
for _k in ("KP_MINUS", "KP_PLUS", "KP_ENTER", "KP_EQUAL", "KP_ASTERISK", "KP_SLASH"):
    KEY_SUB[_k] = "kp"
for _f in range(1, 25):
    KEY_LABEL[f"F{_f}"] = f"F{_f}"

# --------------------------------------------------------------------- notes --
# Free-form annotations drawn next to the Base block. Edit to taste; the
# combo note is generated from the keymap's combos block.
NOTES = [
    dict(lines=["Home-row mods", "按住 A/S/D/F = ⌥ ⇧ ⌃ ⌘", "J/K/L/; 镜像 (balanced, 200 ms)"],
         x=X0 - 10, y=Y0 + 235, w=190, h=50, color="#FF8000", align="left", arrows=[(13, (0.3, 0), (0, 1))]),
    dict(lines=["✦ Hyper = ⇧⌥⌃⌘", "左右空格长按"],
         x=X0 + 470, y=Y0 + 235, w=150, h=36, color="#CC99FF", align="right", arrows=[(44, (0.8, 0), (0.5, 1))]),
    dict(lines=["Soft-off 41 + 46 同按", "两半区一起关机；按 42 (Tab) 唤醒"],
         x=X0 + 190, y=Y0 + 275, w=220, h=36, color="#e74c3c", align="center",
         arrows=[(41, (0.2, 0), (0.5, 1)), (46, (0.8, 0), (0.5, 1))]),
    dict(lines=["Encoders 左: 音量  右: 亮度"],
         x=X0 + 210, y=Y0 - 80, w=160, h=20, color="#3399FF", align="center", arrows=[]),
]
LAYER_NOTES = {  # by display-name
    "Number": ("BOOT = 进入 bootloader 刷固件", "#c0392b"),
    "Symbol": ("BT 0/1/2 = 切换蓝牙配置；BOOT = bootloader", "#3399FF"),
    "Navigation": ("右手数字小键盘 (kp)；⌃⌘Q = 锁屏", "#a20025"),
    "Function": ("BT CLR = 清除所有蓝牙配对；CLR 0/1/2 = 只清除对应 profile", "#60a917"),
}


# ------------------------------------------------------------------ parsing --
def parse_layout(path):
    keys = []
    for m in re.finditer(
        r"&key_physical_attrs\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+\(?\s*(-?\d+)\s*\)?\s+(\d+)\s+(\d+)", path.read_text()
    ):
        w, h, x, y, rot, rx, ry = map(int, m.groups())
        cx, cy = x + w / 2, y + h / 2
        deg = rot / 100
        if rot:
            th = math.radians(deg)
            dx, dy = cx - rx, cy - ry
            cx = rx + dx * math.cos(th) - dy * math.sin(th)
            cy = ry + dx * math.sin(th) + dy * math.cos(th)
        keys.append((cx, cy, deg))
    return keys


def tokenize_bindings(body):
    out = []
    for chunk in body.split("&")[1:]:
        parts = chunk.split()
        if parts:
            out.append(parts)
    return out


def parse_keymap(path):
    text = path.read_text()
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    defines = {m.group(1): m.group(2) for m in re.finditer(r"#define\s+(\w+)\s+([^\n]+)", text)}
    layers = []
    for m in re.finditer(r'display-name\s*=\s*"([^"]+)"\s*;\s*bindings\s*=\s*<([^>]*)>', text):
        layers.append((m.group(1), tokenize_bindings(m.group(2))))
    combos = []
    cm = re.search(r"combos\s*\{(.*)\n    \};", text, flags=re.S)
    if cm:
        for c in re.finditer(r"key-positions\s*=\s*<([^>]*)>\s*;\s*bindings\s*=\s*<([^>]*)>", cm.group(1)):
            combos.append(([int(p) for p in c.group(1).split()], tokenize_bindings(c.group(2))[0]))
    return defines, layers, combos


def layer_index(name, defines, layers):
    if name in defines and defines[name].strip().isdigit():
        return int(defines[name])
    for i, (dn, _) in enumerate(layers):
        if dn.lower() == name.lower():
            return i
    return None


def keycode_label(code):
    """'LC(LG(Q))' -> '⌃⌘Q'; 'BSPC' -> '⌫'. Returns (main, sub)."""
    mods = ""
    while True:
        m = re.fullmatch(r"(\w\w)\((.*)\)", code)
        if not m or m.group(1) not in MOD_FN:
            break
        mods += MOD_FN[m.group(1)]
        code = m.group(2)
    if code in MOD_SYM and not mods:
        return MOD_SYM[code], MOD_NAME[MOD_SYM[code]]
    main = KEY_LABEL.get(code, code.replace("_", " ").title() if len(code) > 3 else code)
    return mods + main, KEY_SUB.get(code, "")


class Key:
    def __init__(self, main="", sub="", kind="key", layer=None):
        self.main, self.sub, self.kind, self.layer = main, sub, kind, layer


def binding_to_key(tok, defines, layers):
    b, args = tok[0], tok[1:]
    if b == "none":
        return Key()
    if b == "trans":
        return Key("▽", "", "trans")
    if b == "bootloader":
        return Key("BOOT", "", "boot")
    if b == "sys_reset":
        return Key("RESET", "", "boot")
    if b == "bt":
        if args[0] == "BT_SEL":
            return Key(f"BT {args[1]}", "", "bt")
        return Key(args[0].replace("BT_", "BT ").replace("_ALL", ""), "", "bt")
    m = re.fullmatch(r"bt_clr(\d)", b)
    if m:
        return Key(f"CLR {m.group(1)}", "BT", "bt")
    if b == "kp":
        main, sub = keycode_label(args[0])
        if args[0] in MOD_SYM:
            return Key(main, sub, "mod")
        return Key(main, sub)
    if b in ("hml", "hmr", "mt", "hm"):
        main, _ = keycode_label(args[1])
        return Key(main, MOD_SYM.get(args[0], args[0]))
    if b in ("vlt", "lt"):
        li = layer_index(args[0], defines, layers)
        main, _ = keycode_label(args[1])
        return Key(main, args[0], "layer", li)
    if b == "mo":
        li = layer_index(args[0], defines, layers)
        return Key("", args[0], "layer", li)
    if b == "to" or b == "tog":
        li = layer_index(args[0], defines, layers)
        return Key(b.upper(), args[0], "layer", li)
    return Key(" ".join(tok), "", "key")


# -------------------------------------------------------------------- model --
positions = parse_layout(LAYOUT)
defines, layers, combos = parse_keymap(KEYMAP)
assert layers, f"no layers parsed from {KEYMAP}"
for name, toks in layers:
    assert len(toks) == len(positions), f"layer {name}: {len(toks)} bindings vs {len(positions)} keys"

model = [[binding_to_key(t, defines, layers) for t in toks] for _, toks in layers]
held = set()  # (layer, pos) drawn as an empty dashed outline
for li, keys in enumerate(model):
    for pos, k in enumerate(keys):
        if k.kind == "layer" and k.layer is not None and model[k.layer][pos].kind in ("key", "trans") and not model[k.layer][pos].main:
            held.add((k.layer, pos))

if combos:
    base = model[0]
    combo_lines = ["Combo"] + [
        " + ".join(base[p].main for p in poss) + " → " + keycode_label(b[1])[0] if b[0] == "kp" else " ".join(b)
        for poss, b in combos
    ]
    NOTES.append(dict(lines=combo_lines, x=X0 + 90, y=Y0 - 70, w=140, h=32, color="#6906F9", align="left", arrows=[]))


def key_style(k, li):
    if k.kind == "layer" and k.layer is not None:
        return LAYER_COLORS[k.layer % len(LAYER_COLORS)]
    return {"mod": (MODFILL, MODSTROKE, MODFONT), "bt": (BTFILL, BTSTROKE, "#ffffff"),
            "boot": (BOOTFILL, BOOTSTROKE, "#ffffff")}.get(k.kind, (KEYFILL, KEYSTROKE, KEYFONT))


def key_xy(li, pos):
    cx, cy, rot = positions[pos]
    return X0 + cx * S - KEY / 2, Y0 + li * BLOCK_PITCH + cy * S - KEY / 2, rot


MID_X = X0 + (max(p[0] for p in positions) + min(p[0] for p in positions)) / 2 * S
LEFT_X, RIGHT_X = X0 - 30, X0 + max(p[0] for p in positions) * S + 60
WIDTH = int(RIGHT_X + 60)
HEIGHT = int(Y0 + len(layers) * BLOCK_PITCH)


def layer_edges():
    """(src layer, src pos, dst layer, colour, side x, left?)"""
    out, li_count = [], {}
    for li, keys in enumerate(model):
        for pos, k in enumerate(keys):
            if k.kind == "layer" and k.layer is not None and k.layer != li:
                cx = positions[pos][0]
                left = cx < LEFT_HALF_MAX_X
                n = li_count.get(left, 0)
                li_count[left] = n + 1
                sx = LEFT_X - 20 * n if left else RIGHT_X + 20 * n
                out.append((li, pos, k.layer, LAYER_COLORS[k.layer % len(LAYER_COLORS)][0], sx, left))
    return out


# ------------------------------------------------------------------- drawio --
def write_drawio(path):
    mxfile = ET.Element("mxfile", host="app.diagrams.net")
    diagram = ET.SubElement(mxfile, "diagram", id=f"{KEYBOARD}-keymap", name=KEYBOARD)
    modelx = ET.SubElement(diagram, "mxGraphModel", dx="1400", dy="900", grid="1", gridSize="10", guides="1",
                           tooltips="1", connect="1", arrows="1", fold="1", page="0", pageScale="1",
                           pageWidth="827", pageHeight="1169", math="0", shadow="0")
    root = ET.SubElement(modelx, "root")
    ET.SubElement(root, "mxCell", id="0")
    ET.SubElement(root, "mxCell", id="1", parent="0")
    nid = [1]

    def cell(value, style, x, y, w, h):
        nid[0] += 1
        i = f"c{nid[0]}"
        c = ET.SubElement(root, "mxCell", id=i, value=value, style=style, vertex="1", parent="1")
        ET.SubElement(c, "mxGeometry", x=f"{x:.1f}", y=f"{y:.1f}", width=str(w), height=str(h), **{"as": "geometry"})
        return i

    def edge(src, tgt, color, points, exit, entry, ortho=True):
        nid[0] += 1
        st = ("edgeStyle=orthogonalEdgeStyle;" if ortho else "") + (
            f"rounded=1;html=1;endArrow=classic;strokeColor={color};strokeWidth=1.5;"
            f"exitX={exit[0]};exitY={exit[1]};exitDx=0;exitDy=0;entryX={entry[0]};entryY={entry[1]};entryDx=0;entryDy=0;")
        c = ET.SubElement(root, "mxCell", id=f"e{nid[0]}", style=st, edge="1", parent="1", source=src, target=tgt)
        g = ET.SubElement(c, "mxGeometry", relative="1", **{"as": "geometry"})
        if points:
            arr = ET.SubElement(g, "Array", **{"as": "points"})
            for px, py in points:
                ET.SubElement(arr, "mxPoint", x=f"{px:.0f}", y=f"{py:.0f}")

    KEYSTYLE = "whiteSpace=wrap;html=1;aspect=fixed;rounded=1;fontFamily=Verdana;fontSize=15;fontStyle=1;"
    title_ids, key_ids = {}, {}
    for li, (lname, _) in enumerate(layers):
        oy = Y0 + li * BLOCK_PITCH
        tfill, tstroke, tfont = LAYER_COLORS[li % len(LAYER_COLORS)]
        title_ids[li] = cell(f"<b>{html.escape(lname)}</b>",
                             f"rounded=0;whiteSpace=wrap;html=1;fontFamily=Verdana;fontSize=17;fontColor={tfont};fillColor={tfill};strokeColor={tstroke};",
                             MID_X - 105, oy - 45, 210, 30)
        for pos, k in enumerate(model[li]):
            x, y, rot = key_xy(li, pos)
            fill, stroke, font = key_style(k, li)
            extra = ""
            if (li, pos) in held:
                value, fill, stroke, extra = "", "none", tfill, "dashed=1;strokeWidth=2;"
            elif k.kind == "mod":
                value = f'<font style="font-size:20px">{html.escape(k.main)}</font><br><font style="font-size:8px;font-weight:normal">{html.escape(k.sub)}</font>'
            elif k.sub:
                value = f'<b>{html.escape(k.main)}</b><br><font style="font-size:9px;font-weight:normal">{html.escape(k.sub)}</font>'
            else:
                value = html.escape(k.main)
            st = KEYSTYLE + f"fillColor={fill};strokeColor={stroke};fontColor={font};" + extra
            if rot:
                st += f"rotation={rot:.2f};"
            key_ids[(li, pos)] = cell(value, st, x, y, KEY, KEY)
        if lname in LAYER_NOTES:
            txt, color = LAYER_NOTES[lname]
            cell(f"<b>{html.escape(lname)} 层:</b> {html.escape(txt)}",
                 f"text;html=1;strokeColor=none;fillColor=none;align=left;verticalAlign=middle;whiteSpace=wrap;fontFamily=Verdana;fontSize=11;fontColor={color};",
                 X0 - 10, oy + 240, 320, 20)

    for li, pos, dst, color, sx, left in layer_edges():
        _, y, _ = key_xy(li, pos)
        ty = Y0 + dst * BLOCK_PITCH - 30
        edge(key_ids[(li, pos)], title_ids[dst], color, [(sx, y + KEY + 20), (sx, ty)], (0.5, 1), ((0, 0.5) if left else (1, 0.5)))

    for n in NOTES:
        value = "<br>".join(f"<b>{html.escape(l)}</b>" if i == 0 else html.escape(l) for i, l in enumerate(n["lines"]))
        nid_ = cell(value, f"text;html=1;strokeColor=none;fillColor=none;align={n['align']};verticalAlign=middle;whiteSpace=wrap;fontFamily=Verdana;fontSize=11;fontColor={n['color']};",
                    n["x"], n["y"], n["w"], n["h"])
        for pos, exit, entry in n["arrows"]:
            edge(nid_, key_ids[(0, pos)], n["color"], [], exit, entry, ortho=False)

    ET.ElementTree(mxfile).write(path, encoding="utf-8", xml_declaration=True)


# ---------------------------------------------------------------------- svg --
def _arrow_colors():
    return sorted({e[3] for e in layer_edges()} | {n["color"] for n in NOTES if n["arrows"]})


def write_svg(path):
    e = html.escape
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{HEIGHT}" viewBox="0 0 {WIDTH} {HEIGHT}" '
         f'font-family="Verdana, DejaVu Sans, Helvetica, Arial, sans-serif">',
         "<defs>" + "".join(
             f'<marker id="arr{c[1:]}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto">'
             f'<path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>' for c in _arrow_colors()) + "</defs>",
         f'<rect width="100%" height="100%" fill="#f4f4f4"/>']

    def text(x, y, s, size, fill, anchor="middle", weight="bold"):
        o.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" text-anchor="{anchor}" font-weight="{weight}">{e(s)}</text>')

    def note_box(n):
        ax = {"left": n["x"] + 2, "right": n["x"] + n["w"] - 2, "center": n["x"] + n["w"] / 2}[n["align"]]
        anchor = {"left": "start", "right": "end", "center": "middle"}[n["align"]]
        lh = 13
        y = n["y"] + n["h"] / 2 - lh * (len(n["lines"]) - 1) / 2 + 4
        for i, l in enumerate(n["lines"]):
            text(ax, y + i * lh, l, 10, n["color"], anchor, "bold" if i == 0 else "normal")

    def line(points, color, marker=True):
        d = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
        o.append(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="1.5" stroke-linejoin="round"'
                 + (f' marker-end="url(#arr{color[1:]})"' if marker else "") + "/>")

    for li, (lname, _) in enumerate(layers):
        oy = Y0 + li * BLOCK_PITCH
        tfill, tstroke, tfont = LAYER_COLORS[li % len(LAYER_COLORS)]
        o.append(f'<rect x="{MID_X - 105:.1f}" y="{oy - 45}" width="210" height="30" fill="{tfill}" stroke="{tstroke}"/>')
        text(MID_X, oy - 25, lname, 15, tfont)
        for pos, k in enumerate(model[li]):
            x, y, rot = key_xy(li, pos)
            fill, stroke, font = key_style(k, li)
            tr = f' transform="rotate({rot:.2f} {x + KEY / 2:.1f} {y + KEY / 2:.1f})"' if rot else ""
            o.append(f"<g{tr}>")
            if (li, pos) in held:
                o.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{KEY}" height="{KEY}" rx="5" fill="none" stroke="{tfill}" stroke-width="2" stroke-dasharray="4 3"/>')
            else:
                o.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{KEY}" height="{KEY}" rx="5" fill="{fill}" stroke="{stroke}"/>')
                cx = x + KEY / 2
                if k.kind == "mod":
                    text(cx, y + 21, k.main, 17, font)
                    text(cx, y + 33, k.sub, 7, font, weight="normal")
                elif k.sub:
                    size = 14 if len(k.main) <= 2 else 11 if len(k.main) <= 4 else 8
                    text(cx, y + 20, k.main, size, font)
                    text(cx, y + 33, k.sub, 7, font, weight="normal")
                elif k.main:
                    size = 14 if len(k.main) <= 2 else 10 if len(k.main) <= 4 else 8
                    text(cx, y + KEY / 2 + size * 0.36, k.main, size, font)
            o.append("</g>")
        if lname in LAYER_NOTES:
            txt, color = LAYER_NOTES[lname]
            o.append(f'<text x="{X0 - 8}" y="{oy + 254}" font-size="10" fill="{color}"><tspan font-weight="bold">{e(lname)} 层:</tspan> {e(txt)}</text>')

    for li, pos, dst, color, sx, left in layer_edges():
        x, y, _ = key_xy(li, pos)
        ty = Y0 + dst * BLOCK_PITCH - 30
        tx = MID_X - 105 if left else MID_X + 105
        line([(x + KEY / 2, y + KEY), (x + KEY / 2, y + KEY + 20), (sx, y + KEY + 20), (sx, ty), (tx, ty)], color)

    for n in NOTES:
        note_box(n)
        for pos, exit, entry in n["arrows"]:
            kx, ky, _ = key_xy(0, pos)
            line([(n["x"] + n["w"] * exit[0], n["y"] + n["h"] * exit[1]), (kx + KEY * entry[0], ky + KEY * entry[1])], n["color"])

    o.append("</svg>")
    path.write_text("\n".join(o) + "\n", encoding="utf-8")


write_drawio(OUT_DRAWIO)
write_svg(OUT_SVG)
print(f"wrote {OUT_DRAWIO.relative_to(ROOT)} and {OUT_SVG.relative_to(ROOT)} ({len(layers)} layers, {len(positions)} keys)")
