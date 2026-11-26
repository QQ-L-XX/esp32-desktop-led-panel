"""Subset Fusion Pixel 8px zh_Hans into a flash-resident common Chinese font."""
from pathlib import Path
from PIL import Image
import json

root = Path(__file__).resolve().parents[1]
(root / "work").mkdir(exist_ok=True)
(root / "docs/images").mkdir(parents=True, exist_ok=True)
source = root / 'font-tools/source-zh_hans.bdf'
wanted = set(range(32, 127))
for first in range(0xA1, 0xF8):
    for second in range(0xA1, 0xFF):
        try:
            wanted.add(ord(bytes([first, second]).decode('gb2312')))
        except UnicodeDecodeError:
            pass
for begin, end in [(0x2000, 0x2070), (0x3000, 0x3040), (0xFF00, 0xFFF0)]:
    wanted.update(range(begin, end))
glyphs = {}
for block in source.read_text('utf-8').split('STARTCHAR ')[1:]:
    lines = block.splitlines()
    code = int(next(line[9:] for line in lines if line.startswith('ENCODING ')))
    if code not in wanted:
        continue
    width, height, left, bottom = map(int, next(line[4:] for line in lines if line.startswith('BBX ')).split())
    top = 7 - bottom - height
    start = lines.index('BITMAP') + 1
    rows = [0] * 8
    for y in range(height):
        bitmap = int(lines[start + y], 16)
        bits = ((width + 7) // 8) * 8
        for x in range(width):
            if bitmap & (1 << (bits - x - 1)) and 0 <= y + top < 8 and 0 <= x + left < 8:
                rows[y + top] |= 0x80 >> (x + left)
    glyphs[code] = rows

defaults = ['物来顺应', '未来不迎', '当时不杂', '既往不恋']
assert all(ord(c) in glyphs for line in defaults for c in line)
header = '#pragma once\n// Fusion Pixel 8px zh_Hans 2026.11.10, SIL OFL 1.1. See FONT-LICENSES.\n'
header += 'struct ChineseGlyph { uint16_t character; uint8_t rows[8]; };\n'
header += 'const ChineseGlyph chineseGlyphs[] PROGMEM = {\n'
for code, rows in sorted(glyphs.items()):
    header += '  {0x%04x, {%s}},\n' % (code, ', '.join('0x%02x' % row for row in rows))
header += '};\n'
(root / 'firmware-source/src/chinese_font.h').write_text(header, 'utf-8')
(root / 'work/font-subset.json').write_text(json.dumps(glyphs), 'utf-8')

preview = Image.new('RGB', (32, 8 * 4 + 3 * 2), '#08100f')
colors = ['#00e6cf', '#ffbd3d', '#55acff', '#ed50d4']
for i, line in enumerate(defaults):
    for x, character in enumerate(line):
        for y, bitmap in enumerate(glyphs[ord(character)]):
            for column in range(8):
                if bitmap & (0x80 >> column):
                    preview.putpixel((x * 8 + column, i * 10 + y), tuple(bytes.fromhex(colors[i][1:])))
preview.resize((640, preview.height * 20), Image.Resampling.NEAREST).save(root / 'docs/images/phrases-preview.png')
print('font glyphs', len(glyphs), 'flash bytes', len(glyphs) * 10)
