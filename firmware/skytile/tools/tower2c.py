# Crop the rendered tower, shrink to icon size, emit an LVGL 9 ARGB8888 image.
import sys
from PIL import Image
src, out, H = sys.argv[1], sys.argv[2], int(sys.argv[3])
im = Image.open(src).convert('RGBA')
im = im.crop(im.getchannel('A').getbbox())
W = max(1, round(im.width * H / im.height))
im = im.resize((W, H), Image.LANCZOS)
im.save(out.replace('.h', '_preview.png'))
px = im.load()
data = []
for y in range(H):
    for x in range(W):
        r, g, b, a = px[x, y]
        data += [b, g, r, a]  # LVGL ARGB8888 is stored B, G, R, A
with open(out, 'w') as f:
    f.write('// Airport control tower icon, generated from airport-control-tower.svg by tower2c.py.\n#pragma once\n#include <lvgl.h>\n\n')
    f.write('static const uint8_t TOWER_PX[] = {\n')
    for i in range(0, len(data), 16):
        f.write('  ' + ', '.join('0x%02x' % v for v in data[i:i + 16]) + ',\n')
    f.write('};\n\n')
    f.write('static const lv_image_dsc_t TOWER_IMG = {\n')
    f.write('    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .flags = 0, .w = %d, .h = %d, .stride = %d, .reserved_2 = 0},\n' % (W, H, W * 4))
    f.write('    .data_size = sizeof(TOWER_PX),\n    .data = TOWER_PX,\n    .reserved = NULL,\n};\n')
print('icon', W, 'x', H)
