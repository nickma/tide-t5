#!/usr/bin/env python3
"""Parametric two-part case for the LilyGo T5 4.7" e-paper with the 18650 holder.

    uv run --with manifold3d --with numpy case/case.py

Writes case/tray.stl and case/cover.stl, both already in print orientation.

Shape: a wedge. The board drops face-down into the tray (the bezel). The back is thin at the
top edge, where the buttons are, and deep at the bottom edge, where the 18650 sits, so the
case stands on its bottom edge and the cover is a flat plate that prints without supports.

Details borrowed from 51m0 Lab's slab case for the same board (makerworld.com/en/models/615564,
which only fits a flat LiPo): five flexing paddles in the top edge that press the side-facing
buttons, a closed USB-C opening in the bottom edge, and a back that locates on corner posts.
Positions along the edges were scaled from that model's photo of the board on a cutting mat.

All coordinates are in millimetres, looking at the screen: x to the right, y up, origin at
the bottom-left corner of the circuit board, z from the front of the bezel towards the back.

EVERY NUMBER MARKED "measure" IS AN ESTIMATE. Check them against the board before printing.
"""
import math
import struct
from pathlib import Path

import numpy as np
from manifold3d import CrossSection, JoinType, Manifold

# ----------------------------------------------------------------- board ----
PCB_W = 122.0          # measure: board width (long edge)
PCB_H = 65.0           # measure: board height (short edge)
PCB_T = 1.6            # board thickness

PANEL_W, PANEL_H, PANEL_T = 113.9, 62.5, 1.1   # e-paper glass (datasheet) plus adhesive
PANEL_X0 = 0.6         # measure: gap from the board's left edge to the glass
PANEL_Y0 = (PCB_H - PANEL_H) / 2               # measure: gap from the board's bottom edge

# Visible window, relative to the glass. Picture area is 103.7 x 58.3 mm inside the glass.
WIN_LEFT, WIN_RIGHT = 1.5, 109.5   # measure: window spans this range of the glass width
WIN_MARGIN_Y = 1.2                 # bezel overlap on the glass top and bottom

# Components on the back of the board.
CLEAR_TOP = 7.0        # measure: tallest part near the top edge (JST sockets, module)
CELL_Y = 12.0          # measure: 18650 centre line, up from the board's bottom edge
CELL_H = 10.5          # measure: 18650 centre line, above the back of the board
CELL_R = 9.3           # 18650 radius

USB_X = 9.7            # measure: USB-C centre, from the board's left edge (screen side view)
USB_Z = 1.7            # measure: USB-C centre, above the back of the board
USB_W, USB_H = 12.6, 6.8   # opening: clears a slim cable's plug shell

# Five side-facing buttons on the top edge; centres from the board's left edge (screen side view).
BTN_X = [34.0, 41.0, 48.0, 55.0, 62.0]   # measure
BTN_Z = 1.3            # measure: button plunger centre, above the back of the board
PADDLE_W = 5.0         # width of each flexing paddle
PADDLE_T = 1.0         # paddle thickness (the wall is thinned from outside)
SLIT = 0.5             # gap around each paddle
NUB = 0.8              # how far the paddle's bump reaches in past the wall

# ------------------------------------------------------------------ case ----
WALL = 2.0             # side walls
BEZEL = 1.6            # front face thickness
PLATE = 2.0            # back cover thickness
FIT = 0.3              # gap between board and walls, each side
LIP = 2.5              # depth of the cover's locating lip
LIP_T = 1.4
LIP_FIT = 0.15         # gap between lip and walls; raise if the cover is too tight
CORNER_R = 3.0
POSTS = True           # corner posts on the cover that hold the board against the bezel
POST = 3.5

# --------------------------------------------------------------- derived ----
Z_PANEL = BEZEL                    # front of the glass
Z_PCB = Z_PANEL + PANEL_T          # front of the board
Z_BACK = Z_PCB + PCB_T             # back of the board
X0, X1 = -FIT - WALL, PCB_W + FIT + WALL       # outer footprint
Y0, Y1 = -FIT - WALL, PCB_H + FIT + WALL


def solve_slope():
    """Underside of the cover is the plane z = zb - (y - Y0) * t. Find zb and t so it
    clears the top-edge parts by CLEAR_TOP and the 18650 by 0.6 mm."""
    z_top = Z_BACK + CLEAR_TOP
    zb = z_top + 10
    for _ in range(60):
        t = (zb - z_top) / (Y1 - Y0)
        need = Z_BACK + CELL_H + (CELL_R + 0.6) * math.sqrt(1 + t * t) + (CELL_Y - Y0) * t
        zb = need
    return zb, (zb - z_top) / (Y1 - Y0)


ZB, TAN = solve_slope()
THETA = math.atan(TAN)
DZ = PLATE / math.cos(THETA)       # cover thickness measured along z


def plane_z(y):
    return ZB - (y - Y0) * TAN


def rect(x0, y0, x1, y1, r=0.0):
    if r > 0:
        cs = CrossSection.square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]).translate([x0 + r, y0 + r])
        return cs.offset(r, JoinType.Round, 2.0, 48)
    return CrossSection.square([x1 - x0, y1 - y0]).translate([x0, y0])


def box(x0, y0, x1, y1, z0, z1, r=0.0):
    return Manifold.extrude(rect(x0, y0, x1, y1, r), z1 - z0).translate([0, 0, z0])


def yz_prism(points, x0, x1):
    """Extrude a polygon given in (y, z) along x from x0 to x1."""
    solid = Manifold.extrude(CrossSection([points]), x1 - x0)
    # extrude space (a, b, c) -> model (x = c, y = a, z = b)
    m = np.array([[0, 0, 1, x0], [1, 0, 0, 0], [0, 1, 0, 0]], dtype=float)
    return solid.transform(m)


def slab(z_below, z_above):
    """Everything between the sloped plane shifted by z_below and by z_above."""
    e = 5.0
    ya, yb = Y0 - e, Y1 + e
    pts = [(ya, plane_z(ya) + z_below), (yb, plane_z(yb) + z_below),
           (yb, plane_z(yb) + z_above), (ya, plane_z(ya) + z_above)]
    return yz_prism(pts, X0 - e, X1 + e)


def usb_hole():
    """Rounded opening through the bottom wall, centred on the connector."""
    zc = Z_BACK + USB_Z
    cs = rect(USB_X - USB_W / 2, zc - USB_H / 2, USB_X + USB_W / 2, zc + USB_H / 2, USB_H / 2 - 0.01)
    # extrude space (a, b, c) -> model (x = a, y = -c, z = b), so it runs through the wall
    m = np.array([[1, 0, 0, 0], [0, 0, -1, 0.5], [0, 1, 0, 0]], dtype=float)
    return Manifold.extrude(cs, WALL + FIT + 1.0).transform(m)


def usb_relief():
    """Space the cover's lip and post must leave around the connector and plug."""
    return box(USB_X - USB_W / 2 - 1, -FIT - 0.1, USB_X + USB_W / 2 + 1, 6.0,
               Z_BACK - 1, Z_BACK + USB_Z + USB_H / 2 + 1.5)


def paddles():
    """Returns (cut, add): slits and thinning to cut from the top wall, bumps to add back."""
    y_in, y_out = PCB_H + FIT, PCB_H + FIT + WALL
    z_hinge = Z_PCB                                # fixed end, by the bezel
    z_free = min(Z_BACK + BTN_Z + 4.5, plane_z(Y1) - 1.4)   # free end, towards the back
    zc = Z_BACK + BTN_Z
    cut, add = Manifold(), Manifold()
    for bx in BTN_X:
        a, b = bx - PADDLE_W / 2, bx + PADDLE_W / 2
        for sx in (a - SLIT, b):                                   # side slits
            cut += box(sx, y_in - 0.1, sx + SLIT, y_out + 0.1, z_hinge, z_free + SLIT)
        cut += box(a - SLIT, y_in - 0.1, b + SLIT, y_out + 0.1, z_free, z_free + SLIT)   # free end
        cut += box(a - SLIT, y_in + PADDLE_T, b + SLIT, y_out + 0.1, z_hinge, z_free + SLIT)  # thin it
        # Bump with a 45-degree underside so it prints without support (bezel is on the bed).
        prof = [(y_in + 0.05, zc - 1.2 - NUB), (y_in - NUB, zc - 1.2), (y_in - NUB, zc + 1.2),
                (y_in + 0.05, zc + 1.2)]
        add += yz_prism(prof[::-1], bx - 1.6, bx + 1.6)
    return cut, add


def tray():
    top = ZB + 1
    body = box(X0, Y0, X1, Y1, 0, top, CORNER_R)
    cavity = box(-FIT, -FIT, PCB_W + FIT, PCB_H + FIT, Z_PCB, top + 1, 0.6)
    panel = box(PANEL_X0 - FIT, PANEL_Y0 - FIT, PANEL_X0 + PANEL_W + FIT,
                PANEL_Y0 + PANEL_H + FIT, Z_PANEL, Z_PCB + 0.01)
    window = box(PANEL_X0 + WIN_LEFT, PANEL_Y0 + WIN_MARGIN_Y, PANEL_X0 + WIN_RIGHT,
                 PANEL_Y0 + PANEL_H - WIN_MARGIN_Y, -1, Z_PANEL + 0.01, 1.0)
    cut, bumps = paddles()
    return body - cavity - panel - window - usb_hole() - cut + bumps - slab(0, 100)


def cover():
    plate = box(X0, Y0, X1, Y1, 0, ZB + 20, CORNER_R) ^ slab(0, DZ)
    g = FIT - LIP_FIT   # lip sits just inside the tray walls
    ring = box(-g, -g, PCB_W + g, PCB_H + g, 0, ZB + 20, 0.6) - \
        box(-g + LIP_T, -g + LIP_T, PCB_W + g - LIP_T, PCB_H + g - LIP_T, -1, ZB + 21)
    lip = (ring ^ slab(-LIP / math.cos(THETA), 0.01)) - usb_relief()
    part = plate + lip
    if POSTS:
        for px in (0.2, PCB_W - POST - 0.2):
            for py in (0.2, PCB_H - POST - 0.2):
                post = box(px, py, px + POST, py + POST, Z_BACK + 0.2, ZB + 20)
                part += post ^ slab(-100, 0.01)
        part -= usb_relief()
    return part


def to_print_orientation(c):
    """Lay the cover flat with its outer face on the bed."""
    c = c.rotate([math.degrees(THETA), 0, 0])   # make the plate level
    c = c.rotate([0, 180, 0])                    # outer face down
    lo = c.bounding_box()
    return c.translate([-lo[0], -lo[1], -lo[2]])


def write_stl(solid, path):
    mesh = solid.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    tris = v[np.asarray(mesh.tri_verts)]
    with open(path, "wb") as f:
        f.write(b"tide-t5 case".ljust(80, b"\0"))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            n = np.cross(t[1] - t[0], t[2] - t[0])
            n = n / (np.linalg.norm(n) or 1)
            f.write(struct.pack("<12fH", *n, *t.reshape(-1), 0))


def main():
    out = Path(__file__).parent
    t, c = tray(), to_print_orientation(cover())
    write_stl(t, out / "tray.stl")
    write_stl(c, out / "cover.stl")
    for name, s in (("tray", t), ("cover", c)):
        b = s.bounding_box()
        size = [round(b[i + 3] - b[i], 1) for i in range(3)]
        print(f"{name}: {size} mm, {s.volume() / 1000:.1f} cm3, pieces: {len(s.decompose())}")
    print(f"back slope {math.degrees(THETA):.1f} deg; depth {plane_z(Y1) + DZ:.1f} mm at the top edge, "
          f"{ZB + DZ:.1f} mm at the bottom edge")


if __name__ == "__main__":
    main()
