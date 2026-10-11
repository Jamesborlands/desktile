"""
SkyTile desk case for the Elecrow CrowPanel Advance 5" (ESP32-S3, 800x480).

Two printed parts plus one aluminum part:
  skytile_shell.stl     wedge body with a thin front wall, print face-down (no supports)
  skytile_back.stl      back cover, print flat
  skytile_faceplate.dxf 0.063" (1.6 mm) aluminum faceplate, laser cut + anodized (SendCutSend)
The faceplate (on 0.64 mm VHB tape) drops into a 2.24 mm deep pocket in the front of the body, framed by a 1.2 mm
printed rim, so it sits flush and cannot slide. Small crush ribs on the rim's inner wall give
it a press-in fit. It is held by 3M VHB tape and clamped by the two M7 knob nuts.

Coordinates (shell local): X = width (left to right, viewer's view),
Y = up along the screen face, Z = depth (0 at the front face, + toward the back).
The face leans back TILT degrees when the base sits on the desk.

Panel numbers come from Elecrow's own STEP model (v1.2, "without shell" assembly):
PCB 132.0 x 80.7, LCD module 120.7 x 76.3 x 5.7 sitting on the PCB front,
back-side parts up to 9.0 mm tall, mounting holes on a 126.0 x 74.3 grid.
"""
import math
import cadquery as cq

# ---------------- parameters ----------------
TILT = 15.0          # degrees the face leans back
WALL = 2.4           # side, top and base wall
FRONT = 1.6          # printed front wall behind the aluminum faceplate
PLATE_T = 1.6        # faceplate thickness (0.063 in 5052 aluminum)
TAPE = 0.64          # 3M VHB 5925 tape behind the plate (0.025 in)
POCKET = PLATE_T + TAPE  # pocket depth so the plate ends flush with the rim
CORNER_R = 4.0       # front outline corner radius (body and faceplate match)
COVER = 2.4          # back cover thickness
CLR = 0.4            # clearance around the PCB
RIM = 1.2            # printed rim around the faceplate pocket
PLATE_GAP = 0.15     # gap between the plate edge and the rim, per side
RIB = 0.35           # crush rib protrusion into the pocket (0.2 mm interference)

PCB_W, PCB_H = 132.0, 80.7
PCB_YMIN = -0.4                    # PCB y range in the STEP is -0.4 .. 80.3
LCD_T = 5.7                        # LCD module thickness (front of PCB to glass)
LCD_CX, LCD_CY = 66.25, 40.15      # LCD module centre in PCB coords
WIN_W, WIN_H = 111.0, 68.0         # active area is 108 x 65; 1.5 mm margin each side
HOLES = [(3.0, 3.0), (129.0, 3.0), (3.0, 77.3), (129.0, 77.3)]  # PCB mounting holes
USB_C = (37.5, 6.25)               # vertical USB-C receptacle centre (PCB coords)

KNOB_ZONE = 34.0                   # width of the knob column right of the screen
KNOB_HOLE = 7.6                    # PEC11 M7 bushing, printed wall
PLATE_KNOB_HOLE = 7.3              # M7 bushing, aluminum faceplate
KNOB_GAP = 42.0                    # centre-to-centre distance of the two knobs

BASE_DEPTH = 56.0                  # depth of the base along the shell Z axis
CHIN = 6.0                         # extra bezel under the screen so the sloped base clears the PCB

# derived
# The viewer looks along +Z, so small X is on the viewer's RIGHT.
# Knob column goes at small X (viewer's right), screen at larger X.
OX = WALL + KNOB_ZONE + CLR        # PCB x=0 in shell coords
OY = WALL + CHIN + CLR - PCB_YMIN  # PCB y=0 in shell coords
W = WALL + CLR + PCB_W + CLR + KNOB_ZONE + WALL
H = WALL + CHIN + CLR + PCB_H + CLR + WALL
t = math.tan(math.radians(TILT))
YB = BASE_DEPTH * t                         # where base meets back face
ZTOP = BASE_DEPTH - (H - YB) * t            # depth at the top of the back face

KNOB_X = WALL + KNOB_ZONE / 2
KNOB_Y = [OY + LCD_CY - KNOB_GAP / 2, OY + LCD_CY + KNOB_GAP / 2]

# back plane: passes through (YB, BASE_DEPTH) and (H, ZTOP); outward normal n (in YZ)
dy, dz = H - YB, ZTOP - BASE_DEPTH
L = math.hypot(dy, dz)
uy, uz = dy / L, dz / L            # unit vector up the back face
ny, nz = -uz, uy                   # outward normal (points back and slightly up)


def plane_dist(y, z):
    """signed distance from the back plane, + is outside"""
    return (y - YB) * ny + (z - BASE_DEPTH) * nz


def prism(pts, x0, x1):
    """extrude a (Y,Z) polygon along X from x0 to x1"""
    wp = cq.Workplane("YZ", origin=(x0, 0, 0)).polyline(pts).close()
    return wp.extrude(x1 - x0)


# ---------------- outer body ----------------
far = 40.0
big = 600


def halfspace(point, normal):
    """solid occupying the side of the plane where (p - point) . normal > 0"""
    nx_, ny_, nz_ = normal
    L_ = math.sqrt(nx_ * nx_ + ny_ * ny_ + nz_ * nz_)
    nvec = cq.Vector(nx_ / L_, ny_ / L_, nz_ / L_)
    b = cq.Workplane("XY").box(big, big, big, centered=(True, False, True)).val()  # +y side
    rot_axis = cq.Vector(0, 1, 0).cross(nvec)
    ang = math.degrees(math.acos(max(-1.0, min(1.0, nvec.y))))
    if rot_axis.Length > 1e-9:
        b = b.rotate(cq.Vector(0, 0, 0), rot_axis, ang)
    elif nvec.y < 0:
        b = b.rotate(cq.Vector(0, 0, 0), cq.Vector(1, 0, 0), 180)
    return cq.Workplane("XY").add(b.translate(cq.Vector(*point)))


def halfspace_beyond(offset):
    """solid occupying plane_dist > offset (outside the back plane, shifted by offset)"""
    py = YB + ny * offset
    pz = BASE_DEPTH + nz * offset
    return halfspace((W / 2, py, pz), (0, ny, nz))


front_rr = (cq.Workplane("XY").center(W / 2, H / 2).rect(W, H).extrude(BASE_DEPTH + far)
            .edges("|Z").fillet(CORNER_R))
below_base = halfspace((0, 0, 0), (0, -1.0, t))          # y - z*t < 0
outer = front_rr.cut(below_base).cut(halfspace_beyond(0.0))

# ---------------- cavity ----------------
# inset: front WALL, top WALL, base WALL (perpendicular), open toward the back
c = math.cos(math.radians(TILT))
base_off = WALL / c   # vertical shift of the base line for a perpendicular wall of WALL
cav_pts = [
    (base_off, WALL),                    # on the inset base line at the front plate
    (H - WALL, WALL),
    (H - WALL, ZTOP + far),
    ((BASE_DEPTH + far) * t + base_off, BASE_DEPTH + far),
]
# the inset base line is y = z*t + base_off; first point y must sit on it at z = WALL
cav_pts[0] = (FRONT * t + base_off, FRONT)
cav_pts[1] = (H - WALL, FRONT)
cavity = prism(cav_pts, WALL, W - WALL)

shell = outer.cut(cavity)

# ---------------- panel bosses ----------------
for hx, hy in HOLES:
    cx, cy = OX + hx, OY + hy
    boss = (cq.Workplane("XY").workplane(offset=FRONT)
            .center(cx, cy).circle(2.3).extrude(LCD_T + 0.1))
    shell = shell.union(boss)
    pilot = (cq.Workplane("XY").workplane(offset=FRONT - 0.01)
             .center(cx, cy).circle(1.25).extrude(LCD_T + 0.2))
    shell = shell.cut(pilot)

# ---------------- screen window (chamfered from the front) ----------------
wx, wy = OX + LCD_CX, OY + LCD_CY
win = (cq.Workplane("XY").workplane(offset=-1)
       .center(wx, wy).rect(WIN_W, WIN_H).extrude(FRONT + 2))
shell = shell.cut(win)

# ---------------- knob holes ----------------
for ky in KNOB_Y:
    h = (cq.Workplane("XY").workplane(offset=-1)
         .center(KNOB_X, ky).circle(KNOB_HOLE / 2).extrude(FRONT + 2))
    shell = shell.cut(h)

# ---------------- cover screw blocks (printable 45 deg underside) ----------------
BLOCK = 8.0        # how far a block sticks out from the side wall
BAND = 9.0         # block height along Y
screw_pts = []     # (x, y, z) on the inner face of the cover, for the cover holes


def back_z(y, offset):
    """z of the plane at plane_dist == offset, for a given y"""
    # (y-YB)*ny + (z-BASE_DEPTH)*nz = offset
    return BASE_DEPTH + (offset - (y - YB) * ny) / nz


for y_c in (YB + 9.0, H - WALL - BAND / 2 - 2.0):
    y0, y1 = y_c - BAND / 2, y_c + BAND / 2
    z_top = back_z(y1, -COVER)           # lowest z of the inner cover face across the band
    z1 = z_top - 14.0
    for side in ("L", "R"):
        if side == "L":
            xw, xi = WALL - 0.5, WALL + BLOCK
            sx = WALL + 4.0
        else:
            xw, xi = W - WALL + 0.5, W - WALL - BLOCK
            sx = W - WALL - 4.0
        # XZ profile: wall edge, 45 deg slope out to xi, then straight up past the plane
        prof = [(xw, z1), (xi, z1 + abs(xi - xw)), (xi, z1 + 60), (xw, z1 + 60)]
        blk = (cq.Workplane("XZ", origin=(0, y0, 0)).polyline(prof).close()
               .extrude(-BAND))
        blk = blk.cut(halfspace_beyond(-COVER))
        shell = shell.union(blk)
        # pilot hole along the plane normal
        py = y_c
        pz = back_z(py, -COVER)
        screw_pts.append((sx, py, pz))
        hole = (cq.Workplane(cq.Plane(origin=(sx, py, pz), xDir=(1, 0, 0),
                                      normal=(0, -ny, -nz)))
                .circle(1.25).extrude(12.0))
        shell = shell.cut(hole)

# back cut (cover space)
shell = shell.cut(halfspace_beyond(-COVER))

# ---------------- back cover ----------------
cover = outer.intersect(halfspace_beyond(-COVER))
for (sx, py, pz) in screw_pts:
    pl = cq.Plane(origin=(sx, py, pz), xDir=(1, 0, 0), normal=(0, ny, nz))
    thru = cq.Workplane(pl).workplane(offset=-1).circle(1.7).extrude(COVER + 2)
    cbore = cq.Workplane(pl).workplane(offset=COVER - 1.4).circle(3.2).extrude(3)
    cover = cover.cut(thru).cut(cbore)

# cable slot, lined up with the USB-C port. The port faces straight back and a straight
# plug would hit the sloped base, so the cable uses a right-angle plug pointed UP; the
# slot sits where that cable comes back out through the cover.
ux = OX + USB_C[0]
uy_c = 33.0
pl = cq.Plane(origin=(ux, uy_c, back_z(uy_c, -COVER)), xDir=(1, 0, 0), normal=(0, ny, nz))
slot = cq.Workplane(pl).workplane(offset=-1).slot2D(20.0, 10.0).extrude(COVER + 2)
cover = cover.cut(slot)

# vent slots up high
for k in range(5):
    s = L * 0.55 + k * 4.5          # distance up the face
    vy = YB + s * uy
    vz = BASE_DEPTH + s * uz
    p = cq.Plane(origin=(OX + 66.0, vy - COVER * ny, vz - COVER * nz), xDir=(1, 0, 0),
                 normal=(0, ny, nz))
    v = cq.Workplane(p).workplane(offset=-1).slot2D(60.0, 2.4).extrude(COVER + 2)
    cover = cover.cut(v)

# ---------------- faceplate pocket: printed rim in front of z = 0 ----------------
PW, PH = W - 2 * (RIM + PLATE_GAP), H - 2 * (RIM + PLATE_GAP)   # plate outline
PR = CORNER_R - RIM - PLATE_GAP
rim_outer = (cq.Workplane("XY").workplane(offset=-POCKET).center(W / 2, H / 2).rect(W, H)
             .extrude(POCKET).edges("|Z").fillet(CORNER_R))
rim_inner = (cq.Workplane("XY").workplane(offset=-POCKET - 1).center(W / 2, H / 2)
             .rect(W - 2 * RIM, H - 2 * RIM).extrude(POCKET + 2).edges("|Z").fillet(CORNER_R - RIM))
rim = rim_outer.cut(rim_inner)
# crush ribs: half cylinders on the rim's inner wall, running the depth of the pocket
rib_r = 0.5
ix0, ix1, iy0, iy1 = RIM, W - RIM, RIM, H - RIM
rib_pts = ([(x, iy0 - rib_r + RIB) for x in (W * 0.25, W * 0.75)] +
           [(x, iy1 + rib_r - RIB) for x in (W * 0.25, W * 0.75)] +
           [(ix0 - rib_r + RIB, H / 2), (ix1 + rib_r - RIB, H / 2)])
for rx, ry in rib_pts:
    rib = (cq.Workplane("XY").workplane(offset=-POCKET + 0.4).center(rx, ry).circle(rib_r)
           .extrude(POCKET - 0.4))          # stops 0.4 mm short of the face: easy lead-in
    rim = rim.union(rib.intersect(rim_outer))
shell = shell.union(rim)

# ---------------- aluminum faceplate (in the pocket on its tape, z = -POCKET .. -TAPE) ----------------
faceplate = (cq.Workplane("XY").workplane(offset=-POCKET).center(W / 2, H / 2).rect(PW, PH)
             .extrude(PLATE_T).edges("|Z").fillet(PR))
faceplate = faceplate.cut(cq.Workplane("XY").workplane(offset=-POCKET - 1).center(wx, wy)
                          .rect(WIN_W, WIN_H).extrude(PLATE_T + 2).edges("|Z").fillet(1.0))
for ky in KNOB_Y:
    faceplate = faceplate.cut(cq.Workplane("XY").workplane(offset=-POCKET - 1).center(KNOB_X, ky)
                              .circle(PLATE_KNOB_HOLE / 2).extrude(PLATE_T + 2))


def write_faceplate_dxf(path):
    """Front view as seen by the viewer (shell x is mirrored), units mm, origin bottom-left."""
    import ezdxf
    doc = ezdxf.new("R2010", setup=True)
    doc.units = ezdxf.units.MM
    msp = doc.modelspace()

    def rrect(cx, cy, w, h, r):
        x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2
        pts = [(x0 + r, y0, 0), (x1 - r, y0, 0), (x1, y0 + r, 0), (x1, y1 - r, 0),
               (x1 - r, y1, 0), (x0 + r, y1, 0), (x0, y1 - r, 0), (x0, y0 + r, 0)]
        # bulge 0.4142 = tan(90deg/4) for a quarter arc
        b = math.tan(math.radians(90) / 4)
        poly = [(pts[0][0], pts[0][1], 0, 0, 0), (pts[1][0], pts[1][1], 0, 0, b),
                (pts[2][0], pts[2][1], 0, 0, 0), (pts[3][0], pts[3][1], 0, 0, b),
                (pts[4][0], pts[4][1], 0, 0, 0), (pts[5][0], pts[5][1], 0, 0, b),
                (pts[6][0], pts[6][1], 0, 0, 0), (pts[7][0], pts[7][1], 0, 0, b)]
        msp.add_lwpolyline(poly, format="xyseb", close=True)

    rrect(W / 2, H / 2, PW, PH, PR)
    rrect(W - wx, wy, WIN_W, WIN_H, 1.0)
    for ky in KNOB_Y:
        msp.add_circle((W - KNOB_X, ky), PLATE_KNOB_HOLE / 2)
    doc.saveas(path)


if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    cq.exporters.export(shell, f"{out}/skytile_shell.stl", tolerance=0.05, angularTolerance=0.1)
    cq.exporters.export(cover, f"{out}/skytile_back.stl", tolerance=0.05, angularTolerance=0.1)
    cq.exporters.export(shell, f"{out}/skytile_shell.step")
    cq.exporters.export(cover, f"{out}/skytile_back.step")
    cq.exporters.export(faceplate, f"{out}/skytile_faceplate.step")
    write_faceplate_dxf(f"{out}/skytile_faceplate.dxf")
    sb = shell.val().BoundingBox()
    print(f"W={W:.1f} H={H:.1f} base_depth={BASE_DEPTH} top_depth={ZTOP:.1f}")
    print("shell bbox", round(sb.xlen, 1), round(sb.ylen, 1), round(sb.zlen, 1))
    print("shell valid", shell.val().isValid(), "vol", round(shell.val().Volume()))
    print("cover valid", cover.val().isValid(), "vol", round(cover.val().Volume()))
    print("plate", round(PW, 2), "x", round(PH, 2), "r", PR, "| in", round(PW / 25.4, 3), "x", round(PH / 25.4, 3))
    print("knobs at", round(KNOB_X, 1), [round(k, 1) for k in KNOB_Y])
