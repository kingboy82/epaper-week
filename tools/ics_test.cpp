// ICS 解析与重复规则展开的主机测试。
// 用 tools/host/arduino_shim.h 模拟 Arduino 的 String，直接跑 ics.h。
//
//   clang++ -std=c++17 -Itools/host -Ifirmware/epaper_week tools/ics_test.cpp -o /tmp/ics && /tmp/ics
#include <cstdio>
#include <cstring>

#include "arduino_shim.h"
#include "ics.h"

static int failures = 0;

static void check(const char *what, bool ok) {
  printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failures++;
}

static const char *SAMPLE =
    "BEGIN:VCALENDAR\r\n"
    "BEGIN:VEVENT\r\n"
    "SUMMARY:周会\r\n"
    "DTSTART;TZID=Asia/Shanghai:20261109T093000\r\n"
    "DTEND;TZID=Asia/Shanghai:20261109T103000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "SUMMARY:产品需求评审会\r\n"
    "DTSTART;TZID=Asia/Shanghai:20261110T140000\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "SUMMARY:跨时区的会\r\n"
    "DTSTART:20261111T013000Z\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "SUMMARY:已取消\r\n"
    "DTSTART;TZID=Asia/Shanghai:20261112T100000\r\n"
    "STATUS:CANCELLED\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "SUMMARY:全天培训\r\n"
    "DTSTART;VALUE=DATE:20261113\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

int main() {
  IcsParser p;
  p.feed(String(SAMPLE));

  printf("解析:\n");
  check("事件数 = 4（已取消的被丢弃）", p.count == 4);

  bool foundWeekly = false, foundReview = false, foundUtc = false, foundAllDay = false;
  for (uint8_t i = 0; i < p.count; i++) {
    const RawEvent &e = p.ev[i];
    if (!strcmp(e.summary, "周会")) {
      foundWeekly = (e.hh == 9 && e.mm == 30 && e.freq == 2 && e.byday == 0x01);
    } else if (!strcmp(e.summary, "产品需求评审会")) {
      foundReview = (e.mo == 11 && e.d == 10 && e.hh == 14);
    } else if (!strcmp(e.summary, "跨时区的会")) {
      foundUtc = (e.mo == 11 && e.d == 11 && e.hh == 9 && e.mm == 30);  // UTC 01:30 -> 09:30
    } else if (!strcmp(e.summary, "全天培训")) {
      foundAllDay = e.allDay;
    }
  }
  check("每周重复识别正确（BYDAY=MO）", foundWeekly);
  check("普通事件日期时间正确", foundReview);
  check("UTC 转东八区正确（01:30Z -> 09:30）", foundUtc);
  check("全天事件标记正确", foundAllDay);

  printf("\n重复展开:\n");
  const RawEvent *weekly = nullptr;
  for (uint8_t i = 0; i < p.count; i++)
    if (!strcmp(p.ev[i].summary, "周会")) weekly = &p.ev[i];

  check("11/9 当天出现", IcsParser::occursOn(*weekly, 2026, 11, 9));
  check("11/16 下一周出现", IcsParser::occursOn(*weekly, 2026, 11, 16));
  check("11/17 周二不出现", !IcsParser::occursOn(*weekly, 2026, 11, 17));
  check("11/2 早于起点不出现", !IcsParser::occursOn(*weekly, 2026, 11, 2));

  printf("\n星期计算:\n");
  check("2026-11-09 是周一", IcsParser::weekdayMon0(2026, 11, 9) == 0);
  check("2026-11-15 是周日", IcsParser::weekdayMon0(2026, 11, 15) == 6);
  check("2026-11-16 是周一", IcsParser::weekdayMon0(2026, 11, 16) == 0);

  printf("\n折行与转义:\n");
  IcsParser p2;
  p2.feed(String(
      "BEGIN:VCALENDAR\r\n"
      "BEGIN:VEVENT\r\n"
      "SUMMARY:这是一个很长的会议标题\r\n"
      " 需要折行显示\r\n"
      "DTSTART;TZID=Asia/Shanghai:20261114T100000\r\n"
      "END:VEVENT\r\n"
      "END:VCALENDAR\r\n"));
  const char *full = "这是一个很长的会议标题需要折行显示";
  bool prefixOk = p2.count == 1 && !strncmp(p2.ev[0].summary, full, strlen(p2.ev[0].summary));
  size_t len = p2.count ? strlen(p2.ev[0].summary) : 0;
  check("折行合并后是完整标题的前缀（超长按字节截断）",
        prefixOk && len >= 30 && len <= 47);
  check("截断点没有切断 UTF-8 多字节字符", len % 3 == 0);

  printf("\n带 UNTIL 的重复:\n");
  IcsParser p3;
  p3.feed(String(
      "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nSUMMARY:晨会\r\n"
      "DTSTART;TZID=Asia/Shanghai:20261109T090000\r\n"
      "RRULE:FREQ=DAILY;INTERVAL=1;UNTIL=20261111T235959Z\r\n"
      "END:VEVENT\r\nEND:VCALENDAR\r\n"));
  check("11/10 在范围内", IcsParser::occursOn(p3.ev[0], 2026, 11, 10));
  check("11/12 超出 UNTIL", !IcsParser::occursOn(p3.ev[0], 2026, 11, 12));

  printf("\n%s（%d 项失败）\n", failures ? "FAIL" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
