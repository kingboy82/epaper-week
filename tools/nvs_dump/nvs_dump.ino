// NVS 体检：列出所有命名空间/键/类型，用于确认配网凭据是否真的丢了。
#include <Arduino.h>
#include <nvs.h>
#include <nvs_flash.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  esp_err_t err = nvs_flash_init();
  Serial.printf("[nvs] init=%d (%s)\n", (int)err, esp_err_to_name(err));

  nvs_stats_t st;
  if (nvs_get_stats(NULL, &st) == ESP_OK) {
    Serial.printf("[nvs] used=%u free=%u total=%u ns=%u\n", (unsigned)st.used_entries,
                  (unsigned)st.free_entries, (unsigned)st.total_entries,
                  (unsigned)st.namespace_count);
  }

  nvs_iterator_t it = nullptr;
  esp_err_t r = nvs_entry_find("nvs", nullptr, NVS_TYPE_ANY, &it);
  int n = 0;
  while (r == ESP_OK && it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    Serial.printf("[key] %s / %s  type=%d\n", info.namespace_name, info.key,
                  (int)info.type);
    n++;
    r = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  Serial.printf("[nvs] 共 %d 个键\n", n);
  Serial.println("[nvs] DONE");
}

void loop() {}
