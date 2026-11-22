#pragma once
#include <Arduino.h>

// 板上 4 个按键的实测结论（2026-11-18，逐键验证）：
//   1 号（最上） = EN 硬复位，按下去直接重启，没有 GPIO 事件
//   2 号         = GPIO34，外部上拉，按下为低
//   3 号         = GPIO35，同上
//   4 号（最下） = GPIO39，同上；deep sleep 期间用它唤醒
// 34/35/39 都是 input-only 脚，没有内部上拉，只能 pinMode(INPUT) 靠板上电阻。

static const int KEY_REFRESH = 34;  // 2 号键：短按立即刷新
static const int KEY_WEEK = 35;     // 3 号键：短按切换本周/下周
static const int KEY_PORTAL = 39;   // 4 号键：短按唤醒；5 秒信息页；30 秒配网

enum KeyAction { ACT_NONE = 0, ACT_REFRESH, ACT_TOGGLE_WEEK, ACT_INFO, ACT_PORTAL };

static const int KEY_COUNT = 3;
static const int KEY_PINS[KEY_COUNT] = {KEY_REFRESH, KEY_WEEK, KEY_PORTAL};

static const uint32_t HOLD_SHORT = 1500;    // 小于此值算短按
static const uint32_t HOLD_INFO = 5000;     // 4 号键：进/出信息页
static const uint32_t HOLD_PORTAL = 30000;  // 4 号键：进配网

struct BtnState {
  uint8_t stable;    // 1=当前按下
  uint8_t cnt;       // 去抖计数
  uint32_t tDown;    // 按下时刻
  bool infoDone;     // 5 秒动作已触发
  bool portalDone;   // 30 秒动作已触发
  bool armed;        // 是否已松过手：唤醒那一下按住的不算长按
};

static BtnState g_btn[KEY_COUNT];

static void keysBegin() {
  for (int i = 0; i < KEY_COUNT; i++) {
    pinMode(KEY_PINS[i], INPUT);  // input-only 脚没有内部上拉，写 PULLUP 只会报错
    g_btn[i].stable = digitalRead(KEY_PINS[i]) == LOW ? 1 : 0;
    g_btn[i].cnt = 0;
    g_btn[i].tDown = millis();
    g_btn[i].infoDone = false;
    g_btn[i].portalDone = false;
    // 唤醒那一下会读成"一直按着"，不排掉就会在 5 秒后误触发信息页
    g_btn[i].armed = (g_btn[i].stable == 0);
  }
}

// 扫描一次；需要每 8~10ms 调一次。长按在按住过程中就返回，短按在松手时返回。
static int keysScan() {
  for (int i = 0; i < KEY_COUNT; i++) {
    bool raw = digitalRead(KEY_PINS[i]) == LOW;
    if (raw == (bool)g_btn[i].stable) {
      g_btn[i].cnt = 0;
      continue;
    }
    if (++g_btn[i].cnt < 3) continue;  // 3 x ~8ms ≈ 25ms 去抖
    g_btn[i].cnt = 0;
    g_btn[i].stable = raw ? 1 : 0;
    if (raw) {
      g_btn[i].tDown = millis();
      g_btn[i].infoDone = false;
      g_btn[i].portalDone = false;
    } else {
      g_btn[i].armed = true;  // 松手后才允许下一次按算数
      uint32_t held = millis() - g_btn[i].tDown;
      if (held < HOLD_SHORT) {
        if (i == 0) return ACT_REFRESH;      // 2 号键
        if (i == 1) return ACT_TOGGLE_WEEK;  // 3 号键
        // 4 号键短按只负责唤醒，不做别的事
      }
    }
  }

  // 4 号键：按住 5 秒进/出信息页，继续按到 30 秒进配网
  BtnState &p = g_btn[2];
  if (p.stable && p.armed) {
    uint32_t held = millis() - p.tDown;
    if (held >= HOLD_PORTAL && !p.portalDone) {
      p.portalDone = true;
      p.infoDone = true;
      return ACT_PORTAL;
    }
    if (held >= HOLD_INFO && !p.infoDone) {
      p.infoDone = true;
      return ACT_INFO;
    }
  }
  return ACT_NONE;
}

// 阻塞操作（取数、刷屏、配网）结束后调用：把长按计时重新归零。
// 否则"按住 5 秒退出信息页"这一下，会连同后面 2 分钟取数一起被算成按住 30 秒，误进配网。
static void keysReArm() {
  for (int i = 0; i < KEY_COUNT; i++) {
    g_btn[i].tDown = millis();
    g_btn[i].cnt = 0;
    g_btn[i].infoDone = false;
    g_btn[i].portalDone = false;
  }
}

// 轮询一段时间，返回第一个动作；超时返回 ACT_NONE。
static int keysWait(uint32_t windowMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < windowMs) {
    int a = keysScan();
    if (a != ACT_NONE) return a;
    delay(8);
  }
  return ACT_NONE;
}
