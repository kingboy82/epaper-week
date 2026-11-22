#pragma once
#include <Arduino.h>

// iCalendar 解析：只挑周视图需要的字段，不做完整实现。
// 目标是把一段 VCALENDAR 文本变成若干"某天某个时刻、标题是什么"的事件，
// 并支持最常见的重复规则（每天 / 每周 / 每月）。

struct RawEvent {
  int16_t y, mo, d;
  int16_t hh, mm;
  int16_t ey, emo, ed;   // DTEND 的日期，跨天事件用
  char summary[48];
  uint8_t freq;      // 0=不重复 1=DAILY 2=WEEKLY 3=MONTHLY
  uint8_t interval;
  uint8_t byday;     // bit0=周一 … bit6=周日，0 表示未指定
  bool hasUntil;
  int16_t uy, umo, ud;
  int16_t uh, umin;
  bool allDay;
  bool cancelled;
};

static const int MAX_RAW_EVENTS = 72;

// "20261109T093000" / "20261109T013000Z" / "20261109"（可带参数段）
static bool parseIcsDateTime(const String &value, bool &allDay, int16_t &y, int16_t &mo,
                             int16_t &d, int16_t &hh, int16_t &mm) {
  String v = value;
  v.trim();
  allDay = (v.indexOf('T') < 0);
  bool utc = false;
  if (v.endsWith("Z")) {
    utc = true;
    v.remove(v.length() - 1);
  }
  if (v.length() < 8) return false;
  y = v.substring(0, 4).toInt();
  mo = v.substring(4, 6).toInt();
  d = v.substring(6, 8).toInt();
  hh = mm = 0;
  if (!allDay && v.length() >= 13) {
    hh = v.substring(9, 11).toInt();
    mm = v.substring(11, 13).toInt();
  }
  if (hh < 0 || hh > 23) hh = 0;
  if (mm < 0 || mm > 59) mm = 0;
  if (utc) {
    // UTC 转到东八区，跨日要进位
    hh += 8;
    if (hh >= 24) {
      hh -= 24;
      d += 1;
      static const uint8_t dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      uint8_t limit = dim[(mo - 1) % 12];
      if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) limit = 29;
      if (d > limit) {
        d = 1;
        mo += 1;
        if (mo > 12) {
          mo = 1;
          y += 1;
        }
      }
    }
  }
  return true;
}

class IcsParser {
 public:
  RawEvent ev[MAX_RAW_EVENTS];
  uint8_t count = 0;

  void reset() { count = 0; }

  // 把一段 VCALENDAR 里的事件并入缓冲区，超出上限就丢弃（周视图也就显示几条）
  void feed(const String &ics) {
    RawEvent cur;
    bool inEvent = false;
    String logical;  // 逻辑行：折行拼接后的完整属性

    auto handle = [&](const String &line) {
      if (line.startsWith("BEGIN:VEVENT")) {
        inEvent = true;
        memset(&cur, 0, sizeof(cur));
        cur.interval = 1;
        return;
      }
      if (line.startsWith("END:VEVENT")) {
        if (inEvent && cur.summary[0] && !cur.cancelled) commit(cur);
        inEvent = false;
        return;
      }
      if (!inEvent || !line.length()) return;
      int colon = line.indexOf(':');
      if (colon <= 0) return;
      // 有些服务器写成 TZID=GMT+08:00:2026...，冒号会出现在参数里。
      // 取"后面紧跟 8 位数字"的那个冒号作为属性名与值的分界。
      for (int k = line.length() - 9; k > 0; k--) {
        if (line[k] != ':') continue;
        bool digits = true;
        for (int q = 1; q <= 8; q++) {
          char c = line[k + q];
          if (c < '0' || c > '9') { digits = false; break; }
        }
        if (digits) { colon = k; break; }
      }
      String head = line.substring(0, colon);
      String val = line.substring(colon + 1);
      head.toUpperCase();
      if (head.startsWith("DTSTART")) {
        bool ad = false;
        parseIcsDateTime(val, ad, cur.y, cur.mo, cur.d, cur.hh, cur.mm);
        cur.allDay = ad;
      } else if (head.startsWith("DTEND")) {
        bool ad = false;
        int16_t ey, emo, ed, eh, em;
        if (parseIcsDateTime(val, ad, ey, emo, ed, eh, em)) {
          cur.ey = ey; cur.emo = emo; cur.ed = ed;
        }
      } else if (head.startsWith("SUMMARY")) {
        copySummary(cur, val);
      } else if (head.startsWith("RRULE")) {
        parseRRule(cur, val);
      } else if (head.startsWith("STATUS")) {
        if (val.indexOf("CANCELLED") >= 0) cur.cancelled = true;
      }
    };

    int i = 0;
    const int len = ics.length();
    String raw;
    while (i <= len) {
      char c = (i < len) ? ics[i] : '\n';
      i++;
      if (c != '\n') {
        if (c != '\r') raw += c;
        continue;
      }
      // RFC5545 折行：以空格或制表符开头的行，是上一行的续行
      if (raw.length() && (raw[0] == ' ' || raw[0] == '\t')) {
        String tail = raw.substring(1);
        logical += tail;
      } else {
        if (logical.length()) handle(logical);
        logical = raw;
      }
      raw = "";
    }
    if (logical.length()) handle(logical);
  }

  // 判断事件是否落在指定日期
  static bool occursOn(const RawEvent &e, int16_t y, int16_t mo, int16_t d) {
    long target = y * 10000L + mo * 100L + d;
    long start = e.y * 10000L + e.mo * 100L + e.d;
    if (target == start) return true;
    // 跨天事件（出差、培训之类）：起始日之后的每一天也要出现
    if (e.ey) {
      long end = e.ey * 10000L + e.emo * 100L + e.ed;
      if (target > start && target <= end) return true;
    }
    if (!e.freq || target < start) return false;
    if (e.hasUntil) {
      long uDate = e.uy * 10000L + e.umo * 100L + e.ud;
      if (target > uDate) return false;
      if (target == uDate && (e.hh * 60 + e.mm) > (e.uh * 60 + e.umin)) return false;
    }
    long days = daysBetween(e.y, e.mo, e.d, y, mo, d);
    if (days <= 0) return false;
    uint8_t interval = e.interval ? e.interval : 1;

    if (e.freq == 1) return (days % interval) == 0;
    if (e.freq == 2) {
      if ((days / 7) % interval) return false;
      if (!e.byday) return (days % 7) == 0;
      return e.byday & (1 << weekdayMon0(y, mo, d));
    }
    if (e.freq == 3) {
      if (mo != e.mo || d != e.d) return false;
      long months = (y - e.y) * 12L + (mo - e.mo);
      return months > 0 && (months % interval) == 0;
    }
    return false;
  }

  static uint8_t weekdayMon0(int16_t y, int16_t mo, int16_t d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int yy = y;
    if (mo < 3) yy -= 1;
    int w = (yy + yy / 4 - yy / 100 + yy / 400 + t[mo - 1] + d) % 7;  // 0=周日
    return (uint8_t)((w + 6) % 7);                                      // 转成 0=周一
  }

 private:
  static void copySummary(RawEvent &e, String v) {
    v.replace("\\,", ",");
    v.replace("\\n", " ");
    v.replace("\\;", ";");
    size_t cap = sizeof(e.summary) - 1;
    size_t n = v.length();
    if (n > cap) {
      n = cap;
      // 截断时不能把 UTF-8 多字节字符切成两半，否则屏幕上会出现乱码方块
      while (n > 0 && ((unsigned char)v[n] & 0xC0) == 0x80) n--;
    }
    memcpy(e.summary, v.c_str(), n);
    e.summary[n] = 0;
  }

  static void parseRRule(RawEvent &e, const String &rule) {
    int pos = 0;
    while (pos < (int)rule.length()) {
      int semi = rule.indexOf(';', pos);
      if (semi < 0) semi = rule.length();
      String kv = rule.substring(pos, semi);
      pos = semi + 1;
      int eq = kv.indexOf('=');
      if (eq < 0) continue;
      String key = kv.substring(0, eq);
      String val = kv.substring(eq + 1);
      key.toUpperCase();
      if (key == "FREQ") {
        if (val.indexOf("DAILY") >= 0) e.freq = 1;
        else if (val.indexOf("WEEKLY") >= 0) e.freq = 2;
        else if (val.indexOf("MONTHLY") >= 0) e.freq = 3;
      } else if (key == "INTERVAL") {
        e.interval = (uint8_t)constrain(val.toInt(), 1, 12);
      } else if (key == "BYDAY") {
        int p = 0;
        while (p < (int)val.length()) {
          String code = val.substring(p, min(p + 2, (int)val.length()));
          code.toUpperCase();
          if (code == "MO") e.byday |= 1 << 0;
          else if (code == "TU") e.byday |= 1 << 1;
          else if (code == "WE") e.byday |= 1 << 2;
          else if (code == "TH") e.byday |= 1 << 3;
          else if (code == "FR") e.byday |= 1 << 4;
          else if (code == "SA") e.byday |= 1 << 5;
          else if (code == "SU") e.byday |= 1 << 6;
          p += 3;
        }
      } else if (key == "UNTIL") {
        bool ad;
        int16_t y, mo, d, hh, mm;
        if (parseIcsDateTime(val, ad, y, mo, d, hh, mm)) {
          e.hasUntil = true;
          e.uy = y;
          e.umo = mo;
          e.ud = d;
          e.uh = hh;
          e.umin = mm;
        }
      }
    }
  }

  static long daysBetween(int16_t y1, int16_t m1, int16_t d1, int16_t y2, int16_t m2, int16_t d2) {
    return toSerial(y2, m2, d2) - toSerial(y1, m1, d1);
  }

  static long toSerial(int16_t y, int16_t m, int16_t d) {
    // 公历转 N 日序号（Howard Hinnant 的算法，简化版够用）
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe;
  }

  void commit(const RawEvent &e) {
    if (count >= MAX_RAW_EVENTS) return;
    ev[count++] = e;
  }
};
