#pragma once
#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM  // 主机侧自检时不需要
#endif

// 1bit 点阵字库的公共结构。字模由 tools/mkfont.py 从 TTF 烘焙生成，
// 坐标以基线为参考：yoff 为字形顶部相对基线的偏移（向上为负）。

typedef struct {
  uint16_t cp;     // Unicode 码点
  uint8_t w;       // 位图宽度
  uint8_t h;       // 位图高度
  int8_t xoff;     // 位图左边相对笔位的偏移
  int8_t yoff;     // 位图顶部相对基线的偏移（负值＝基线之上）
  uint8_t adv;     // 前进宽度
  uint16_t off;    // 在位图数组中的字节偏移
} WkGlyph;

typedef struct {
  const WkGlyph *glyphs;
  const uint8_t *bits;
  uint8_t count;
  uint8_t size;    // 字号（像素），仅作参考
} WkFont;
