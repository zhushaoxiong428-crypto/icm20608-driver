#!/usr/bin/env python3
"""按屏幕生成小屏版 mainwindow.ui（控件 objectName 与原 1024x600 版完全一致，C++ 无需改动）。

用法：python3 gen_ui.py 800x480   -> ui_800x480/mainwindow.ui
      python3 gen_ui.py 480x272   -> ui_480x272/mainwindow.ui

两种屏都是 4.3 寸，物理尺寸相同：
  480x272（ATK-4342）约 0.20 mm/px，800x480（ATK-4384）约 0.12 mm/px，
  所以 800x480 的字号、按钮等按 1.5 倍放大，保证手指点得准、字看得清。
"""
import os
import sys
import xml.etree.ElementTree as ET

PROFILES = {
    "480x272": dict(w=480, h=272, scale=1.0),
    "800x480": dict(w=800, h=480, scale=1.5),
}

profile_name = sys.argv[1] if len(sys.argv) > 1 else "800x480"
P = PROFILES[profile_name]


def s(n):
    """按屏幕缩放像素值"""
    return int(round(n * P["scale"]))


FONT = s(12)
SMALL = s(11)
BTN_H = s(30)  # 触摸按钮高度约 6 mm

STYLE = f"""QWidget#centralwidget {{
	background-color: #1e1e1e;
	color: #d4d4d4;
}}
QWidget {{
	font-family: Consolas;
	font-size: {FONT}px;
}}
QLabel {{
	color: #d4d4d4;
}}
QGroupBox {{
	background-color: #252526;
	border: 1px solid #454545;
	border-radius: {s(4)}px;
	margin-top: {s(9)}px;
	padding: {s(2)}px;
}}
QGroupBox::title {{
	subcontrol-origin: margin;
	subcontrol-position: top left;
	left: {s(8)}px;
	padding: 0 {s(3)}px;
	color: #4fc1ff;
}}
QGroupBox#controlGroup {{
	margin-top: 0px;
}}
QPushButton {{
	background-color: #333333;
	color: #ffffff;
	border: 1px solid #555555;
	border-radius: {s(4)}px;
	padding: {s(2)}px {s(8)}px;
}}
QPushButton:pressed {{
	background-color: #094771;
	border-color: #4fc1ff;
}}
QPushButton:disabled {{
	background-color: #292929;
	color: #777777;
	border-color: #383838;
}}
QPushButton#exitButton {{
	background-color: #683333;
}}
QPushButton#exitButton:pressed {{
	background-color: #8c4242;
}}
QPushButton#applySettingsButton, QPushButton#recordButton {{
	background-color: #094771;
}}
QLabel#apStatusLabel, QLabel#icmStatusLabel, QLabel#settingsHintLabel {{
	color: #dcdcaa;
}}
QLabel#settingsHintLabel, QLabel#configPathLabel, QLabel#recordInfoLabel, QLabel#recordPathLabel {{
	font-size: {SMALL}px;
	color: #999999;
}}
QWidget#monitorPage, QWidget#settingsPage, QWidget#recordPage {{
	background-color: #1e1e1e;
}}
QTabWidget::pane {{
	border: 1px solid #454545;
	border-radius: {s(4)}px;
	top: -1px;
}}
QTabBar::tab {{
	background-color: #2d2d30;
	color: #d4d4d4;
	min-width: {s(72)}px;
	min-height: {s(24)}px;
	padding: 0 {s(10)}px;
}}
QTabBar::tab:selected {{
	background-color: #094771;
	color: #ffffff;
}}
QCheckBox {{
	color: #d4d4d4;
	spacing: {s(6)}px;
}}
QCheckBox::indicator {{
	width: {s(18)}px;
	height: {s(18)}px;
}}
QCheckBox:disabled {{
	color: #777777;
}}
QSpinBox {{
	background-color: #333333;
	color: #ffffff;
	border: 1px solid #555555;
	border-radius: {s(4)}px;
	padding: {s(1)}px {s(26)}px {s(1)}px {s(6)}px;
}}
QSpinBox::up-button, QSpinBox::down-button {{
	width: {s(24)}px;
}}
QStatusBar {{
	font-size: {SMALL}px;
	color: #999999;
}}
"""


def prop(parent, name, kind, value, **attrs):
    p = ET.SubElement(parent, "property", name=name, **attrs)
    if kind == "size" or kind == "rect":
        v = ET.SubElement(p, kind)
        for k, val in value:
            ET.SubElement(v, k).text = str(val)
    else:
        ET.SubElement(p, kind).text = str(value)
    return p


def widget(parent_item, cls, name, **props):
    w = ET.SubElement(parent_item, "widget", {"class": cls, "name": name})
    for k, v in props.items():
        if k == "text" or k == "title" or k == "suffix":
            prop(w, k, "string", v)
        elif k == "wordWrap":
            prop(w, k, "bool", "true" if v else "false")
        elif k == "minH":
            prop(w, "minimumSize", "size", [("width", 0), ("height", v)])
        elif k == "minW_H":
            prop(w, "minimumSize", "size", [("width", v[0]), ("height", v[1])])
        elif k == "noFocus":
            prop(w, "focusPolicy", "enum", "Qt::NoFocus")
        elif k in ("minimum", "maximum", "value", "singleStep"):
            prop(w, k, "number", v)
    return w


def layout(parent, cls, name, margins=(0, 0, 0, 0), spacing=None, stretch=None):
    attrs = {"class": cls, "name": name}
    if stretch:
        attrs["stretch"] = stretch
    l = ET.SubElement(parent, "layout", attrs)
    if spacing is not None:
        prop(l, "spacing", "number", spacing)
    for side, m in zip(("left", "top", "right", "bottom"), margins):
        prop(l, side + "Margin", "number", m)
    return l


def m4(a, b, c, d):
    return (s(a), s(b), s(c), s(d))


def item(l, **attrs):
    return ET.SubElement(l, "item", {k: str(v) for k, v in attrs.items()})


def vspacer(l, name):
    sp = ET.SubElement(item(l), "spacer", name=name)
    prop(sp, "orientation", "enum", "Qt::Vertical")
    prop(sp, "sizeHint", "size", [("width", 0), ("height", 0)], stdset="0")


def page(tabs, name, title):
    pg = ET.SubElement(tabs, "widget", {"class": "QWidget", "name": name})
    ET.SubElement(ET.SubElement(pg, "attribute", name="title"), "string").text = title
    return pg


ui = ET.Element("ui", version="4.0")
ET.SubElement(ui, "class").text = "MainWindow"
mw = ET.SubElement(ui, "widget", {"class": "QMainWindow", "name": "MainWindow"})
prop(mw, "geometry", "rect", [("x", 0), ("y", 0), ("width", P["w"]), ("height", P["h"])])
prop(mw, "windowTitle", "string", "Device Monitor")

central = ET.SubElement(mw, "widget", {"class": "QWidget", "name": "centralwidget"})
ET.SubElement(ET.SubElement(central, "property", name="styleSheet"), "string", notr="true").text = STYLE
main = layout(central, "QVBoxLayout", "mainLayout", m4(3, 3, 3, 0), s(3), "1,0")

tabs = widget(item(main), "QTabWidget", "mainTabWidget")
prop(tabs, "currentIndex", "number", 0)

# ---- Monitor 页：左 AP3216C（短行），右 ICM20608（ACC 一行约 33 字符，需要更宽）
mon = page(tabs, "monitorPage", "Monitor")
monl = layout(mon, "QHBoxLayout", "monitorLayout", m4(3, 2, 3, 3), s(4), "1,2")

env = widget(item(monl), "QGroupBox", "environmentGroup", title="Environment")
envl = layout(env, "QVBoxLayout", "verticalLayout", m4(4, 4, 4, 4), s(2))
widget(item(envl), "QLabel", "apStatusLabel", text="AP3216C: WAITING")
widget(item(envl), "QLabel", "irLabel", text="IR: --")
widget(item(envl), "QLabel", "alsLabel", text="ALS: --")
widget(item(envl), "QLabel", "psLabel", text="PS: --")
vspacer(envl, "envSpacer")
widget(item(envl), "QPushButton", "readSensorButton", text="Read Sensor", minH=BTN_H, noFocus=True)

mot = widget(item(monl), "QGroupBox", "motionGroup", title="Motion")
motl = layout(mot, "QVBoxLayout", "verticalLayout_2", m4(4, 4, 4, 4), s(2))
widget(item(motl), "QLabel", "icmStatusLabel", text="ICM20608: WAITING")
widget(item(motl), "QLabel", "accelLabel", text="ACC: X=-- Y=-- Z=--", wordWrap=True)
widget(item(motl), "QLabel", "gyroLabel", text="GYRO: X=-- Y=-- Z=--", wordWrap=True)
widget(item(motl), "QLabel", "tempLabel", text="TEMP: --")
vspacer(motl, "motionSpacer")

# ---- Settings 页：三行参数 + 右侧 Apply，下方反馈/提示/配置路径
st = page(tabs, "settingsPage", "Settings")
stl = layout(st, "QVBoxLayout", "settingsLayout", m4(3, 2, 3, 3), s(2))
sg = widget(item(stl), "QGroupBox", "settingsGroup", title="Sampling and Auto LED")
sgl = layout(sg, "QGridLayout", "settingsGridLayout", m4(6, 4, 6, 4), None)
prop(sgl, "horizontalSpacing", "number", s(6))
prop(sgl, "verticalSpacing", "number", s(3))
rows = [
    ("lightOnSpinBoxLabel", "LED ON threshold", "lightOnSpinBox", dict(minimum=0, maximum=65535, value=20)),
    ("lightOffSpinBoxLabel", "LED OFF threshold", "lightOffSpinBox", dict(minimum=0, maximum=65535, value=50)),
    ("sampleIntervalSpinBoxLabel", "Sample interval", "sampleIntervalSpinBox",
     dict(suffix=" ms", minimum=200, maximum=60000, singleStep=100, value=1000)),
]
for r, (ln, lt, sn, sp) in enumerate(rows):
    widget(item(sgl, row=r, column=0), "QLabel", ln, text=lt)
    widget(item(sgl, row=r, column=1), "QSpinBox", sn, minW_H=(s(120), s(26)), **sp)
widget(item(sgl, row=0, column=2, rowspan=3), "QPushButton", "applySettingsButton",
       text="Apply", minW_H=(s(70), 3 * s(26)), noFocus=True)
widget(item(stl), "QLabel", "settingsFeedbackLabel",
       text="Parameters apply after pressing Apply.", wordWrap=True)
widget(item(stl), "QLabel", "settingsHintLabel",
       text="ALS thresholds use raw counts, not lux. LED ON must be lower than LED OFF.", wordWrap=True)
widget(item(stl), "QLabel", "configPathLabel", text="Config: device_monitor.ini", wordWrap=True)
vspacer(stl, "settingsSpacer")

# ---- Record 页（原来在 Settings 页右半边，小屏上单独成页）
rp = page(tabs, "recordPage", "Record")
rpl = layout(rp, "QVBoxLayout", "recordPageLayout", m4(3, 2, 3, 3), s(2))
rg = widget(item(rpl), "QGroupBox", "recordGroup", title="Data Recording")
rgl = layout(rg, "QVBoxLayout", "recordLayout", m4(6, 4, 6, 4), s(3))
rrow = layout(item(rgl), "QHBoxLayout", "recordStatusRow", spacing=s(8), stretch="1,0")
widget(item(rrow), "QLabel", "recordStatusLabel", text="Recording: OFF")
widget(item(rrow), "QPushButton", "recordButton", text="Start Recording", minW_H=(s(140), BTN_H), noFocus=True)
widget(item(rgl), "QLabel", "recordPathLabel", text="Path: --", wordWrap=True)
widget(item(rgl), "QLabel", "recordInfoLabel",
       text="Two timestamped CSV files per session.\nAP: raw values. ICM: g, dps, C.", wordWrap=True)
vspacer(rpl, "recordSpacer")

# ---- 底部常驻控制条（无标题，省高度）
cg = widget(item(main), "QGroupBox", "controlGroup")
cgl = layout(cg, "QHBoxLayout", "horizontalLayout", m4(4, 2, 4, 2), s(4), "1,0,2,0")
widget(item(cgl), "QLabel", "keyLabel", text="KEY: --", minW_H=(s(96), 0))
widget(item(cgl), "QCheckBox", "autoModeCheckBox", text="Auto LED", minH=BTN_H, noFocus=True)
widget(item(cgl), "QPushButton", "pushButton", text="LedTouch", minH=BTN_H, noFocus=True)
widget(item(cgl), "QPushButton", "exitButton", text="Exit", minW_H=(s(56), BTN_H), noFocus=True)

sb = ET.SubElement(mw, "widget", {"class": "QStatusBar", "name": "statusbar"})
# 状态栏不在 centralwidget 内，单独设置样式；关掉右下角拖拽手柄（全屏用不到）
ET.SubElement(ET.SubElement(sb, "property", name="styleSheet"), "string", notr="true").text = (
    f"QStatusBar {{ background-color: #1e1e1e; color: #999999; font-family: Consolas; font-size: {SMALL}px; }}")
prop(sb, "sizeGripEnabled", "bool", "false")
ET.SubElement(ui, "resources")
ET.SubElement(ui, "connections")

ET.indent(ui, space=" ")
out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui_" + profile_name)
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "mainwindow.ui")
with open(out, "w", encoding="utf-8") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
    f.write(ET.tostring(ui, encoding="unicode"))
    f.write("\n")
print("wrote", out)
