// 临时工具：连续记录候选 ADC 脚的最小/最大值到 NVS。
// 用法：拔掉 USB 让设备靠电池跑一会儿，再插回 —— 开机时会把"电池供电期间"的记录打出来，
// 与当下（USB 供电）对比，变化明显的那个脚就是电池采样脚。
#include <Arduino.h>
#include <Preferences.h>

static const int PINS[] = {36, 32, 33, 25, 26, 27, 15, 14, 12, 13};
static const int N = sizeof(PINS) / sizeof(PINS[0]);

static uint16_t mn[N], mx[N];
static Preferences prefs;
static uint32_t tSave = 0;

static void printPrev() {
  size_t len = prefs.getBytesLength("mn");
  if (len != N * sizeof(uint16_t)) {
    Serial.println("[rec] 上一次没有记录");
    return;
  }
  uint16_t pmn[N], pmx[N];
  prefs.getBytes("mn", pmn, sizeof(pmn));
  prefs.getBytes("mx", pmx, sizeof(pmx));
  Serial.println("[rec] 上次运行期间（可能是电池供电）:");
  for (int i = 0; i < N; i++) {
    Serial.printf("[rec]   GPIO%-2d min=%4u max=%4u 跨度=%u\n", PINS[i], pmn[i], pmx[i],
                  (unsigned)(pmx[i] - pmn[i]));
  }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  prefs.begin("adc", false);
  uint8_t boots = prefs.getUChar("n", 0) + 1;
  prefs.putUChar("n", boots);
  Serial.printf("[rec] 第 %u 次运行\n", boots);
  printPrev();

  for (int i = 0; i < N; i++) {
    uint32_t mv = analogReadMilliVolts(PINS[i]);
    mn[i] = mv;
    mx[i] = mv;
  }
  Serial.println("[rec] 开始记录，可以拔掉 USB 了");
}

void loop() {
  for (int i = 0; i < N; i++) {
    uint32_t mv = analogReadMilliVolts(PINS[i]);
    if (mv < mn[i]) mn[i] = mv;
    if (mv > mx[i]) mx[i] = mv;
  }
  if (millis() - tSave > 8000) {
    tSave = millis();
    prefs.putBytes("mn", mn, sizeof(mn));
    prefs.putBytes("mx", mx, sizeof(mx));
    Serial.print("[rec] 当前:");
    for (int i = 0; i < N; i++) Serial.printf(" %d=%u/%u", PINS[i], mn[i], mx[i]);
    Serial.println();
  }
  delay(150);
}
