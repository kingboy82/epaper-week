#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "ca_certs.h"
#include "ics.h"
#include "model.h"

// 设备直连模式：自己去 Open-Meteo 取天气、去企微 CalDAV 取日程。
// 全部走 HTTPS 且校验证书——这条链路会发送企微密码，不能用 setInsecure。

static const char *CALDAV_HOST = "https://caldav.wecom.work";

// 单轮联网的总预算。实测热点到企微会出现"卡 150 秒才失败"的情况，
// 而 HTTPClient 的连接超时在这种阻塞下并不生效，只能用墙钟兜底。
static uint32_t g_fetchDeadline = 0;
static inline bool fetchBudgetLeft() {
  return g_fetchDeadline == 0 || (int32_t)(g_fetchDeadline - millis()) > 0;
}

// 解析器里有 48 个 RawEvent（约 3.6KB）。放在栈上会直接撑爆 loopTask，
// 实测就是这里触发了 stack canary。放静态区，栈只留指针。
static IcsParser g_ics;

struct DayDate {
  int16_t y, mo, d;
};

static const char *wmoToText(int code) {
  if (code == 0 || code == 1) return "晴";
  if (code == 2) return "多云";
  if (code == 3) return "阴";
  if (code == 45 || code == 48) return "雾";
  if (code >= 51 && code <= 57) return "小雨";
  if (code == 61 || code == 63) return "小雨";
  if (code == 65) return "大雨";
  if (code == 66 || code == 67) return "雨";
  if (code == 71 || code == 73 || code == 77) return "雪";
  if (code == 75) return "大雪";
  if (code == 80 || code == 81) return "阵雨";
  if (code == 82) return "暴雨";
  if (code == 85 || code == 86) return "阵雪";
  if (code >= 95) return "雷阵雨";
  return "阴";
}

// ---------------------------------------------------------------- 天气
static bool fetchWeatherDirect(float lat, float lon, const DayDate days[7], WeekData &w) {
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&daily=weather_code,temperature_2m_max,temperature_2m_min"
               // 15 天才能覆盖"下周"视图（8 天只到今天+7）
               "&timezone=Asia%2FShanghai&forecast_days=15&past_days=7";

  String body;
  int code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {  // 热点网络偶发抖动，重试一次
    WiFiClientSecure client;
    client.setCACert(CA_OPENMETEO);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    if (!http.begin(client, url)) {
      Serial.println("[wx] begin failed");
      continue;
    }
    code = http.GET();
    if (code == 200) {
      body = http.getString();
      http.end();
      break;
    }
    Serial.printf("[wx] attempt %d http %d (%s)\n", attempt + 1, code,
                  http.errorToString(code).c_str());
    http.end();
    delay(500);
  }
  if (code != 200 || body.length() == 0) {
    Serial.printf("[wx] give up after 2 tries, code=%d\n", code);
    return false;
  }
  Serial.printf("[wx] body %u bytes\n", (unsigned)body.length());

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[wx] json parse failed");
    return false;
  }
  JsonArray times = doc["daily"]["time"];
  JsonArray codes = doc["daily"]["weather_code"];
  JsonArray maxs = doc["daily"]["temperature_2m_max"];
  JsonArray mins = doc["daily"]["temperature_2m_min"];
  if (times.isNull() || codes.isNull()) return false;

  int filled = 0;
  for (int i = 0; i < 7; i++) {
    for (size_t k = 0; k < times.size(); k++) {
      const char *ts = times[k];
      int y, mo, dd;
      if (sscanf(ts, "%d-%d-%d", &y, &mo, &dd) != 3) continue;
      if (mo != days[i].mo || dd != days[i].d) continue;
      DayInfo &d = w.days[i];
      strncpy(d.cond, wmoToText(codes[k] | 0), sizeof(d.cond) - 1);
      d.hi = (int8_t)round(maxs[k] | 0.0f);
      d.lo = (int8_t)round(mins[k] | 0.0f);
      filled++;
      break;
    }
  }
  Serial.printf("[wx] filled %d/7\n", filled);
  return filled > 0;
}

// ---------------------------------------------------------------- CalDAV
static int davRequest(const char *email, const char *pass, const String &url, const char *method,
                      const String &body, String *out) {
  WiFiClientSecure client;
  client.setCACert(CA_WECOM);
  client.setHandshakeTimeout(5);   // 秒：握手卡死时不要无限等
  client.setTimeout(8000);         // 毫秒：读超时
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(12000);
  if (!http.begin(client, url)) return -1;
  http.setAuthorization(email, pass);
  http.addHeader("Depth", "1");
  http.addHeader("Content-Type", "application/xml; charset=utf-8");
  int code = http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
  if (code > 0 && out) *out = http.getString();
  http.end();
  return code;
}

// 从 pos 开始找下一个 calendar-data 段，找到后把 pos 推到该段之后
static bool extractCalendarDataFrom(const String &xml, int *pos, String *out) {
  int a = xml.indexOf("calendar-data", *pos);
  if (a < 0) return false;
  int gt = xml.indexOf('>', a);
  if (gt < 0) return false;
  int b = xml.indexOf("</", gt);
  if (b < 0) return false;
  *pos = b + 2;
  String data = xml.substring(gt + 1, b);
  if (data.indexOf("BEGIN:VEVENT") < 0) return false;
  *out = data;
  return true;
}

static void collectHrefs(const String &xml, String *out, int maxOut, int *count) {
  int pos = 0;
  while (*count < maxOut) {
    int a = xml.indexOf("<d:href>", pos);
    int plen = 8;
    if (a < 0) {
      a = xml.indexOf("<href>", pos);
      plen = 6;
    }
    if (a < 0) break;
    int b = xml.indexOf("</d:href>", a);
    int elen = 9;
    if (b < 0) {
      b = xml.indexOf("</href>", a);
      elen = 8;
    }
    if (b < 0) break;
    String href = xml.substring(a + plen, b);
    href.trim();
    if (href.length()) out[(*count)++] = href;
    pos = b + elen;
  }
}

// 有的服务器把 VEVENT 直接内联在 calendar-data 里
static bool extractCalendarData(const String &xml, String *out) {
  int pos = 0;
  return extractCalendarDataFrom(xml, &pos, out);
}

static String buildQuery(const char *startUtc, const char *endUtc) {
  String s = "<?xml version=\"1.0\"?><c:calendar-query xmlns:d=\"DAV:\" "
             "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><d:getetag/>"
             "<c:calendar-data/></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\">"
             "<c:comp-filter name=\"VEVENT\"><c:time-range start=\"";
  s += startUtc;
  s += "\" end=\"";
  s += endUtc;
  s += "\"/></c:comp-filter></c:comp-filter></c:filter></c:calendar-query>";
  return s;
}

// maskIn：上次扫描时"有日程"的日历位图（按 PROPFIND 返回顺序）。
// 只查有日程的那些，能省掉大部分 TLS 握手；每 2 小时做一次全量兜底，避免漏。
static bool fetchCalendarDirect(const char *email, const char *pass, const DayDate days[7],
                                WeekData &w, uint8_t maskIn, uint32_t maskAgeSec,
                                uint8_t *maskOut) {
  if (!email[0] || !pass[0]) return false;

  // 查询区间：企微 CalDAV 只认开始时间——开始时间在今天或更早才返回数据，
  // 结束时间基本被忽略（实测 start=11/18 时 end 给 1 天还是 15 天都返回同样的 7 条；
  // start 给未来日期一律 0 条）。所以"看下周"不能从下周一开始查，
  // 必须从今天往前一天起算，本地再按目标周筛选。
  char startUtc[48], endUtc[48];
  // 起点 = 所看那一周的周一往前 8 天。
  // 实测服务器只按 DTSTART >= start 过滤、没有上限，起点越晚丢得越多：
  // 本周日（11/22）用"今天-3"(11/19) 查询，周一~周三的日程全部消失。
  // 取"周一-8"既一定落在过去（下周视图也不会变成未来起点），又能覆盖整周。
  int sy = days[0].y, sm = days[0].mo, sd = days[0].d;
  for (int i = 0; i < 8; i++) {
    static const uint8_t dimB[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    sd--;
    if (sd < 1) {
      sm--;
      if (sm < 1) {
        sm = 12;
        sy--;
      }
      sd = dimB[(sm - 1) % 12];
      if (sm == 2 && ((sy % 4 == 0 && sy % 100 != 0) || sy % 400 == 0)) sd = 29;
    }
  }
  int ey = days[6].y, em = days[6].mo, ed = days[6].d;
  for (int i = 0; i < 8; i++) {
    static const uint8_t dims[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int lim = dims[(em - 1) % 12];
    if (em == 2 && ((ey % 4 == 0 && ey % 100 != 0) || ey % 400 == 0)) lim = 29;
    ed++;
    if (ed > lim) {
      ed = 1;
      em++;
      if (em > 12) {
        em = 1;
        ey++;
      }
    }
  }
  snprintf(startUtc, sizeof(startUtc), "%04d%02d%02dT000000Z", sy, sm, sd);
  snprintf(endUtc, sizeof(endUtc), "%04d%02d%02dT000000Z", ey, em, ed);

  String home = String(CALDAV_HOST) + "/calendar/";
  String xml;
  int code = -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    code = davRequest(email, pass, home, "PROPFIND",
                        String("<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\" "
                               "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop>"
                                 "<d:resourcetype/><d:displayname/></d:prop></d:propfind>"),
                          &xml);
    Serial.printf("[cal] propfind try%d=%d (%u bytes)\n", attempt + 1, code,
                  (unsigned)xml.length());
    if (code > 0 && xml.length()) break;
    delay(800);
  }
  if (code <= 0 || !xml.length()) return false;

  String hrefs[32];
  int nHref = 0;
  collectHrefs(xml, hrefs, 32, &nHref);
  Serial.printf("[cal] collections=%d\n", nHref);

  IcsParser &parser = g_ics;
  parser.reset();
  int icsFetched = 0;
  // 原本想按"上次有日程的日历"跳过空日历省握手，但 PROPFIND 返回的 href 顺序
  // 不保证稳定，索引位图会指向别的日历，直接把日程跳没了（实测 events 6→0）。
  // 稳妥起见永远全量扫描。
  bool fullScan = true;
  (void)maskIn;
  (void)maskAgeSec;
  uint8_t newMask = 0;
  int scanned = 0;
  int skipped = 0;
  int getHref = 0, getFail = 0, noVevent = 0, noSummary = 0;

  for (int i = 0; i < nHref && icsFetched < 24; i++) {
    if (!fetchBudgetLeft()) {
      Serial.println("[cal] 超出时间预算，提前结束");
      break;
    }
    String href = hrefs[i];
    if (!href.endsWith("/")) continue;         // 只处理集合
    if (href.endsWith("/calendar/")) continue; // 跳过 home 自己
    scanned++;
    uint8_t before = parser.count;
    String calUrl = href.startsWith("http") ? href : String(CALDAV_HOST) + href;

    String rep;
    int rc = davRequest(email, pass, calUrl, "REPORT", buildQuery(startUtc, endUtc), &rep);
    Serial.printf("[cal] report=%d (%u bytes)\n", rc, (unsigned)rep.length());
    if (rc <= 0) continue;

    String inlineData;
    if (extractCalendarData(rep, &inlineData)) {
      parser.feed(inlineData);
      icsFetched++;
      if (parser.count > before) newMask |= (1 << i);
      continue;
    }

    String icsHrefs[20];
    int nIcs = 0;
    collectHrefs(rep, icsHrefs, 20, &nIcs);

    String targets[20];
    int nTargets = 0;
    for (int j = 0; j < nIcs && nTargets < 20; j++) {
      if (icsHrefs[j].endsWith("/")) {
        skipped++;
        continue;
      }
      targets[nTargets++] = icsHrefs[j];
    }
    if (nTargets == 0) continue;

    // 企微 CalDAV 的 multiget 一律 403（偶尔 -1），白白多一次大 POST，直接省掉
    for (int j = 0; j < nTargets && icsFetched < 24; j++) {
      if (!fetchBudgetLeft()) break;
      String icsUrl =
          targets[j].startsWith("http") ? targets[j] : String(CALDAV_HOST) + targets[j];
      String body;
      getHref++;
      uint8_t cntBefore = parser.count;
      int gc = davRequest(email, pass, icsUrl, "GET", "", &body);
      if (gc <= 0) {  // 连续请求时服务器偶发抽风，隔 300ms 重试一次
        delay(300);
        gc = davRequest(email, pass, icsUrl, "GET", "", &body);
      }
      if (gc > 0) {
        if (body.indexOf("BEGIN:VEVENT") < 0) noVevent++;
        parser.feed(body);
        icsFetched++;
        if (parser.count == cntBefore) {
          Serial.printf("[ics] 未解析出事件: %s\n", icsUrl.c_str());
        }
      } else {
        getFail++;
        Serial.printf("[ics] 取回失败(%d): %s\n", gc, icsUrl.c_str());
      }
      delay(30);
    }
    if (parser.count > before) newMask |= (1 << i);
  }
  (void)scanned;

  if (!fullScan) newMask |= (maskIn & 0xFF);  // 增量模式保留旧标记
  if (maskOut) *maskOut = newMask;
  Serial.printf("[cal] events=%u ics=%d scan=%d skippedColl=%d href=%d fail=%d noVEVENT=%d "
                "noSUMMARY=%d\n",
                parser.count, icsFetched, scanned, skipped, getHref, getFail, noVevent,
                noSummary);

  for (int i = 0; i < 7; i++) {
    DayInfo &d = w.days[i];
    d.n = 0;
    struct Tmp {
      int minutes;
      uint8_t idx;
    } sorted[8];
    int n = 0;
    for (uint8_t k = 0; k < parser.count && n < 8; k++) {
      if (!IcsParser::occursOn(parser.ev[k], days[i].y, days[i].mo, days[i].d)) continue;
      sorted[n].minutes = parser.ev[k].hh * 60 + parser.ev[k].mm;
      sorted[n].idx = k;
      n++;
    }
    for (int a = 1; a < n; a++) {
      Tmp key = sorted[a];
      int b = a - 1;
      while (b >= 0 && sorted[b].minutes > key.minutes) {
        sorted[b + 1] = sorted[b];
        b--;
      }
      sorted[b + 1] = key;
    }
    for (int j = 0; j < n; j++) {
      const RawEvent &e = parser.ev[sorted[j].idx];
      Ev &tgt = d.ev[d.n++];
      if (e.allDay) {
        strncpy(tgt.t, "全天", sizeof(tgt.t) - 1);
      } else {
        snprintf(tgt.t, sizeof(tgt.t), "%02u:%02u", (unsigned)(e.hh % 24), (unsigned)(e.mm % 60));
      }
      strncpy(tgt.n, e.summary, sizeof(tgt.n) - 1);
    }

    // 去重：同一事件会被重复取回（多个集合里都有它，或 href 重复），
    // 重复条数每次还不一样，导致内容指纹反复变化、每半小时无谓重刷。
    // 按「时间 + 标题」只保留第一条。
    for (uint8_t a = 0; a < d.n; a++) {
      for (uint8_t b = a + 1; b < d.n;) {
        if (strcmp(d.ev[a].t, d.ev[b].t) == 0 && strcmp(d.ev[a].n, d.ev[b].n) == 0) {
          for (uint8_t k = b; k + 1 < d.n; k++) d.ev[k] = d.ev[k + 1];
          d.n--;
        } else {
          b++;
        }
      }
    }
  }
  for (int i = 0; i < 7; i++) {
    Serial.printf("[day] %02d/%02d n=%u:", days[i].mo, days[i].d, w.days[i].n);
    for (uint8_t j = 0; j < w.days[i].n; j++)
      Serial.printf(" %s %s |", w.days[i].ev[j].t, w.days[i].ev[j].n);
    Serial.println();
  }
  return parser.count > 0;
}


// ---------------------------------------------------------------- 和风天气
// 国内可达性比 Open-Meteo 稳，代价是要注册一个免费 key。
// 返回 daily[7]，日期从今天开始；本周已经过去的日子没有数据，保持哨兵值。
static bool fetchWeatherQWeather(const char *host, const char *key, float lat, float lon,
                                 const DayDate days[7], WeekData &w) {
  if (!key[0]) return false;
  String url = String("https://") + host + "/v7/weather/7d?location=" + String(lon, 2) + "," +
               String(lat, 2) + "&key=" + key;

  String body;
  int code = 0;
  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setCACert(CA_QWEATHER);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    if (!http.begin(client, url)) continue;
    code = http.GET();
    if (code == 200) {
      body = http.getString();
      http.end();
      break;
    }
    Serial.printf("[qwx] attempt %d http %d\n", attempt + 1, code);
    http.end();
    delay(500);
  }
  if (code != 200 || !body.length()) {
    Serial.printf("[qwx] fail code=%d\n", code);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[qwx] json fail");
    return false;
  }
  if (strcmp(doc["code"] | "", "200") != 0) {
    Serial.printf("[qwx] api code=%s\n", doc["code"] | "?");
    return false;
  }

  int filled = 0;
  for (JsonObject d : doc["daily"].as<JsonArray>()) {
    const char *fx = d["fxDate"] | "";
    int y, mo, dd;
    if (sscanf(fx, "%d-%d-%d", &y, &mo, &dd) != 3) continue;
    for (int i = 0; i < 7; i++) {
      if (days[i].mo != mo || days[i].d != dd) continue;
      strncpy(w.days[i].cond, d["textDay"] | "阴", sizeof(w.days[i].cond) - 1);
      w.days[i].hi = (int8_t)atoi(d["tempMax"] | "0");
      w.days[i].lo = (int8_t)atoi(d["tempMin"] | "0");
      filled++;
      break;
    }
  }
  Serial.printf("[qwx] filled %d/7\n", filled);
  return filled > 0;
}

// ---------------------------------------------------------------- 天气缓存
// 网络抖动或接口被阻断时，用上次成功的数据兜底：宁可显示昨天的温度，
// 也比整列 -- 好，至少用户知道大致冷热。
static String buildWxCache(const WeekData &w) {
  String s;
  for (int i = 0; i < 7; i++) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%02u%02u,%s,%d,%d;", w.days[i].mon, w.days[i].day,
             w.days[i].cond, w.days[i].lo, w.days[i].hi);
    s += buf;
  }
  return s;
}

static bool applyWxCache(const String &cache, const DayDate days[7], WeekData &w) {
  if (!cache.length()) return false;
  int filled = 0;
  int pos = 0;
  while (pos < (int)cache.length()) {
    int end = cache.indexOf(';', pos);
    if (end < 0) break;
    String item = cache.substring(pos, end);
    pos = end + 1;
    int c1 = item.indexOf(',');
    int c2 = item.indexOf(',', c1 + 1);
    int c3 = item.indexOf(',', c2 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0) continue;
    String md = item.substring(0, c1);
    if (md.length() < 4) continue;
    int mo = md.substring(0, 2).toInt();
    int dd = md.substring(2, 4).toInt();
    int lo = item.substring(c2 + 1, c3).toInt();
    int hi = item.substring(c3 + 1).toInt();
    if (lo <= -99) continue;
    for (int i = 0; i < 7; i++) {
      if (days[i].mo == mo && days[i].d == dd) {
        String cond = item.substring(c1 + 1, c2);
        strncpy(w.days[i].cond, cond.c_str(), sizeof(w.days[i].cond) - 1);
        w.days[i].lo = (int8_t)lo;
        w.days[i].hi = (int8_t)hi;
        filled++;
        break;
      }
    }
  }
  if (filled) Serial.printf("[wx] cache hit %d/7\n", filled);
  return filled > 0;
}


// ---------------------------------------------------------------- 日程缓存
// 网络抖动导致日历取不到时，沿用上一次的日程；真的是"这周没日程"则正常清空。
static String buildCalCache(const WeekData &w) {
  String s;
  for (int i = 0; i < 7; i++) {
    for (int j = 0; j < w.days[i].n; j++) {
      char buf[80];
      snprintf(buf, sizeof(buf), "%02u%02u,%s,%s;", w.days[i].mon, w.days[i].day,
               w.days[i].ev[j].t, w.days[i].ev[j].n);
      s += buf;
    }
  }
  return s;
}

static bool applyCalCache(const String &cache, const DayDate days[7], WeekData &w) {
  if (!cache.length()) return false;
  for (int i = 0; i < 7; i++) w.days[i].n = 0;
  int pos = 0, n = 0;
  while (pos < (int)cache.length()) {
    int end = cache.indexOf(';', pos);
    if (end < 0) break;
    String item = cache.substring(pos, end);
    pos = end + 1;
    int c1 = item.indexOf(',');
    int c2 = item.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) continue;
    int mo = item.substring(0, 2).toInt();
    int dd = item.substring(2, 4).toInt();
    String t = item.substring(c1 + 1, c2);
    String name = item.substring(c2 + 1);
    for (int i = 0; i < 7; i++) {
      if (days[i].mo != mo || days[i].d != dd) continue;
      if (w.days[i].n >= 8) break;
      Ev &e = w.days[i].ev[w.days[i].n++];
      strncpy(e.t, t.c_str(), sizeof(e.t) - 1);
      strncpy(e.n, name.c_str(), sizeof(e.n) - 1);
      n++;
      break;
    }
  }
  if (n) Serial.printf("[cal] 用缓存日程 %d 条\n", n);
  return n > 0;
}
