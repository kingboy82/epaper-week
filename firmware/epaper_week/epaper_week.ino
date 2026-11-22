// epaper-week · 7.5 寸三色墨水屏周历
// by kingboy82 · https://github.com/kingboy82/epaper-week
// MIT License

// 7.5 寸三色墨水屏 · 周视图固件
//
// 硬件：ESP32 + GDEW075Z08（800x480，黑/白/红）+ CH340
// 数据：由局域网内的 bridge 服务提供，设备只拉一段 JSON，不直接对接企微。
//
// 刷新策略：每 30 分钟醒来一次，拉数据后比对指纹，内容没变就不刷屏
// （墨水屏断电保留画面，跳过刷新对用户是无感的，但能省掉一次 20 秒的闪屏）。

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GxEPD2_3C.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "font_lat18.h"
#include "font_num64.h"
#include "font_txt16.h"
#include "keys.h"
#include "model.h"
#include "net_direct.h"
#include "wkfont.h"

// TLS 握手 + 证书校验在 loopTask 里跑，默认 8KB 栈不够用
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---------------------------------------------------------------- 引脚
// 取自仓库 README 的 GXEPD2 示例：CS=5 DC=19 RST=16 BUSY=17
// SPI: SCK=18 MOSI=23。MISO 未接（墨水屏只写不读），传 -1 避免与 DC 抢 19 号脚。
static const int PIN_EPD_CS = 5;
static const int PIN_EPD_DC = 19;
static const int PIN_EPD_RST = 16;
static const int PIN_EPD_BUSY = 17;
static const int PIN_SCK = 18;
static const int PIN_MOSI = 23;

GxEPD2_3C<GxEPD2_750c, GxEPD2_750c::HEIGHT / 2> display(
    GxEPD2_750c(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

U8G2_FOR_ADAFRUIT_GFX u8g2;
WebServer server(80);
Preferences prefs;

// ---------------------------------------------------------------- 版式常量
// 与 preview/week_view_v18_800x480.png 逐像素对应
// 面板是 640x384 的三色屏（原厂固件名里的 LowRes 指的就是这个分辨率）
static const int SCR_W = 640;
static const int SCR_H = 384;
static const int MARGIN = 10;   // 19→10：把四周留白收掉，内容再撑开一点
static const int RULE_Y = 76;
static const int ROW_TOP = 76;
static const int ROW_H = 43;    // 42→43，7 行刚好填到 377
static const int COL_TEXT_X = 18;
static const int COL_DIV_X = 56;
static const int EV_X = 64;
static const int WX_X = 566;
static const int WX_DIV_X = 557;
static const int EV_GAP = 14;
static const int LINE_W = WX_DIV_X - 14 - EV_X;
static const int MINI_COL_W = 18;   // 16→18：格子更大，数字更清楚
static const int MINI_ROW_H = 10;
static const int MINI_GAP = 12;
static const int MINI_LABEL_W = 36;  // 月份文字占位，放在框的左侧
// 再左移 3px：第二个月边框画在 x0-4、宽 gw+7，其右端正好落在 x=629，
// 与顶部电量条的右端（MARGIN + (SCR_W-2*MARGIN) - 1 = 629）平齐
static const int MINI_X =
    SCR_W - MARGIN - (2 * 7 * MINI_COL_W + MINI_LABEL_W + MINI_GAP) - 3;
static const int MINI_Y = 8;

static const char *MON3[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                             "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
static const char *WD_CN[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};

static WeekData g_week;
static uint8_t g_woff = 0;    // 0=本周 1=下周（3 号键切换，持久化到 NVS）
static bool g_infoShown = false;  // 当前屏幕是不是信息页
static bool g_bootEvent = false;  // 本次是否为重启（非定时/按键唤醒）

// 连按两次复位键进配网。
// 不用 RTC 内存：实测复位后 esp_reset_reason 报 POWERON，RTC 域被清，计数会归零。
// 改用 NVS 计数，只统计"非深度睡眠唤醒"的启动——定时唤醒不计数。
static bool g_forcePortal = false;

// ---------------------------------------------------------------- 运行日志
// 只记三类事件：开机、取数失败、刷屏。深睡定时唤醒不记（一天 48 次会写穿 flash）。
// 目的：夜里"没更新"时，能区分是断电（日志断档 + 新开机记录）、还是醒了但没取到数（fail 记录）。
// 最多保留 8 条，写 NVS 的 wk/jrn。
static void journal(const char *tag) {
  prefs.begin("wk", false);
  String j = prefs.getString("jrn", "");
  time_t now = time(nullptr);
  char line[48];
  if (now > 1700000000) {          // 没对时就写"--"，也算一条记录
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(line, sizeof(line), "%02d-%02d %02d:%02d %s\n", t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, tag);
  } else {
    snprintf(line, sizeof(line), "--  未对时 %s\n", tag);
  }
  j += line;
  int cnt = 0;
  for (unsigned i = 0; i < j.length(); i++)
    if (j[i] == '\n') cnt++;
  while (cnt > 8) {
    int p = j.indexOf('\n');
    if (p < 0) break;
    j.remove(0, p + 1);
    cnt--;
  }
  prefs.putString("jrn", j);
  prefs.end();
}

// ---------------------------------------------------------------- 配置
struct Config {
  char ssid[33];
  char pass[65];
  char bridge[128];
  char email[64];
  char calpass[64];
  char wxkey[48];
  char wxhost[64];
  uint8_t wxsrc;   // 0=Open-Meteo 1=和风天气
  uint8_t source;  // 0=设备直连 1=桥接
  float lat, lon;

  bool valid() const {
    if (!ssid[0]) return false;
    if (source == 1) return bridge[0] != 0;
    return email[0] && calpass[0];
  }
};
static Config g_cfg;

static void loadConfig() {
  prefs.begin("wk", true);
  prefs.getString("ssid", "").toCharArray(g_cfg.ssid, sizeof(g_cfg.ssid));
  prefs.getString("pass", "").toCharArray(g_cfg.pass, sizeof(g_cfg.pass));
  prefs.getString("bridge", "").toCharArray(g_cfg.bridge, sizeof(g_cfg.bridge));
  prefs.getString("email", "").toCharArray(g_cfg.email, sizeof(g_cfg.email));
  prefs.getString("calpass", "").toCharArray(g_cfg.calpass, sizeof(g_cfg.calpass));
  g_cfg.source = prefs.getUChar("src", 0);
  g_cfg.wxsrc = prefs.getUChar("wxsrc", 0);
  prefs.getString("wxkey", "").toCharArray(g_cfg.wxkey, sizeof(g_cfg.wxkey));
  prefs.getString("wxhost", "").toCharArray(g_cfg.wxhost, sizeof(g_cfg.wxhost));
  g_cfg.lat = prefs.getFloat("lat", 31.2304f);
  g_cfg.lon = prefs.getFloat("lon", 121.4737f);
  prefs.end();
}

static void saveConfigAll() {
  prefs.begin("wk", false);
  prefs.putString("ssid", g_cfg.ssid);
  prefs.putString("pass", g_cfg.pass);
  prefs.putString("bridge", g_cfg.bridge);
  prefs.putString("email", g_cfg.email);
  prefs.putString("calpass", g_cfg.calpass);
  prefs.putUChar("src", g_cfg.source);
  prefs.putUChar("wxsrc", g_cfg.wxsrc);
  prefs.putString("wxkey", g_cfg.wxkey);
  prefs.putString("wxhost", g_cfg.wxhost);
  prefs.putFloat("lat", g_cfg.lat);
  prefs.putFloat("lon", g_cfg.lon);
  prefs.end();
}

static void copyInto(char *dst, size_t cap, const String &src) {
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = 0;
}

// ---------------------------------------------------------------- 点阵绘制
static bool glyphLookup(const WkFont &f, uint32_t cp, const WkGlyph **out) {
  for (uint8_t i = 0; i < f.count; i++) {
    if (f.glyphs[i].cp == cp) {
      *out = &f.glyphs[i];
      return true;
    }
  }
  return false;
}

// 以基线为参考绘制单个字形；bold 为 1bit 假粗体（右移一像素重绘）
static void drawGlyph(const WkFont &f, int x, int yBase, uint32_t cp, uint16_t color,
                      int bold = 0) {
  const WkGlyph *g = nullptr;
  if (!glyphLookup(f, cp, &g)) return;
  int ox = x + g->xoff;
  int oy = yBase + g->yoff;
  for (int pass = 0; pass <= bold; pass++) {
    for (int row = 0; row < g->h; row++) {
      for (int col = 0; col < g->w; col++) {
        int byteIdx = g->off + row * ((g->w + 7) / 8) + (col >> 3);
        if (f.bits[byteIdx] & (0x80 >> (col & 7))) {
          display.drawPixel(ox + col + pass, oy + row, color);
        }
      }
    }
  }
}

static int utf8Next(const char *s, uint32_t *cp) {
  unsigned char c = (unsigned char)s[0];
  if (c < 0x80) {
    *cp = c;
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return 3;
  }
  *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
  return 4;
}

static int textWidth(const WkFont &f, const char *s) {
  int w = 0;
  uint32_t cp;
  while (*s) {
    s += utf8Next(s, &cp);
    const WkGlyph *g = nullptr;
    if (glyphLookup(f, cp, &g)) w += g->adv;
  }
  return w;
}

static void drawText(const WkFont &f, int x, int yBase, const char *s, uint16_t color,
                     int bold = 0) {
  uint32_t cp;
  while (*s) {
    int len = utf8Next(s, &cp);
    drawGlyph(f, x, yBase, cp, color, bold);
    const WkGlyph *g = nullptr;
    if (glyphLookup(f, cp, &g)) x += g->adv;
    s += len;
  }
}

// ---------------------------------------------------------------- 中文文字
// 事件标题是不可预知的，用 U8g2 的 GB2312 字库按需渲染
static void cnFont(int size) {
  // 640 面板用两档就够：13 给星期/标题，12 给时间/日期/天气。
  // 每多一套 GB2312 点阵要多吃约 200KB flash，能省则省。
  if (size >= 13) {
    u8g2.setFont(u8g2_font_wqy13_t_gb2312);
  } else {
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  }
}

static int cnWidth(int size, const char *s) {
  cnFont(size);
  return u8g2.getUTF8Width(s);
}

static void cnText(int size, int x, int yBase, const char *s, uint16_t color, int bold = 0) {
  cnFont(size);
  u8g2.setForegroundColor(color);
  u8g2.setBackgroundColor(GxEPD_WHITE);
  for (int pass = 0; pass <= bold; pass++) {
    u8g2.setCursor(x + pass, yBase);
    u8g2.print(s);
  }
}

// ---------------------------------------------------------------- 天气图标
// 图标统一按 14x14 的框画，与 640 版式的天气列宽度匹配
static void iconSun(int x, int y) {
  display.fillCircle(x + 7, y + 7, 3, GxEPD_RED);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4.0f;
    int x1 = x + 7 + (int)(cos(a) * 5), y1 = y + 7 + (int)(sin(a) * 5);
    int x2 = x + 7 + (int)(cos(a) * 7), y2 = y + 7 + (int)(sin(a) * 7);
    display.drawLine(x1, y1, x2, y2, GxEPD_RED);
  }
}

static void iconCloud(int x, int y, bool withDot) {
  display.drawCircle(x + 4, y + 10, 3, GxEPD_BLACK);
  display.drawCircle(x + 9, y + 8, 4, GxEPD_BLACK);
  display.drawLine(x + 2, y + 13, x + 12, y + 13, GxEPD_BLACK);
  if (withDot) display.fillCircle(x + 12, y + 2, 2, GxEPD_RED);
}

static void iconRain(int x, int y) {
  display.drawCircle(x + 4, y + 7, 3, GxEPD_BLACK);
  display.drawCircle(x + 9, y + 5, 4, GxEPD_BLACK);
  display.drawLine(x + 2, y + 10, x + 12, y + 10, GxEPD_BLACK);
  for (int i = 0; i < 3; i++)
    display.drawLine(x + 3 + i * 4, y + 12, x + 2 + i * 4, y + 14, GxEPD_BLACK);
}

static void drawIcon(const char *cond, int x, int y) {
  if (strstr(cond, "晴")) {
    iconSun(x, y);
  } else if (strstr(cond, "雨")) {
    iconRain(x, y);
  } else if (strstr(cond, "云")) {
    iconCloud(x, y, true);
  } else {
    iconCloud(x, y, false);
  }
}

// ---------------------------------------------------------------- 电池
// 板上电池分压接在 GPIO15：USB 供电时实测 2.014V，正好是 4.0V 的一半（1:2 分压）。
// GPIO36/32/33 也扫过，读数分别是 0.14V / 0.85V / 0.41V，比例都不像单节锂电。
// 实测：GPIO25 拉高时 GPIO36 稳定读到 2126mV（电池 4.25V，充电中），
// 拉低立刻回落到 142mV —— 说明 GPIO25 是分压使能，GPIO36 才是采样脚。
// 采样完必须把使能脚关掉，否则分压电阻会一直漏电。
static const int PIN_BAT_EN = 25;
static const int PIN_BAT_ADC = 36;
static int g_batPct = -1;    // -1 表示读不到（没接电池）
static uint32_t g_batMv = 0;

static void readBattery() {
  pinMode(PIN_BAT_EN, OUTPUT);
  digitalWrite(PIN_BAT_EN, HIGH);
  delay(20);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(PIN_BAT_ADC);
    delay(2);
  }
  digitalWrite(PIN_BAT_EN, LOW);
  uint32_t pinMv = sum / 8;
  g_batMv = pinMv * 2;  // 1:2 分压还原成电池电压
  if (g_batMv < 3000) {  // 没接电池时引脚基本贴地，别显示成 0%
    g_batPct = -1;
    Serial.printf("[bat] pin=%lumV 判定为未接电池\n", (unsigned long)pinMv);
    return;
  }
  // 单节锂电经验区间：4.20V=100%，3.40V=0%
  int pct = (int)(((int32_t)g_batMv - 3400) * 100 / 800);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  g_batPct = pct;
  Serial.printf("[bat] pin=%lumV bat=%lumV %d%%\n", (unsigned long)pinMv,
                (unsigned long)g_batMv, pct);
}

// ---------------------------------------------------------------- 页面
static void drawHeader(const WeekData &w) {
  uint8_t hm = w.viewMon ? w.viewMon : w.month;  // 看下周且跨月时，表头跟着翻月
  char num[4];
  snprintf(num, sizeof(num), "%u", hm);
  drawText(wk_num64, 14, 60, num, GxEPD_RED);
  int numW = textWidth(wk_num64, num);

  // NOV 与周数上移到第一行，更新时间占它们原来的位置
  const char *mon = MON3[(hm - 1) % 12];
  int infoX = 14 + numW + 12;
  drawText(wk_lat18, infoX, 34, mon, GxEPD_RED, 1);  // 最后一个参数=1 表示加粗一像素
  int monW = textWidth(wk_lat18, mon);

  char week[16];
  snprintf(week, sizeof(week), "第 %u 周", w.week);
  drawText(wk_txt16, infoX + monW + 14, 34, week, GxEPD_BLACK);

  cnText(12, infoX, 58, w.updated, GxEPD_BLACK);

  // 表格顶线兼作电量条：左边实心=剩余电量，右边白底黑框=已消耗。
  // 线只有 2px 高时看不出空心，所以做成 8px 高的条，位置压在原 RULE_Y 上。
  const int barX = MARGIN;
  const int barW = SCR_W - 2 * MARGIN;
  const int barH = 4;              // 总高 4px：1px 边框 + 2px 空心 + 1px 边框
  // 底边贴到 y=75（表格内容从 76 开始）：周一那天"今天"红框顶边正好在 y=76，
  // 若条子还占到 77，红框最后绘制会把条子的下沿涂红，看起来缺一截。
  const int barY = RULE_Y - barH;  // 76-4 = 72 → 占 72~75
  display.fillRect(barX, barY, barW, barH, GxEPD_BLACK);
  if (g_batPct >= 0 && g_batPct < 100) {
    int emptyW = barW * (100 - g_batPct) / 100;
    if (emptyW > 4) {
      int ex = barX + barW - emptyW;
      display.fillRect(ex, barY, emptyW, barH, GxEPD_WHITE);
      display.drawRect(ex, barY, emptyW, barH, GxEPD_BLACK);
    }
  }
}

// 小月历用的是 U8g2 的像素字体：10px 的 TTF 在 1bit 下会糊成一片，
// 而 6x10 / wqy12 这类点阵字本来就是按像素设计的，缩到最小反而清楚。
static uint8_t daysInMonth(uint16_t y, uint8_t m) {
  static const uint8_t dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2) {
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return leap ? 29 : 28;
  }
  return dim[(m - 1) % 12];
}

static void drawOneMonth(int x0, int y0, uint8_t firstWd, uint8_t ndays, uint8_t today,
                         uint8_t labelMonth) {
  // 框体高度固定 6 行：否则 11 月 6 行、12 月 5 行，两个框一高一矮很难看
  int gw = MINI_COL_W * 7, gh = 6 * MINI_ROW_H;
  display.drawRect(x0 - 4, y0 - 3, gw + 7, gh + 5, GxEPD_BLACK);

  char label[8];
  snprintf(label, sizeof(label), "%u月", labelMonth);
  cnText(12, x0 - MINI_LABEL_W + 2, y0 + 7, label, GxEPD_BLACK);  // 基线再上提 3px，使字形顶端与框顶齐平

  u8g2.setFont(u8g2_font_6x10_tf);
  int row = 0, col = firstWd;
  for (int d = 1; d <= ndays; d++) {
    int cx = x0 + col * MINI_COL_W + MINI_COL_W / 2;
    int cy = y0 + 1 + row * MINI_ROW_H;
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", d);
    int dw = 6 * (int)strlen(buf);
    u8g2.setForegroundColor(d == today ? GxEPD_RED : GxEPD_BLACK);
    u8g2.setCursor(cx - dw / 2, cy + 8);
    u8g2.print(buf);
    if (++col == 7) {
      col = 0;
      row++;
    }
  }
}

// 并排显示本月和下月，不带月份文字（640 宽下没有多余纵向空间）
static void drawMiniMonth(const WeekData &w) {
  drawOneMonth(MINI_X, MINI_Y, w.first_wd, w.ndays, w.today, w.month);

  uint8_t m2 = (w.month % 12) + 1;
  uint16_t y2 = (w.month == 12) ? (uint16_t)(w.year + 1) : w.year;
  uint8_t fw2 = (uint8_t)((w.first_wd + w.ndays) % 7);
  uint8_t nd2 = daysInMonth(y2, m2);
  int x2 = MINI_X + 7 * MINI_COL_W + MINI_LABEL_W + MINI_GAP;
  drawOneMonth(x2, MINI_Y, fw2, nd2, 0, m2);  // 下个月没有"今天"
}

// 把一天的事件按两行装填，返回是否还有溢出
static void drawDayEvents(const DayInfo &d, int y) {
  struct Item {
    const char *t;
    const char *n;
    int w;
  } items[8];
  for (int i = 0; i < d.n; i++) {
    items[i].t = d.ev[i].t;
    items[i].n = d.ev[i].n;
    items[i].w = cnWidth(12, d.ev[i].t) + 4 + cnWidth(13, d.ev[i].n);
  }

  int lineCount[2] = {0, 0};
  int used[2] = {0, 0};
  int overflow = 0;
  int placed[8];
  for (int i = 0; i < 8; i++) placed[i] = -1;
  for (int i = 0; i < d.n; i++) {
    bool ok = false;
    for (int ln = 0; ln < 2; ln++) {
      int need = used[ln] + (lineCount[ln] ? EV_GAP : 0) + items[i].w;
      if (need <= LINE_W) {
        placed[i] = ln;
        used[ln] = need;
        lineCount[ln]++;
        ok = true;
        break;
      }
    }
    if (!ok) overflow++;
  }

  int activeLines = (lineCount[0] && lineCount[1]) ? 2 : 1;
  // 行高 43，文字块中心对齐行中心 y+21.5
  int top = (activeLines > 1) ? (y + 6) : (y + 15);

  for (int ln = 0; ln < 2; ln++) {
    if (!lineCount[ln]) continue;
    int ly = top + ln * 18;
    int x = EV_X;
    bool first = true;
    for (int i = 0; i < d.n; i++) {
      if (placed[i] != ln) continue;
      if (!first) display.drawLine(x - EV_GAP / 2, ly - 1, x - EV_GAP / 2, ly + 16,
                                   GxEPD_BLACK);
      cnText(12, x, ly + 12, items[i].t, GxEPD_BLACK);
      int tw = cnWidth(12, items[i].t);
      cnText(13, x + tw + 4, ly + 11, items[i].n, GxEPD_BLACK);
      x += items[i].w + EV_GAP;
      first = false;
    }
    if (overflow && ln == (lineCount[0] && lineCount[1] ? 1 : 0)) {
      char more[12];
      snprintf(more, sizeof(more), "+%u", (unsigned)overflow);
      cnText(12, x, ly + 12, more, GxEPD_BLACK);
    }
  }
}

static void drawDays(const WeekData &w) {
  char date[12];
  char temp[16];
  int todayIdx = -1;
  for (int i = 0; i < 7; i++) {
    const DayInfo &d = w.days[i];
    int y = ROW_TOP + i * ROW_H;
    bool today = !w.nextWeek && (d.day == w.today && d.mon == w.month);

    if (today) {
      todayIdx = i;  // 红框最后统一画，避免被下一行的虚线覆盖
    } else if (i) {
      for (int x = MARGIN; x < SCR_W - MARGIN; x += 8) {
        display.drawPixel(x, y, GxEPD_BLACK);
        display.drawPixel(x + 1, y, GxEPD_BLACK);
      }
    }

    uint16_t col = today ? GxEPD_RED : GxEPD_BLACK;
    cnText(13, COL_TEXT_X, y + 19, WD_CN[i], col, 1);
    snprintf(date, sizeof(date), "%u/%u", d.mon, d.day);
    cnText(12, COL_TEXT_X, y + 36, date, col, 1);
    display.drawLine(COL_DIV_X, y + 8, COL_DIV_X, y + 35, GxEPD_BLACK);

    if (d.n == 0) {
      cnText(12, EV_X + 2, y + 27, "--", GxEPD_BLACK);
    } else {
      drawDayEvents(d, y);
    }

    display.drawLine(WX_DIV_X, y + 8, WX_DIV_X, y + 35, GxEPD_BLACK);
    if (d.lo <= -99) {
      // 天气没取到：宁可留白，也不要显示成 0/0° 让人误以为是真实温度
      cnText(12, WX_X + 6, y + 27, "--", GxEPD_BLACK);
    } else {
      drawIcon(d.cond, WX_X, y + 7);
      cnText(12, WX_X + 18, y + 19, d.cond, GxEPD_BLACK);
      snprintf(temp, sizeof(temp), "%d/%d°", d.lo, d.hi);
      cnText(12, WX_X + 18, y + 36, temp, GxEPD_BLACK);
    }
  }

  // 今天那一行最后画红框：上下边沿正好落在 y 和 y+ROW_H 的分隔虚线上
  if (todayIdx >= 0) {
    int ty = ROW_TOP + todayIdx * ROW_H;
    // 高度取 ROW_H+1，底边正好落在下一行的虚线位置（y+43），上下边沿与虚线重合
    display.drawRoundRect(4, ty, SCR_W - 8, ROW_H + 1, 10, GxEPD_RED);
    display.drawRoundRect(5, ty + 1, SCR_W - 10, ROW_H - 1, 10, GxEPD_RED);
  }
}

#ifndef ALWAYS_RENDER
#define ALWAYS_RENDER 0
#endif

static void renderWeek(const WeekData &w) {
  Serial.printf("[draw] panel %dx%d page=%u pages=%u\n", display.width(), display.height(),
                (unsigned)display.pageHeight(), (unsigned)(display.height() / display.pageHeight()));
  // 缓冲只有半屏高，整页内容要按页重绘一遍，这是 GxEPD2 分页模式的规定动作
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader(w);
    drawMiniMonth(w);
    drawDays(w);
  } while (display.nextPage());
}

// ---------------------------------------------------------------- 信息页
// 3 号键呼出。办公网络经常连不通，把 Wi-Fi/信号/上次取数/连败次数摆在一屏，
// 比连电脑看串口现实得多。
static void initDisplay();  // 定义在配网一节，这里先用

static int countEvents(const WeekData &w) {
  int n = 0;
  for (int i = 0; i < 7; i++) n += w.days[i].n;
  return n;
}

static void renderInfo() {
  uint8_t fails = 0, evN = 0;
  char lastUpd[40] = {0};
  prefs.begin("wk", true);
  fails = prefs.getUChar("fails", 0);
  evN = prefs.getUChar("evN", 0);
  prefs.getString("lastUpd", "").toCharArray(lastUpd, sizeof(lastUpd));
  prefs.end();

  // 按键唤醒不联网，g_week 可能是空的，这时用上一轮存下来的摘要
  bool have = g_week.updated[0] != 0;
  const char *upd = have ? g_week.updated : (lastUpd[0] ? lastUpd : "(尚未取数)");
  int evCount = have ? countEvents(g_week) : (int)evN;

  char l1[72], l2[64], l3[72], l4[64], l5[96], l6[48];
  snprintf(l1, sizeof(l1), "Wi-Fi: %s", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "(未连接)");
  snprintf(l2, sizeof(l2), "IP: %s   信号: %d dBm",
           WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
           WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  snprintf(l3, sizeof(l3), "上次取数: %s", upd);
  snprintf(l4, sizeof(l4), "日程 %d 条   连续失败 %u 次", evCount, (unsigned)fails);
  if (g_batPct >= 0)
    snprintf(l5, sizeof(l5), "电量 %d%%（%lu mV）  空闲内存 %u KB", g_batPct,
             (unsigned long)g_batMv, (unsigned)(ESP.getFreeHeap() / 1024));
  else
    snprintf(l5, sizeof(l5), "电量 --  空闲内存 %u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  if (have)
    snprintf(l6, sizeof(l6), "当前显示: %s（第 %u 周）", g_woff ? "下周" : "本周",
             (unsigned)g_week.week);
  else
    snprintf(l6, sizeof(l6), "当前显示: %s", g_woff ? "下周" : "本周");

  initDisplay();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(MARGIN, 46, SCR_W - 2 * MARGIN, 2, GxEPD_BLACK);
    cnText(13, MARGIN + 4, 38, "设备信息", GxEPD_BLACK, 1);
    cnText(12, MARGIN + 4, 76, l1, GxEPD_BLACK);
    cnText(12, MARGIN + 4, 102, l2, GxEPD_BLACK);
    cnText(12, MARGIN + 4, 128, l3, GxEPD_BLACK);
    cnText(12, MARGIN + 4, 154, l4, GxEPD_BLACK);
    cnText(12, MARGIN + 4, 180, l5, GxEPD_BLACK);
    display.fillRect(MARGIN, 198, SCR_W - 2 * MARGIN, 2, GxEPD_BLACK);
    cnText(12, MARGIN + 4, 224, l6, GxEPD_RED);
    cnText(12, MARGIN + 4, 252, "2 号键：立即刷新", GxEPD_BLACK);
    cnText(12, MARGIN + 4, 278, "3 号键：切换 本周 / 下周", GxEPD_BLACK);
    cnText(12, MARGIN + 4, 304, "4 号键：按 5 秒 进/出本页；长按 30 秒 进配网", GxEPD_BLACK);
    cnText(12, MARGIN + 4, 330, "1 号键：硬复位　休眠中按 4 号键唤醒", GxEPD_BLACK);
    cnText(12, MARGIN + 4, 360, "by kingboy82  https://github.com/kingboy82/epaper-week", GxEPD_BLACK);
  } while (display.nextPage());
  display.hibernate();
  // 信息页盖住了周历，清掉指纹让下一轮（或退出本页时）必定重画周历
  prefs.begin("wk", false);
  prefs.putUInt("hash", 0);
  prefs.end();
}

// ---------------------------------------------------------------- 取数
static bool fetchWeek(WeekData *out) {
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  if (!http.begin(g_cfg.bridge)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) return false;

  out->month = doc["month"] | 1;
  out->week = doc["week"] | 1;
  out->today = doc["today"] | 1;
  out->first_wd = doc["first_wd"] | 0;
  out->ndays = doc["ndays"] | 30;
  strncpy(out->updated, doc["updated"] | "", sizeof(out->updated) - 1);

  JsonArray days = doc["days"].as<JsonArray>();
  int i = 0;
  for (JsonObject day : days) {
    if (i >= 7) break;
    DayInfo &d = out->days[i];
    d.mon = day["m"] | 1;
    d.day = day["d"] | 1;
    d.lo = day["lo"] | 0;
    d.hi = day["hi"] | 0;
    strncpy(d.cond, day["cond"] | "", sizeof(d.cond) - 1);
    d.n = 0;
    JsonArray evs = day["ev"].as<JsonArray>();
    for (JsonObject ev : evs) {
      if (d.n >= 8) break;
      strncpy(d.ev[d.n].t, ev["t"] | "", sizeof(d.ev[d.n].t) - 1);
      strncpy(d.ev[d.n].n, ev["n"] | "", sizeof(d.ev[d.n].n) - 1);
      d.n++;
    }
    i++;
  }
  return i == 7;
}

static uint32_t weekHash(const WeekData &w) {
  uint32_t h = 5381;
  auto mix = [&h](const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = ((h << 5) + h) + p[i];
  };
  mix((const uint8_t *)&w.month, 1);
  mix((const uint8_t *)&w.today, 1);
  for (int i = 0; i < 7; i++) {
    mix((const uint8_t *)&w.days[i].lo, 1);
    mix((const uint8_t *)&w.days[i].hi, 1);
    mix((const uint8_t *)w.days[i].cond, strlen(w.days[i].cond));
    for (int j = 0; j < w.days[i].n; j++) {
      mix((const uint8_t *)w.days[i].ev[j].t, strlen(w.days[i].ev[j].t));
      mix((const uint8_t *)w.days[i].ev[j].n, strlen(w.days[i].ev[j].n));
    }
  }
  return h;
}

// ---------------------------------------------------------------- 配网
static void initDisplay() {
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_EPD_CS);
  display.init(115200, true, 2, false);
  u8g2.begin(display);
  u8g2.setFontMode(1);
}

static void runPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("MyWIFI-Station");
  IPAddress ip = WiFi.softAPIP();

  server.on("/", HTTP_GET, []() {
    // 回显已保存的值；字段空着却静默重启过一次，就是因为看不到存了什么
    String selD = g_cfg.source == 0 ? " selected" : "";
    String selB = g_cfg.source == 1 ? " selected" : "";
    String html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>周视图配置</title><style>"
        "body{font-family:-apple-system,sans-serif;max-width:460px;margin:24px auto;padding:0 16px}"
        "label{display:block;margin:14px 0 6px;font-size:14px;color:#333}"
        "input,select{width:100%;padding:10px;font-size:15px;border:1px solid #ccc;"
        "border-radius:6px;box-sizing:border-box}"
        "button{margin-top:20px;width:100%;padding:12px;font-size:16px;border:0;"
        "border-radius:6px;background:#2f7d4f;color:#fff}"
        ".tip{font-size:12px;color:#888;margin-top:6px}"
        "h3{margin:28px 0 4px;font-size:15px;color:#2f7d4f}"
        "</style></head><body><h2>周视图配置</h2><form method='POST' action='/save'>"
        "<label>数据来源</label><select name='source'>"
        "<option value='0'" + selD + ">设备直连（不需要电脑）</option>"
        "<option value='1'" + selB + ">桥接服务</option></select>"
        "<label>Wi-Fi 名称</label><input name='ssid' required value='" + String(g_cfg.ssid) + "'>"
        "<div class='tip'>必须是 2.4G 频段</div>"
        "<label>Wi-Fi 密码</label><input name='pass' type='password' value='" +
        String(g_cfg.pass) + "'>"
        "<h3>直连模式填这里</h3>"
        "<label>企微邮箱</label><input name='email' value='" + String(g_cfg.email) + "'>"
        "<label>日历密码</label><input name='calpass' type='password' value='" +
        String(g_cfg.calpass) + "'>"
        "<div class='tip'>企微 App → 工作台 → 日程 → 右上角三 → 同步至其他日历</div>"
        "<label>纬度</label><input name='lat' value='" + String(g_cfg.lat, 4) + "'>"
        "<label>经度</label><input name='lon' value='" + String(g_cfg.lon, 4) + "'>"
        "<label>天气源</label><select name='wxsrc'>"
        "<option value='0'" + String(g_cfg.wxsrc == 0 ? " selected" : "") +
        ">Open-Meteo（免 key，境外）</option>"
        "<option value='1'" + String(g_cfg.wxsrc == 1 ? " selected" : "") +
        ">和风天气（需 key，国内稳）</option></select>"
        "<div class='tip'>境外源在国内经常连不上，长期用建议选和风</div>"
        "<label>和风 Key</label><input name='wxkey' value='" + String(g_cfg.wxkey) + "'>"
        "<label>和风 API Host</label><input name='wxhost' value='" + String(g_cfg.wxhost) +
        "' placeholder='devapi.qweather.com'>"
        "<h3>桥接模式填这里</h3>"
        "<label>数据地址</label><input name='bridge' value='" + String(g_cfg.bridge) + "'>"
        "<div class='tip'>形如 http://192.168.1.100:8787/api/week</div>"
        "<button type='submit'>保存并重启</button></form></body></html>";
    server.send(200, "text/html", html);
  });
  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String bridge = server.arg("bridge");
    String email = server.arg("email");
    String calpass = server.arg("calpass");
    ssid.trim();
    bridge.trim();
    email.trim();
    uint8_t source = server.arg("source").toInt() ? 1 : 0;
    uint8_t wxsrc = server.arg("wxsrc").toInt() ? 1 : 0;
    String wxkey = server.arg("wxkey");
    String wxhost = server.arg("wxhost");
    wxkey.trim();
    wxhost.trim();
    float lat = server.arg("lat").toFloat();
    float lon = server.arg("lon").toFloat();

    String err;
    if (ssid.isEmpty()) {
      err = "Wi-Fi 名称不能为空";
    } else if (source == 1 && bridge.indexOf("http") != 0) {
      err = "桥接模式要填数据地址，且以 http 开头";
    } else if (source == 0 && (email.isEmpty() || calpass.isEmpty())) {
      err = "直连模式要填企微邮箱和日历密码";
    } else if (source == 0 && (lat == 0 || lon == 0)) {
      err = "直连模式要填经纬度";
    }
    if (err.length()) {
      server.send(400, "text/html", "<meta charset='utf-8'><h3>没保存：" + err +
                                        "</h3><a href='/'>返回修改</a>");
      return;
    }

    copyInto(g_cfg.ssid, sizeof(g_cfg.ssid), ssid);
    copyInto(g_cfg.pass, sizeof(g_cfg.pass), server.arg("pass"));
    copyInto(g_cfg.bridge, sizeof(g_cfg.bridge), bridge);
    copyInto(g_cfg.email, sizeof(g_cfg.email), email);
    copyInto(g_cfg.calpass, sizeof(g_cfg.calpass), calpass);
    copyInto(g_cfg.wxkey, sizeof(g_cfg.wxkey), wxkey);
    copyInto(g_cfg.wxhost, sizeof(g_cfg.wxhost), wxhost);
    g_cfg.wxsrc = wxsrc;
    g_cfg.source = source;
    g_cfg.lat = lat;
    g_cfg.lon = lon;
    saveConfigAll();
    Serial.printf("[portal] saved src=%u ssid=%s bridge=%s email=%s\n", source, g_cfg.ssid,
                  g_cfg.bridge, g_cfg.email);
    server.send(200, "text/html", "<meta charset='utf-8'><h3>已保存，设备即将重启</h3>");
    delay(800);
    ESP.restart();
  });

  server.begin();
  String tip = "浏览器打开 " + ip.toString();
  // 配网页会盖住周历，指纹必须作废，否则下次醒来会判定"内容没变"而跳过重画，
  // 屏幕就永远停在配网提示页（真实踩到过）
  prefs.begin("wk", false);
  prefs.putUInt("hash", 0);
  prefs.end();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    cnText(24, 60, 120, "配网模式", GxEPD_BLACK, 1);
    cnText(14, 60, 160, "手机连接热点 MyWIFI-Station", GxEPD_BLACK);
    cnText(14, 60, 190, tip.c_str(), GxEPD_BLACK);
  } while (display.nextPage());

  uint32_t portalStart = millis();
  while (true) {
    server.handleClient();
    delay(10);
    if (millis() - portalStart > 15UL * 60 * 1000UL) {
      prefs.begin("wk", false);
      prefs.putUChar("boots", 0);
      prefs.end();
      Serial.println("[portal] 15 分钟无人操作，休眠 5 分钟后重试");
      Serial.flush();
      esp_sleep_enable_timer_wakeup(5ULL * 60 * 1000000ULL);
      esp_deep_sleep_start();
    }
  }
}

// ---------------------------------------------------------------- 直连取数
// NTP 走 UDP 123，有些网络会封。兜底方案走普通 HTTP 的 Date 头：
// 不碰 TLS，栈和内存开销最小，也不依赖证书。
static bool syncClockHttp() {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(6000);
  if (!http.begin(client, "http://www.baidu.com")) return false;
  const char *hdrKeys[] = {"Date"};
  http.collectHeaders(hdrKeys, 1);
  int code = http.GET();
  String d = http.header("Date");
  http.end();
  if (code <= 0 || d.length() < 20) return false;

  // 形如 "Tue, 17 Nov 2026 14:35:00 GMT"，不用 strptime（依赖 locale）
  char wd[8] = {0}, mon[8] = {0};
  int dd = 0, yy = 0, hh = 0, mi = 0, ss = 0;
  if (sscanf(d.c_str(), "%7s %d %7s %d %d:%d:%d", wd, &dd, mon, &yy, &hh, &mi, &ss) != 7) {
    Serial.printf("[ntp] Date 头格式不认识: %s\n", d.c_str());
    return false;
  }
  static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int mm = 0;
  for (int i = 0; i < 12; i++) {
    if (!strncmp(mon, MON[i], 3)) {
      mm = i + 1;
      break;
    }
  }
  if (!mm) return false;

  // 按公历算 UTC 秒数（ESP32 的 newlib 没导出 timegm）
  int y = yy;
  y -= (mm <= 2);
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (mm + (mm > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long daysEpoch = era * 146097L + (long)doe - 719468L;
  time_t t = (time_t)daysEpoch * 86400 + hh * 3600 + mi * 60 + ss;
  if (t < 1700000000) return false;

  // 直接把系统时钟设成北京时间，后面统一用 gmtime_r 读。
  // 这样完全不碰 TZ 环境变量——setenv/tzset 在 newlib 上是刚才卡死的嫌疑点。
  struct timeval tv = {t + 8 * 3600, 0};
  settimeofday(&tv, nullptr);
  Serial.printf("[ntp] HTTP Date 对时成功（UTC %s）\n", d.c_str());
  return true;
}

// 自己发 SNTP 请求。不用 configTime：它会起一个后台 SNTP 任务，
// 那个任务创建 UDP 套接字时会撞上 lwIP 的线程保护直接 assert 重启。
static bool syncClockNtp() {
  // 只留阿里云：国家授时中心在国内多地实测都不回包，白等 4 秒
  static const char *SERVERS[] = {"ntp.aliyun.com"};
  for (int si = 0; si < 1; si++) {
    WiFiUDP udp;
    if (!udp.begin(0)) continue;
    uint8_t pkt[48] = {0};
    pkt[0] = 0x1B;  // LI=0, VN=3, Mode=3（客户端）
    if (udp.beginPacket(SERVERS[si], 123)) {
      udp.write(pkt, sizeof(pkt));
      udp.endPacket();
      uint32_t t0 = millis();
      while (millis() - t0 < 2000) {
        if (udp.parsePacket() >= 48) {
          uint8_t buf[48];
          udp.read(buf, sizeof(buf));
          uint32_t secs = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) |
                          ((uint32_t)buf[42] << 8) | (uint32_t)buf[43];
          if (secs > 2208988800UL) {
            struct timeval tv;
            tv.tv_sec = (time_t)(secs - 2208988800UL) + 8 * 3600;  // 直接存北京时间
            tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            udp.stop();
            Serial.printf("[ntp] %s 对时成功\n", SERVERS[si]);
            return true;
          }
        }
        delay(10);
      }
    }
    udp.stop();
    Serial.printf("[ntp] %s 无响应\n", SERVERS[si]);
  }
  return false;
}

static bool syncClock() {
  if (syncClockNtp()) return true;
  Serial.println("[ntp] NTP 不通，改用 HTTP Date 兜底");
  return syncClockHttp();
}

static uint8_t isoWeekOf(const struct tm &t) {
  int doy = t.tm_yday + 1;
  int wd = (t.tm_wday + 6) % 7;  // 0=周一
  int wk = (doy - wd + 10) / 7;
  if (wk < 1) wk = 1;
  if (wk > 53) wk = 53;
  return (uint8_t)wk;
}

static bool buildDirectWeek(WeekData &w, uint8_t weekOff) {
  g_fetchDeadline = millis() + 130UL * 1000UL;  // 单轮最多联网 130 秒（查两周要多 GET 几条）
  if (!syncClock()) {
    Serial.println("[ntp] no time");
    return false;
  }
  time_t now = time(nullptr);
  struct tm lt;
  gmtime_r(&now, &lt);
  Serial.printf("[step] clock ok, local=%04d-%02d-%02d %02d:%02d wday=%d\n",
                lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_wday);
  if (g_bootEvent) {  // 对时后才有可用时间，这里补记开机
    journal("BOOT");
    g_bootEvent = false;
  }

  int wd = (lt.tm_wday + 6) % 7;
  // 3 号键切到下周时整体后移 7 天，日期行、日历区间、天气都跟着走
  time_t monday = now - (time_t)wd * 86400 + (time_t)weekOff * 7 * 86400;

  memset(&w, 0, sizeof(w));
  DayDate days[7];
  for (int i = 0; i < 7; i++) {
    time_t t = monday + (time_t)i * 86400;
    struct tm d;
    gmtime_r(&t, &d);
    days[i].y = d.tm_year + 1900;
    days[i].mo = d.tm_mon + 1;
    days[i].d = d.tm_mday;
    w.days[i].mon = days[i].mo;
    w.days[i].day = days[i].d;
    w.days[i].n = 0;
    w.days[i].lo = -99;  // 哨兵值：表示天气没取到
    w.days[i].hi = -99;
    w.days[i].cond[0] = 0;
  }

  w.year = lt.tm_year + 1900;
  w.month = lt.tm_mon + 1;   // 小月历始终展示"本月"
  w.today = lt.tm_mday;
  w.nextWeek = weekOff ? 1 : 0;

  // 周数和表头月份取被查看那一周的周一，跨月时表头跟着翻
  time_t wt = monday + 12 * 3600;
  struct tm wm;
  gmtime_r(&wt, &wm);
  w.week = isoWeekOf(wm);
  w.viewMon = wm.tm_mon + 1;

  time_t first = now - (time_t)(lt.tm_mday - 1) * 86400;
  struct tm f;
  gmtime_r(&first, &f);
  w.first_wd = (f.tm_wday + 6) % 7;

  static const uint8_t dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int y = lt.tm_year + 1900;
  w.ndays = dim[lt.tm_mon];
  if (lt.tm_mon == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) w.ndays = 29;

  snprintf(w.updated, sizeof(w.updated), "更新 %02d-%02d %02d:%02d", lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min);

  // 天气变化慢，且已有缓存兜底：2 小时内不重复拉，省下每次约 11 秒的射频时间
  Serial.println("[step] begin weather");
  bool wx = false;
  uint32_t wxStamp = 0;  // 屏幕上这份天气是什么时候取到的
  // 本周/下周各存一份缓存，否则来回切换会把对方的日期冲掉
  const char *K_WXC = weekOff ? "wxc1" : "wxc";
  const char *K_WXAT = weekOff ? "wxAt1" : "wxAt";
  const char *K_CALC = weekOff ? "calc1" : "calc";
  prefs.begin("wk", true);
  uint32_t wxAge = prefs.getUInt(K_WXAT, 0);
  String cachedWx = prefs.getString(K_WXC, "");
  prefs.end();
  time_t nowSec = time(nullptr);
  bool wxFresh = wxAge && nowSec > (time_t)wxAge && (nowSec - (time_t)wxAge) < 2 * 3600;

  if (wxFresh && applyWxCache(cachedWx, days, w)) {
    Serial.println("[wx] 缓存足够新，本轮跳过联网");
    wx = true;
    wxStamp = wxAge;
  } else if (g_cfg.wxsrc == 1) {
    const char *host = g_cfg.wxhost[0] ? g_cfg.wxhost : "devapi.qweather.com";
    wx = fetchWeatherQWeather(host, g_cfg.wxkey, g_cfg.lat, g_cfg.lon, days, w);
  } else {
    wx = fetchWeatherDirect(g_cfg.lat, g_cfg.lon, days, w);
  }
  if (wx) {
    if (!wxFresh) {
      prefs.begin("wk", false);
      prefs.putString(K_WXC, buildWxCache(w));
      prefs.putUInt(K_WXAT, (uint32_t)nowSec);
      prefs.end();
      wxStamp = (uint32_t)nowSec;
    }
  } else {
    prefs.begin("wk", true);
    String cache = prefs.getString(K_WXC, "");
    prefs.end();
    wx = applyWxCache(cache, days, w);
  }

  Serial.println("[step] begin calendar");
  prefs.begin("wk", true);
  uint8_t calMask = prefs.getUChar("calmask", 0);
  uint32_t calMaskAt = prefs.getUInt("calmaskAt", 0);
  prefs.end();
  uint32_t calAgeSec = (calMaskAt && nowSec > (time_t)calMaskAt)
                           ? (uint32_t)(nowSec - (time_t)calMaskAt)
                           : 999999;
  uint8_t calMaskOut = calMask;
  bool cal = fetchCalendarDirect(g_cfg.email, g_cfg.calpass, days, w, calMask, calAgeSec,
                                 &calMaskOut);
  bool wxFromNet = !wxFresh;
  if (cal) {
    prefs.begin("wk", false);
    prefs.putString(K_CALC, buildCalCache(w));
    prefs.end();
  } else {
    prefs.begin("wk", true);
    String calCache = prefs.getString(K_CALC, "");
    prefs.end();
    applyCalCache(calCache, days, w);
  }
  if (cal && calMaskOut != calMask) {
    prefs.begin("wk", false);
    prefs.putUChar("calmask", calMaskOut);
    prefs.putUInt("calmaskAt", (uint32_t)nowSec);
    prefs.end();
  } else if (cal && calAgeSec > 7200) {
    prefs.begin("wk", false);
    prefs.putUChar("calmask", calMaskOut);
    prefs.putUInt("calmaskAt", (uint32_t)nowSec);
    prefs.end();
  }
  Serial.printf("[direct] weather=%d(%s) calendar=%d\n", wx, wxFresh ? "缓存" : "联网", cal);

  // 把解析到的每条事件原样打出来，方便核对哪一条没落到日期上
  for (uint8_t i = 0; i < g_ics.count; i++) {
    const RawEvent &e = g_ics.ev[i];
    Serial.printf("[ev] %02d/%02d %02d:%02d end=%02d/%02d freq=%u byday=%02x  %s\n", e.mo, e.d,
                  e.hh, e.mm, e.emo, e.ed, e.freq, e.byday, e.summary);
  }

  // 天气和日程通常不是同一时刻取的（天气有 2 小时节流），
  // 只显示一个时间会让人分不清屏幕上的天气到底是新是旧，所以单独标出来
  // 原来要求 nowSec > wxStamp，但天气就是本轮取的时候两者相等，条件不成立，
  // 结果"天气 HH:MM"被吞掉（用户实测反馈）。只要有时间戳就显示。
  if (wxStamp) {
    struct tm wt;
    time_t ws = (time_t)wxStamp;
    gmtime_r(&ws, &wt);
    char tail[24];
    snprintf(tail, sizeof(tail), " · 天气 %02d:%02d", wt.tm_hour, wt.tm_min);
    strncat(w.updated, tail, sizeof(w.updated) - strlen(w.updated) - 1);
  }
  // 两条都来自缓存 == 这一轮其实什么都没取到，交给外面做失败退避
  if (!cal && !wxFromNet) {
    Serial.println("[direct] 天气和日程都来自缓存，本轮视为失败");
    return false;
  }
  return wx || cal;
}

// 睡到下一个整点或半点。固定睡 30 分钟会因为"刷屏 22 秒"不断往后漂，
// 久了就变成 21:45 → 22:27 这种看着像迟到的时间。
static uint64_t sleepUntilNextHalfHour() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 30ULL * 60;  // 没对时就退回固定 30 分钟
  time_t next = ((now / 1800) + 1) * 1800;
  long sec = (long)(next - now);
  if (sec < 60) sec = 60;
  if (sec > 1800) sec = 1800;
  return (uint64_t)sec;
}

// ---------------------------------------------------------------- 取数 + 绘制
// 把当前 g_week 重绘一遍并刷新指纹，用于从信息页退回周历
static void renderWeekNow() {
  initDisplay();
  renderWeek(g_week);
  display.hibernate();
  prefs.begin("wk", false);
  prefs.putUInt("hash", weekHash(g_week));
  prefs.putUInt("lastDraw", (uint32_t)time(nullptr));
  prefs.putUChar("rep", 9);  // 与 doUpdate 里的 RENDER_EPOCH 保持一致
  prefs.putInt("batDraw", g_batPct);
  prefs.end();
}

// 一次完整的"取数 → 比对指纹 → 重绘"。按键刷新时会再调一次，所以不能写成一次性流程。
static bool doUpdate(bool force) {
  bool ok = (g_cfg.source == 1) ? fetchWeek(&g_week) : buildDirectWeek(g_week, g_woff);

  // 连续失败说明网络不通（热点没数据、DNS 挂掉等）。这时每次唤醒都要空耗两分钟射频，
  // 电池撑不住，所以退避到 30 分钟再试。
  prefs.begin("wk", true);
  uint8_t failCount = prefs.getUChar("fails", 0);
  prefs.end();

  if (ok) {
    Serial.printf("[wake] 本地时间 %s\n", g_week.updated);
    // 信息页可能在没有本轮数据时被呼出（按键唤醒不联网），所以另存一份摘要
    prefs.begin("wk", false);
    prefs.putString("lastUpd", g_week.updated);
    prefs.putUChar("evN", (uint8_t)countEvents(g_week));
    prefs.end();
    if (failCount) {
      prefs.begin("wk", false);
      prefs.putUChar("fails", 0);
      prefs.end();
    }
  } else {
    failCount = failCount < 200 ? failCount + 1 : failCount;
    prefs.begin("wk", false);
    prefs.putUChar("fails", failCount);
    prefs.end();
    char tag[16];
    snprintf(tag, sizeof(tag), "FAIL%u", (unsigned)failCount);
    journal(tag);
    return false;
  }

  // 指纹存在 NVS 里，内容没变就跳过整屏刷新（三色屏一次全刷要 20 秒）
  uint32_t h = weekHash(g_week);
  prefs.begin("wk", true);
  uint32_t last = prefs.getUInt("hash", 0);
  prefs.end();

  time_t nowTs = time(nullptr);
  prefs.begin("wk", true);
  uint32_t lastDraw = prefs.getUInt("lastDraw", 0);
  prefs.end();
  uint32_t ageMin =
      (lastDraw && nowTs > (time_t)lastDraw) ? (uint32_t)((nowTs - lastDraw) / 60) : 9999;
  const uint32_t MAX_STALE_MIN = 120;  // 数据没变也至少每 2 小时重画一次

  // 换固件/改版式后，屏幕内容可能已经和指纹对不上（停在配网页、信息页），
  // 用版本号强制重画一次，之后恢复正常"内容不变就不刷"。
  const uint8_t RENDER_EPOCH = 9;
  prefs.begin("wk", true);
  uint8_t rep = prefs.getUChar("rep", 0);
  int drawnBat = prefs.getInt("batDraw", -1);
  prefs.end();
  bool epochChanged = (rep != RENDER_EPOCH);
  // 电量条也得刷新：变化 ≥5% 就重画，否则掉电后画面会一直停在旧的电量
  bool batChanged = (g_batPct >= 0 && (drawnBat < 0 || abs(drawnBat - g_batPct) >= 5));

  Serial.printf("[data] month=%u week=%u hash=%08lx last=%08lx age=%lum force=%d\n",
                (unsigned)g_week.month, (unsigned)g_week.week, (unsigned long)h,
                (unsigned long)last, (unsigned long)ageMin, (int)force);

  if (force || epochChanged || batChanged || ALWAYS_RENDER || h != last || !last ||
      ageMin >= MAX_STALE_MIN) {
    Serial.println("[draw] rendering...");
    renderWeekNow();
    Serial.println("[draw] done, panel hibernated");
    journal("DRAW");
  } else {
    Serial.printf("[draw] skip，内容未变（已 %lum 未刷）\n", (unsigned long)ageMin);
  }
  return true;
}

// 休眠：定时唤醒 + 4 号键（GPIO39）低电平唤醒。
// 34/35/39 都是 RTC 脚，但 EXT1 只支持"全部为低"，所以只有 4 号键能单独唤醒。
static void sleepDeep(uint64_t sec) {
  esp_sleep_enable_timer_wakeup(sec * 1000000ULL);
  esp_err_t e = esp_sleep_enable_ext0_wakeup((gpio_num_t)KEY_PORTAL, 0);
  if (e != ESP_OK) Serial.printf("[sleep] ext0 wake 配置失败: %d\n", (int)e);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------- 主流程
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[boot] week view firmware");

  keysBegin();
  readBattery();

  prefs.begin("wk", true);
  String prevJrn = prefs.getString("jrn", "");
  prefs.end();
  if (prevJrn.length()) {
    Serial.printf("[jrn] 历史记录:\n%s", prevJrn.c_str());
  }
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool keyWake = (cause == ESP_SLEEP_WAKEUP_EXT0);
  // 非定时/非按键唤醒＝真正的上电或复位，值得记一条
  g_bootEvent = (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT0);  // 4 号键按醒的，强制重绘一次
  Serial.printf("[boot] reset reason=%d heap=%lu wakeup=%d keyWake=%d\n",
                (int)esp_reset_reason(), (unsigned long)ESP.getFreeHeap(), (int)cause,
                (int)keyWake);

  // 旧方案"连按两次 EN 进配网"已取消：计数只要没取数成功就一直累加，
  // 刷固件/断电重启两次就会误进配网。现在统一用 4 号键长按 5 秒。
  Serial.printf("[boot] forcePortal=%d\n", (int)g_forcePortal);

  loadConfig();
  Serial.printf("[cfg] ssid=%s bridge=%s\n", g_cfg.ssid[0] ? g_cfg.ssid : "(none)",
                g_cfg.bridge[0] ? g_cfg.bridge : "(none)");

  prefs.begin("wk", true);
  g_woff = prefs.getUChar("woff", 0) ? 1 : 0;  // 上次看的是本周还是下周
  prefs.end();
  Serial.printf("[cfg] 视图=%s\n", g_woff ? "下周" : "本周");

  // 注意：不要在这里判断"4 号键是否按住"来进配网。
  // 4 号键本身就是唤醒键，唤醒的那一下会被误判成长按，现在统一用醒来后的按键窗口。

  if (!g_cfg.valid() || g_forcePortal) {
    Serial.printf("[cfg] 进入配网: src=%s ssid=%s email=%s pass=%s\n",
                  g_cfg.source == 1 ? "bridge" : "direct",
                  g_cfg.ssid[0] ? "ok" : "missing",
                  g_cfg.email[0] ? "ok" : "missing",
                  g_cfg.calpass[0] ? "ok" : "missing");
    initDisplay();
    runPortal();
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(g_cfg.ssid, g_cfg.pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(200);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] connect failed, retry in 5min（要改 Wi-Fi 请长按 4 号键 5 秒）");
    Serial.flush();
    sleepDeep(5ULL * 60);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // 热点下发的 DNS 经常无响应，lwIP 会死等 150 秒。显式换成国内公共 DNS。
    WiFi.setDNS(IPAddress(223, 5, 5, 5), IPAddress(119, 29, 29, 29));
    Serial.printf("[wifi] ok ip=%s rssi=%d dns=%s\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(), WiFi.dnsIP().toString().c_str());
  } else {
    Serial.println("[wifi] FAILED - 热点没开或密码变了");
  }

  // 按键唤醒 ≠ 定时刷新：人就在旁边，先开按键窗口让他能立刻操作。
  // 取数要 1~2 分钟，一醒来就闷头拉网，按键全被吞掉，等于按了没反应。
  bool ok = true;
  if (keyWake) {
    Serial.println("[wake] 按键唤醒：直接开按键窗口，不自动取数");
  } else {
    ok = doUpdate(false);
  }

  if (!ok) {
    prefs.begin("wk", true);
    uint8_t fc = prefs.getUChar("fails", 0);
    prefs.end();
    uint64_t retry = (fc >= 3 ? 30 : 5) * 60ULL;  // 连续三次失败就慢下来
    Serial.printf("[data] fetch failed (%u 连败)，%llu 分钟后重试\n", (unsigned)fc,
                  (unsigned long long)(retry / 60));
    Serial.flush();
    sleepDeep(retry);
  }

  // 取数要 1~2 分钟（7 个日历逐个 GET），期间按键不响应。
  // 所以按键唤醒后窗口给到 90 秒：等屏幕闪完再按也来得及。
  const uint32_t winMs = keyWake ? 90000 : 12000;
  for (;;) {
    int act = keysWait(winMs);
    if (act == ACT_REFRESH) {
      Serial.println("[key] 2 号键：立即刷新");
      g_infoShown = false;
      if (!doUpdate(true)) break;
    } else if (act == ACT_TOGGLE_WEEK) {
      g_woff ^= 1;
      prefs.begin("wk", false);
      prefs.putUChar("woff", g_woff);
      prefs.end();
      Serial.printf("[key] 3 号键：切到%s\n", g_woff ? "下周" : "本周");
      g_infoShown = false;
      if (!doUpdate(true)) break;
    } else if (act == ACT_INFO) {
      if (g_infoShown) {
        Serial.println("[key] 4 号键 5 秒：退出信息页");
        g_infoShown = false;
        if (g_week.updated[0]) {
          renderWeekNow();
        } else if (!doUpdate(true)) {
          break;
        }
      } else {
        Serial.println("[key] 4 号键 5 秒：信息页");
        renderInfo();
        g_infoShown = true;
      }
    } else if (act == ACT_PORTAL) {
      Serial.println("[key] 4 号键长按 30 秒：进入配网");
      runPortal();
      break;
    } else {
      break;
    }
    // 上面每个动作都可能阻塞几十秒到两分钟，长按计时必须以"现在"重新起算
    keysReArm();
  }

  WiFi.disconnect(true);
  uint64_t sleepSec = sleepUntilNextHalfHour();
  Serial.printf("[sleep] %llu 秒后醒来（对齐半点，4 号键可提前唤醒）\n",
                (unsigned long long)sleepSec);
  Serial.flush();
  sleepDeep(sleepSec);
}

void loop() {}
