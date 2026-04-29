#!/usr/bin/env python3
"""Generate HiTag2 hardware fix diagram using pycairo."""
import cairo, math

W, H = 900, 660
surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, W, H)
cr = cairo.Context(surface)

# ── helpers ───────────────────────────────────────────────────────────────────

def bg(r, g, b): return r/255, g/255, b/255

def rounded_rect(cr, x, y, w, h, r=10, fill=None, stroke=None, lw=1.5):
    cr.new_path()
    cr.arc(x+r, y+r, r, math.pi, 3*math.pi/2)
    cr.arc(x+w-r, y+r, r, 3*math.pi/2, 0)
    cr.arc(x+w-r, y+h-r, r, 0, math.pi/2)
    cr.arc(x+r, y+h-r, r, math.pi/2, math.pi)
    cr.close_path()
    if fill:
        cr.set_source_rgb(*fill); cr.fill_preserve()
    if stroke:
        cr.set_source_rgb(*stroke); cr.set_line_width(lw); cr.stroke()

def text(cr, s, x, y, size=13, bold=False, color=(0,0,0), align='center'):
    cr.select_font_face("Sans",
        cairo.FONT_SLANT_NORMAL,
        cairo.FONT_WEIGHT_BOLD if bold else cairo.FONT_WEIGHT_NORMAL)
    cr.set_font_size(size)
    ext = cr.text_extents(s)
    if align == 'center':
        ox = -ext.width/2 - ext.x_bearing
    elif align == 'right':
        ox = -ext.width - ext.x_bearing
    else:
        ox = -ext.x_bearing
    cr.set_source_rgb(*color)
    cr.move_to(x + ox, y)
    cr.show_text(s)

def arrow(cr, x1, y1, x2, y2, color=(0.8,0,0), lw=2.0, head=10):
    cr.set_source_rgb(*color)
    cr.set_line_width(lw)
    cr.move_to(x1, y1); cr.line_to(x2, y2); cr.stroke()
    angle = math.atan2(y2-y1, x2-x1)
    for da in (0.4, -0.4):
        cr.move_to(x2, y2)
        cr.line_to(x2 - head*math.cos(angle-da), y2 - head*math.sin(angle-da))
    cr.stroke()

# ── background ────────────────────────────────────────────────────────────────
cr.set_source_rgb(*bg(245, 247, 250))
cr.paint()

# ── title bar ─────────────────────────────────────────────────────────────────
rounded_rect(cr, 20, 12, W-40, 46, r=8,
             fill=bg(30, 60, 120), stroke=bg(20,45,95), lw=1)
text(cr, "HiTag2 Hardware Fix — LF Antenna Damping Resistor",
     W/2, 43, size=17, bold=True, color=(1,1,1))

# ══════════════════════════════════════════════════════════════════════════════
# LEFT PANEL — Flipper Zero PCB overview
# ══════════════════════════════════════════════════════════════════════════════
PCB_X, PCB_Y, PCB_W, PCB_H = 32, 72, 380, 320

# PCB body
rounded_rect(cr, PCB_X, PCB_Y, PCB_W, PCB_H, r=14,
             fill=bg(30, 100, 50), stroke=bg(20,75,35), lw=2.5)
text(cr, "Flipper Zero PCB (top view — simplified)",
     PCB_X + PCB_W/2, PCB_Y + 22, size=12, bold=True, color=(0.9,1,0.9))

# Screen area
rounded_rect(cr, PCB_X+20, PCB_Y+35, 200, 120, r=6,
             fill=bg(15,40,15), stroke=bg(100,200,120), lw=1.5)
text(cr, "Screen", PCB_X+120, PCB_Y+100, size=11, color=bg(100,200,120))

# USB-C port
rounded_rect(cr, PCB_X+PCB_W-70, PCB_Y+50, 55, 28, r=4,
             fill=bg(180,180,190), stroke=bg(100,100,110), lw=1.5)
text(cr, "USB-C", PCB_X+PCB_W-42, PCB_Y+68, size=10, color=bg(40,40,40))

# SD card slot
rounded_rect(cr, PCB_X+PCB_W-65, PCB_Y+95, 50, 22, r=3,
             fill=bg(200,190,170), stroke=bg(130,120,100), lw=1.5)
text(cr, "SD", PCB_X+PCB_W-40, PCB_Y+110, size=9, color=bg(40,40,40))

# Buttons
for bx, label in [(PCB_X+235, "◄"), (PCB_X+260, "▲"), (PCB_X+285, "●"),
                   (PCB_X+310, "▼"), (PCB_X+335, "►")]:
    cr.arc(bx, PCB_Y+105, 9, 0, 2*math.pi)
    cr.set_source_rgb(*bg(60,60,60)); cr.fill()
    text(cr, label, bx, PCB_Y+110, size=8, color=(0.9,0.9,0.9))

# LF RFID antenna coil — bottom-left area of PCB
COIL_CX = PCB_X + 105
COIL_CY = PCB_Y + 230

# Draw coil as concentric arcs (simplified spiral)
cr.set_line_width(2.0)
for i, (rx, ry, a1, a2, col) in enumerate([
    (65, 50, 0, 2*math.pi, bg(180,140,60)),
    (52, 40, 0, 2*math.pi, bg(180,140,60)),
    (39, 30, 0, 2*math.pi, bg(180,140,60)),
    (26, 20, 0, 2*math.pi, bg(180,140,60)),
]):
    cr.set_source_rgb(*col)
    cr.set_line_width(2.5 - i*0.3)
    cr.new_path()
    cr.save()
    cr.translate(COIL_CX, COIL_CY)
    cr.scale(1, ry/rx)
    cr.arc(0, 0, rx, 0, 2*math.pi)
    cr.restore()
    cr.stroke()

text(cr, "LF RFID", COIL_CX, COIL_CY - 4, size=10, bold=True, color=(1,1,0.6))
text(cr, "Antenna Coil", COIL_CX, COIL_CY + 10, size=10, color=(1,1,0.6))

# Coil lead lines — outer ring exits bottom
LEAD_X1 = COIL_CX - 68   # left lead
LEAD_X2 = COIL_CX + 68   # right lead (toward resistor mod point)
LEAD_Y  = COIL_CY + 2

cr.set_source_rgb(*bg(220,170,80))
cr.set_line_width(2.5)

# left lead goes down-left to pad A
PAD_A_X = PCB_X + 28; PAD_A_Y = PCB_Y + 290
cr.move_to(LEAD_X1, LEAD_Y)
cr.line_to(PAD_A_X, PAD_A_Y)
cr.stroke()

# right lead goes right then down to gap / resistor mod point
MOD_X = PCB_X + 210; MOD_Y = PCB_Y + 258
cr.move_to(LEAD_X2, LEAD_Y)
cr.line_to(MOD_X, MOD_Y)
cr.stroke()

# Pad A (solder point — intact)
cr.arc(PAD_A_X, PAD_A_Y, 7, 0, 2*math.pi)
cr.set_source_rgb(*bg(200,160,50)); cr.fill()
cr.arc(PAD_A_X, PAD_A_Y, 7, 0, 2*math.pi)
cr.set_source_rgb(*bg(240,200,80)); cr.set_line_width(1.5); cr.stroke()
text(cr, "A", PAD_A_X, PAD_A_Y+4, size=8, bold=True, color=(0,0,0))

# Mod point — where trace is cut and resistor added
cr.arc(MOD_X, MOD_Y, 8, 0, 2*math.pi)
cr.set_source_rgb(*bg(220,80,80)); cr.fill()
cr.arc(MOD_X, MOD_Y, 8, 0, 2*math.pi)
cr.set_source_rgb(*bg(255,100,100)); cr.set_line_width(1.5); cr.stroke()
text(cr, "✕", MOD_X, MOD_Y+5, size=11, bold=True, color=(1,1,1))

text(cr, "cut trace /", MOD_X, MOD_Y + 22, size=9, color=(1,0.9,0.5))
text(cr, "solder R here", MOD_X, MOD_Y + 34, size=9, color=(1,0.9,0.5))

# continuation line from mod point to pad B (toward driver IC or PB13 path)
PAD_B_X = PCB_X + 340; PAD_B_Y = PCB_Y + 258
cr.set_source_rgb(*bg(220,170,80))
cr.set_line_width(2.5)
cr.move_to(MOD_X + 9, MOD_Y)
cr.line_to(PAD_B_X, PAD_B_Y)
cr.stroke()

cr.arc(PAD_B_X, PAD_B_Y, 7, 0, 2*math.pi)
cr.set_source_rgb(*bg(200,160,50)); cr.fill()
cr.arc(PAD_B_X, PAD_B_Y, 7, 0, 2*math.pi)
cr.set_source_rgb(*bg(240,200,80)); cr.set_line_width(1.5); cr.stroke()
text(cr, "B", PAD_B_X, PAD_B_Y+4, size=8, bold=True, color=(0,0,0))

# driver / MOSFET label
rounded_rect(cr, PAD_B_X-2, PCB_Y+200, 42, 30, r=4,
             fill=bg(50,50,70), stroke=bg(100,100,140), lw=1.5)
text(cr, "RFID", PAD_B_X+19, PCB_Y+218, size=8, color=bg(180,180,220))
text(cr, "Driver", PAD_B_X+19, PCB_Y+228, size=8, color=bg(180,180,220))

cr.set_source_rgb(*bg(220,170,80)); cr.set_line_width(2)
cr.move_to(PAD_B_X+19, PAD_B_Y-7); cr.line_to(PAD_B_X+19, PCB_Y+230); cr.stroke()

# ── PCB label: top arrow pointing to resistor spot
arrow(cr, PCB_X+210, PCB_Y+180, MOD_X, MOD_Y-10,
      color=bg(255,100,60), lw=2, head=9)
text(cr, "Add resistor here", PCB_X+210, PCB_Y+172,
     size=10, bold=True, color=bg(255,100,60))

# ══════════════════════════════════════════════════════════════════════════════
# MIDDLE PANEL — schematic before / after
# ══════════════════════════════════════════════════════════════════════════════
SCH_X = 430; SCH_Y = 72

text(cr, "SCHEMATIC", SCH_X + 215, SCH_Y + 16, size=13, bold=True,
     color=bg(30,60,120))

# ─── BEFORE ───────────────────────────────────────────────────────────────────
BEF_Y = SCH_Y + 34
rounded_rect(cr, SCH_X+10, BEF_Y, 420, 130, r=8,
             fill=bg(255,240,240), stroke=bg(200,160,160), lw=1.5)
text(cr, "BEFORE  (stock Flipper — τ ≈ 1000 µs)", SCH_X+220, BEF_Y+18,
     size=11, bold=True, color=bg(160,40,40))

# nodes
NX1 = SCH_X+50;  NY = BEF_Y+75
NX2 = SCH_X+200
NX3 = SCH_X+350; NY2 = BEF_Y+75

# PB13 driver box
rounded_rect(cr, NX1-38, NY-22, 75, 44, r=5,
             fill=bg(200,220,255), stroke=bg(80,100,180), lw=1.5)
text(cr, "PB13", NX1, NY-8, size=10, bold=True, color=bg(30,50,150))
text(cr, "TIM1_CH1N", NX1, NY+8, size=8, color=bg(30,50,150))

# wire PB13→coil
cr.set_source_rgb(*bg(40,40,40)); cr.set_line_width(2.5)
cr.move_to(NX1+37, NY); cr.line_to(NX2-35, NY); cr.stroke()

# coil symbol
def draw_coil(cr, cx, cy, n=4, rx=30, ry=12, color=bg(40,40,40)):
    cr.set_source_rgb(*color)
    cr.set_line_width(2)
    step = (2*rx) / n
    cr.move_to(cx - rx, cy)
    for i in range(n):
        x0 = cx - rx + i*step
        cr.arc(x0 + step/2, cy, step/2, math.pi, 0)
    cr.stroke()

draw_coil(cr, NX2+5, NY, n=4, rx=35, ry=14)

# wire coil→GND
cr.set_source_rgb(*bg(40,40,40)); cr.set_line_width(2.5)
cr.move_to(NX2+40, NY); cr.line_to(NX3+10, NY); cr.stroke()

# GND symbol
GX = NX3+10
cr.move_to(GX, NY); cr.line_to(GX, NY+18); cr.stroke()
for i, hw in enumerate([14, 9, 4]):
    yy = NY+18+i*6
    cr.move_to(GX-hw, yy); cr.line_to(GX+hw, yy); cr.stroke()

text(cr, "L (antenna coil)", NX2+5, NY+30, size=9, color=bg(60,60,60))
text(cr, "R_coil only — high Q → τ≈1000µs", SCH_X+220, BEF_Y+112,
     size=10, color=bg(160,40,40))

# ─── AFTER ────────────────────────────────────────────────────────────────────
AFT_Y = BEF_Y + 148
rounded_rect(cr, SCH_X+10, AFT_Y, 420, 145, r=8,
             fill=bg(235,255,235), stroke=bg(140,190,140), lw=1.5)
text(cr, "AFTER  (15–22 Ω added — τ ≈ 200 µs)", SCH_X+220, AFT_Y+18,
     size=11, bold=True, color=bg(30,120,30))

NY3 = AFT_Y + 80

# PB13 driver
rounded_rect(cr, NX1-38, NY3-22, 75, 44, r=5,
             fill=bg(200,220,255), stroke=bg(80,100,180), lw=1.5)
text(cr, "PB13", NX1, NY3-8, size=10, bold=True, color=bg(30,50,150))
text(cr, "TIM1_CH1N", NX1, NY3+8, size=8, color=bg(30,50,150))

# wire → resistor
RES_CX = NX1 + 80
cr.set_source_rgb(*bg(40,40,40)); cr.set_line_width(2.5)
cr.move_to(NX1+37, NY3); cr.line_to(RES_CX-22, NY3); cr.stroke()

# resistor symbol (zigzag)
def draw_resistor(cr, cx, cy, color=bg(40,40,40)):
    cr.set_source_rgb(*color)
    cr.set_line_width(2.2)
    pts = [(-22,0),(-16,-10),(-8,10),(0,-10),(8,10),(16,-10),(22,0)]
    cr.move_to(cx+pts[0][0], cy+pts[0][1])
    for dx,dy in pts[1:]:
        cr.line_to(cx+dx, cy+dy)
    cr.stroke()
    # box around it
    cr.set_line_width(1.2)
    cr.set_source_rgb(*bg(200,120,20))
    rounded_rect(cr, cx-24, cy-14, 48, 28, r=4)
    cr.stroke()

draw_resistor(cr, RES_CX, NY3)

text(cr, "R_damp", RES_CX, NY3-20, size=9, bold=True, color=bg(180,80,10))
text(cr, "15–22 Ω", RES_CX, NY3+28, size=10, bold=True, color=bg(180,80,10))
text(cr, "0402/0603", RES_CX, NY3+40, size=8, color=bg(120,60,10))

# wire R→coil
COIL2_CX = NX2 + 50
cr.set_source_rgb(*bg(40,40,40)); cr.set_line_width(2.5)
cr.move_to(RES_CX+22, NY3); cr.line_to(COIL2_CX-35, NY3); cr.stroke()

draw_coil(cr, COIL2_CX+5, NY3, n=4, rx=35, ry=14)

# wire → GND
cr.set_source_rgb(*bg(40,40,40)); cr.set_line_width(2.5)
cr.move_to(COIL2_CX+40, NY3); cr.line_to(NX3+10, NY3); cr.stroke()
GX2 = NX3+10
cr.move_to(GX2, NY3); cr.line_to(GX2, NY3+18); cr.stroke()
for i, hw in enumerate([14,9,4]):
    yy = NY3+18+i*6
    cr.move_to(GX2-hw, yy); cr.line_to(GX2+hw, yy); cr.stroke()

text(cr, "L (antenna coil)", COIL2_CX+5, NY3+30, size=9, color=bg(60,60,60))
text(cr, "R_total = R_coil + 15–22 Ω → lower Q → τ≈200µs → 160µs gap works",
     SCH_X+220, AFT_Y+126, size=9, bold=True, color=bg(30,120,30))

# ══════════════════════════════════════════════════════════════════════════════
# BOTTOM PANEL — step-by-step instructions
# ══════════════════════════════════════════════════════════════════════════════
INST_Y = 408
rounded_rect(cr, 20, INST_Y, W-40, 230, r=10,
             fill=bg(248,250,255), stroke=bg(180,190,220), lw=1.5)

text(cr, "Step-by-step Installation", W/2, INST_Y+22, size=14, bold=True,
     color=bg(30,60,120))

steps = [
    ("1", "Power off the Flipper Zero completely."),
    ("2", "Open the case — remove the four screws on the back, lift the rear shell."),
    ("3", "Locate the LF RFID antenna coil (large flat spiral near the top of the PCB)."),
    ("4", "Trace one of the coil leads back to where it meets a circuit trace on the PCB."),
    ("5", "Using a sharp blade, cut that PCB trace cleanly at one point (pad B in diagram)."),
    ("6", "Solder a 15–22 Ω resistor (0402 or 0603 SMD) across the cut, bridging pads A and B."),
    ("7", "Verify continuity: ~15–22 Ω between the coil lead and the driver output."),
    ("8", "Reassemble.  Re-run the app: BPLM total=20 and tag response expected."),
]

COL1_X = 48;  COL2_X = 490
for i, (num, desc) in enumerate(steps):
    xi = COL1_X if i < 4 else COL2_X
    yi = INST_Y + 50 + (i % 4) * 44

    # circle badge
    cr.arc(xi+12, yi, 13, 0, 2*math.pi)
    cr.set_source_rgb(*bg(30,60,120)); cr.fill()
    text(cr, num, xi+12, yi+5, size=12, bold=True, color=(1,1,1))

    # description
    text(cr, desc, xi+32, yi+5, size=11, color=bg(30,30,50), align='left')

# ─── value table ──────────────────────────────────────────────────────────────
TBL_X = 20; TBL_Y = INST_Y + 216
rounded_rect(cr, TBL_X, TBL_Y, W-40, 28, r=6,
             fill=bg(225,235,255), stroke=bg(150,170,210), lw=1)
cols = [
    ("Recommended R", "15–22 Ω, ¼ W"),
    ("Package", "0402 or 0603 SMD"),
    ("Effect", "τ: 1000 µs → ~200 µs"),
    ("Trade-off", "Read range −20–40 %"),
]
cx = TBL_X + 20
for label, val in cols:
    text(cr, f"{label}:  ", cx, TBL_Y+19, size=10, bold=True,
         color=bg(50,70,150), align='left')
    ext = cr.text_extents(f"{label}:  ")
    text(cr, val, cx + ext.width + 2, TBL_Y+19, size=10,
         color=bg(30,30,30), align='left')
    cx += (W-40)//4

# ── save ─────────────────────────────────────────────────────────────────────
surface.write_to_png("/home/user/hitag2_flipper/hw_fix.png")
print("Saved hw_fix.png")
