"""ANCHOR v2 enclosure parts (CadQuery, parametric).

Run:  python anchor_cad.py      -> writes STEP and STL files to ./out

All sizes are in mm. Values marked PLACEHOLDER must be replaced with real measurements of the
earmuff and the speaker driver before printing.
"""
import os
import cadquery as cq

# ------------------------------------------------------------------ parameters
CUP_W = 60.0        # PLACEHOLDER: inner width of the ear cup
CUP_H = 75.0        # PLACEHOLDER: inner height of the ear cup
TRAY_GAP = 1.0      # clearance between trays and the cup wall
DRV_D = 40.4        # PLACEHOLDER: speaker driver outer diameter + 0.4 clearance
DRV_LIP_H = 6.0     # lip that holds the driver
WALL = 2.0
PLATE_T = 2.0
ERR_DIST = 7.0      # ERR mic distance in front of the driver face

MIC_W, MIC_H, MIC_T = 12.0, 13.0, 1.6    # mic board (matches pcb/mic)
MIC_PORT = (-2.0, -3.5)                  # ICS-43434 sound port, relative to mic board centre
FIT = 0.3                                # print clearance for boards in pockets

MAIN_W, MAIN_H = 51.0, 38.0              # main board (matches pcb/main)
MAIN_HOLES = [(47.8, 34.8), (47.8, 3.2)] # from the board's top-left corner
BATT = (34.0, 50.0, 5.0)                 # 503450 LiPo: width, length, thickness
STANDOFF_H = 8.0                         # board sits above the battery

HUB_W, HUB_H = 30.0, 22.0                # right cup hub (matches pcb/hub)
HUB_HOLES = [(3.2, 10.0), (26.8, 10.0)]

BAND_W, BAND_T = 30.0, 4.0               # PLACEHOLDER: headband width and thickness
CABLE_D = 4.0                            # 8-core cable outer diameter


def ellipse_plate(w, h, t):
    return cq.Workplane("XY").ellipse(w / 2, h / 2).extrude(t)


# ------------------------------------------------------------------ 1. speaker baffle with ERR mic clip
def speaker_baffle():
    plate = ellipse_plate(CUP_W - TRAY_GAP, CUP_H - TRAY_GAP, PLATE_T)
    lip = (cq.Workplane("XY").workplane(offset=PLATE_T)
           .circle(DRV_D / 2 + WALL).circle(DRV_D / 2).extrude(DRV_LIP_H))
    body = plate.union(lip).cut(cq.Workplane("XY").circle(DRV_D / 2).extrude(PLATE_T))
    # ear side is -Z. Two legs and a bridge hold the ERR mic board in front of the driver.
    leg_x = DRV_D / 2 + 2.5
    z_bridge = -ERR_DIST
    for sx in (-1, 1):
        leg = cq.Workplane("XY").box(3, 4, ERR_DIST + 1.5, centered=(True, True, False)).translate((sx * leg_x, 0, z_bridge - 1.5))
        body = body.union(leg)
    bridge = cq.Workplane("XY").box(2 * leg_x + 3, 3, 1.5, centered=(True, True, False)).translate((0, 0, z_bridge - 1.5))
    fw, fh = MIC_W + FIT + 2 * 1.2, MIC_H + FIT + 2 * 1.2
    frame = cq.Workplane("XY").box(fw, fh, MIC_T + 1.5, centered=(True, True, False)).translate((0, 0, z_bridge - 1.5))
    pocket = cq.Workplane("XY").box(MIC_W + FIT, MIC_H + FIT, MIC_T + 0.1, centered=(True, True, False)).translate((0, 0, z_bridge))
    port = cq.Workplane("XY").circle(2.0).extrude(3).translate((MIC_PORT[0], MIC_PORT[1], z_bridge - 2))
    cable = cq.Workplane("XY").box(8, 3, 4, centered=(True, True, False)).translate((0, fh / 2 - 1, z_bridge))
    body = body.union(bridge).union(frame).cut(pocket).cut(port).cut(cable)
    return body


# ------------------------------------------------------------------ 2. REF mic housing and grille cap
def ref_housing():
    iw, ih = MIC_W + FIT, MIC_H + FIT
    ow, oh = iw + 2 * 1.6, ih + 2 * 1.6
    base = cq.Workplane("XY").rect(ow + 6, oh + 6).extrude(1.5).edges("|Z").fillet(3)
    walls = cq.Workplane("XY").workplane(offset=1.5).rect(ow, oh).rect(iw, ih).extrude(7.0)
    ledge = cq.Workplane("XY").workplane(offset=1.5).rect(iw, ih).rect(iw - 2, ih - 2).extrude(1.0)
    body = base.union(walls).union(ledge)
    body = body.cut(cq.Workplane("XY").circle(3.0).extrude(2).translate((0, 0, -0.2)))  # cable hole into the cup
    return body


def ref_grille_cap():
    iw, ih = MIC_W + FIT, MIC_H + FIT
    ow, oh = iw + 2 * 1.6, ih + 2 * 1.6
    cap = cq.Workplane("XY").rect(ow, oh).extrude(1.2).edges("|Z").fillet(1)
    plug = cq.Workplane("XY").workplane(offset=-2.0).rect(iw - 0.2, ih - 0.2).rect(iw - 2.6, ih - 2.6).extrude(2.0)
    cap = cap.union(plug)
    holes = cq.Workplane("XY").rarray(2.2, 2.2, 4, 4).circle(0.7).extrude(3).translate((0, 0, -1))
    return cap.cut(holes)


# ------------------------------------------------------------------ 3. left cup tray: battery + main board
def left_tray():
    tray = ellipse_plate(CUP_W - TRAY_GAP, CUP_H - TRAY_GAP, 1.6)
    bw, bl, bt = BATT
    pocket = (cq.Workplane("XY").workplane(offset=1.6).rect(bw + 2 * 1.2 + 0.6, bl + 2 * 1.2 + 0.6)
              .rect(bw + 0.6, bl + 0.6).extrude(bt))
    tray = tray.union(pocket)
    # main board is turned 90 degrees so its long side runs along the cup's long axis
    def to_tray(x, y):
        dx, dy = x - MAIN_W / 2, y - MAIN_H / 2
        return (-dy, dx)
    for hx, hy in MAIN_HOLES:
        tx, ty = to_tray(hx, hy)
        post = cq.Workplane("XY").circle(2.4).circle(0.85).extrude(STANDOFF_H).translate((tx, ty, 1.6))
        tray = tray.union(post)
    # two plain supports with a lip on the other short edge of the board
    for sy in (-1, 1):
        sx, syy = to_tray(2.5, MAIN_H / 2 + sy * 14)
        sup = cq.Workplane("XY").box(4, 4, STANDOFF_H, centered=(True, True, False)).translate((sx, syy, 1.6))
        tray = tray.union(sup)
    return tray


# ------------------------------------------------------------------ 4. right cup tray: hub board
def right_tray():
    tray = ellipse_plate(CUP_W - TRAY_GAP, CUP_H - TRAY_GAP, 1.6)
    for hx, hy in HUB_HOLES:
        tx, ty = hx - HUB_W / 2, hy - HUB_H / 2
        tray = tray.union(cq.Workplane("XY").circle(2.4).circle(0.85).extrude(4.0).translate((tx, ty, 1.6)))
    return tray


# ------------------------------------------------------------------ 5. headband cable clip
def cable_clip():
    w, t, wall, depth = BAND_W + 0.4, BAND_T + 0.4, 1.6, 8.0
    outer = cq.Workplane("XY").rect(w + 2 * wall, t + 2 * wall).extrude(depth)
    inner = cq.Workplane("XY").rect(w, t).extrude(depth)
    slot = cq.Workplane("XY").rect(w - 8, wall * 2).extrude(depth).translate((0, -(t / 2 + wall / 2), 0))  # snap-on opening
    tube = (cq.Workplane("XY").circle(CABLE_D / 2 + 1.4).circle(CABLE_D / 2 + 0.2).extrude(depth)
            .translate((0, t / 2 + wall + CABLE_D / 2 + 0.6, 0)))
    tube_slot = cq.Workplane("XY").rect(CABLE_D * 0.8, 3).extrude(depth).translate((0, t / 2 + wall + CABLE_D + 1.2, 0))
    return outer.cut(inner).cut(slot).union(tube).cut(tube_slot)


PARTS = {
    "speaker_baffle": speaker_baffle,
    "ref_mic_housing": ref_housing,
    "ref_mic_grille_cap": ref_grille_cap,
    "left_cup_tray": left_tray,
    "right_cup_tray": right_tray,
    "headband_cable_clip": cable_clip,
}

if __name__ == "__main__":
    os.makedirs("out", exist_ok=True)
    for name, fn in PARTS.items():
        shape = fn()
        cq.exporters.export(shape, f"out/{name}.step")
        cq.exporters.export(shape, f"out/{name}.stl", tolerance=0.05, angularTolerance=0.2)
        bb = shape.val().BoundingBox()
        print(f"{name:22s} {bb.xlen:6.1f} x {bb.ylen:6.1f} x {bb.zlen:6.1f} mm  valid={shape.val().isValid()}")
