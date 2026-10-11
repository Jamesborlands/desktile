# Minimal SVG path rasterizer (M/L/H/V/C/S/Z, abs+rel, solid fills) -> transparent PNG.
import re, sys
from PIL import Image, ImageDraw
src, out, SCALE = sys.argv[1], sys.argv[2], float(sys.argv[3])
svg = open(src).read()
vx, vy, vw, vh = map(float, re.search(r'viewBox="([^"]+)"', svg).group(1).split())
img = Image.new('RGBA', (round(vw * SCALE), round(vh * SCALE)), (0, 0, 0, 0))
dr = ImageDraw.Draw(img)
def bez(p0, p1, p2, p3, n=16):
    return [((1-t)**3*p0[0]+3*(1-t)**2*t*p1[0]+3*(1-t)*t*t*p2[0]+t**3*p3[0],
             (1-t)**3*p0[1]+3*(1-t)**2*t*p1[1]+3*(1-t)*t*t*p2[1]+t**3*p3[1]) for t in [i/n for i in range(1, n+1)]]
for d, fill, rest in re.findall(r'<path d="([^"]+)" fill="([^"]+)"([^>]*)>', svg):
    om = re.search(r'opacity="([^"]+)"', rest)
    alpha = round(255 * float(om.group(1))) if om else 255
    toks = re.findall(r'[a-zA-Z]|-?\d*\.?\d+(?:e-?\d+)?', d)
    i, cmd, cur, start, last_c2, polys, pts = 0, None, (0, 0), (0, 0), None, [], []
    def num():
        global i
        v = float(toks[i]); i += 1; return v
    while i < len(toks):
        if toks[i].isalpha(): cmd = toks[i]; i += 1
        rel = cmd.islower(); c = cmd.lower()
        ox, oy = cur if rel else (0, 0)
        if c == 'm':
            if pts: polys.append(pts)
            cur = (ox + num(), oy + num()); start = cur; pts = [cur]; cmd = 'l' if rel else 'L'
        elif c == 'l': cur = (ox + num(), oy + num()); pts.append(cur)
        elif c == 'h': cur = ((cur[0] if rel else 0) + num(), cur[1]); pts.append(cur)
        elif c == 'v': cur = (cur[0], (cur[1] if rel else 0) + num()); pts.append(cur)
        elif c == 'c':
            p1 = (ox + num(), oy + num()); p2 = (ox + num(), oy + num()); p3 = (ox + num(), oy + num())
            pts += bez(cur, p1, p2, p3); last_c2 = p2; cur = p3
        elif c == 's':
            p1 = (2*cur[0]-last_c2[0], 2*cur[1]-last_c2[1]) if last_c2 else cur
            p2 = (ox + num(), oy + num()); p3 = (ox + num(), oy + num())
            pts += bez(cur, p1, p2, p3); last_c2 = p2; cur = p3
        elif c == 'z':
            cur = start
            if pts: polys.append(pts); pts = []
        if c not in 'cs': last_c2 = None
    if pts: polys.append(pts)
    col = fill if fill.startswith('#') else '#000'
    if len(col) == 4: col = '#' + ''.join(ch * 2 for ch in col[1:])
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    rgb = tuple(int(col[i:i + 2], 16) for i in (1, 3, 5))
    for poly in polys:
        if len(poly) > 2: ld.polygon([((x - vx) * SCALE, (y - vy) * SCALE) for x, y in poly], fill=rgb + (alpha,))
    img.alpha_composite(layer)
img.save(out)
print(img.size)
