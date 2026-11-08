#include "neo6m.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "mekf_firmware";
constexpr char kGpsTag[] = "neo6m_test";

void gps_monitor_task(void *context) {
  auto *gps = static_cast<Neo6M *>(context);
  char sentence[GPS_NMEA_LINE_MAX];

  ESP_LOGI(kGpsTag, "Printing raw GPS NMEA sentences from UART2");
  while (true) {
    const esp_err_t result =
        gps->read_sentence(sentence, sizeof(sentence), pdMS_TO_TICKS(1000));
    if (result == ESP_OK) {
      ESP_LOGI(kGpsTag, "%s", sentence);
    } else if (result != ESP_ERR_TIMEOUT) {
      ESP_LOGW(kGpsTag, "GPS read error: %s", esp_err_to_name(result));
    }
  }
}
} // namespace

extern "C" void app_main() {
  ESP_LOGI(kTag, "Native ESP-IDF MEKF firmware started");

  static Neo6M gps;
  ESP_ERROR_CHECK(gps.init());
  if (xTaskCreate(gps_monitor_task, "gps_monitor", 4096, &gps, 5, nullptr) !=
      pdPASS) {
    ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
  }
}
