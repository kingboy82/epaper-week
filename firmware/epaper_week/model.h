#pragma once
#include <Arduino.h>

// 页面数据模型：桥接模式和直连模式共用同一套结构，渲染层不关心数据从哪来。

struct Ev {
  char t[12];
  char n[40];
};

struct DayInfo {
  uint8_t mon, day;
  char cond[16];
  int8_t lo, hi;
  uint8_t n;
  Ev ev[8];
};

struct WeekData {
  uint16_t year;
  uint8_t month;
  uint8_t week;
  uint8_t today;
  uint8_t first_wd;  // 0=周一
  uint8_t ndays;
  char updated[40];
  DayInfo days[7];
  uint8_t viewMon;   // 表头大月份：看下周且跨月时是下个月
  uint8_t nextWeek;  // 1=当前显示的是下周，日期行不再高亮"今天"
};
