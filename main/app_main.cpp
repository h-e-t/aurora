#include "neo6m.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "mekf_firmware";
constexpr char kGpsTag[] = "neo6m_test";
constexpr BaseType_t kSensorIoCore = 1;

void gps_monitor_task(void *context) {
  auto *gps = static_cast<Neo6M *>(context);
  GpsFix fix;

  ESP_LOGI(kGpsTag, "Waiting for parsed GPS fixes on UART2");
  while (true) {
    bool updated = false;
    const esp_err_t result = gps->read_latest(fix, updated);
    if (result != ESP_OK) {
      ESP_LOGW(kGpsTag, "GPS read error: %s", esp_err_to_name(result));
    } else if (updated && fix.valid() && fix.has_position()) {
      if (fix.has_altitude() && fix.has_speed() && fix.has_course()) {
        ESP_LOGI(kGpsTag,
                 "lat=%.6f deg lon=%.6f deg alt=%.1f m speed=%.2f m/s "
                 "(%.2f km/h) course=%.1f deg sats=%u HDOP=%.2f",
                 fix.latitude_degrees(), fix.longitude_degrees(),
                 fix.altitude_meters(), fix.speed_meters_per_second(),
                 fix.speed_kilometers_per_hour(), fix.course_degrees(),
                 fix.has_satellite_count()
                     ? static_cast<unsigned>(fix.satellite_count())
                     : 0U,
                 fix.has_hdop() ? fix.hdop() : 0.0);
      } else {
        ESP_LOGI(kGpsTag, "lat=%.6f deg lon=%.6f deg (partial fix)",
                 fix.latitude_degrees(), fix.longitude_degrees());
      }
    } else if (updated) {
      ESP_LOGW(kGpsTag, "GPS reports no valid position fix");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
} // namespace

extern "C" void app_main() {
  ESP_LOGI(kTag, "Native ESP-IDF MEKF firmware started");

  static Neo6M gps;
  ESP_ERROR_CHECK(gps.init());
  // Keep GPS I/O and its logging away from the core reserved for IMU
  // propagation.
  if (xTaskCreatePinnedToCore(gps_monitor_task, "gps_monitor", 4096, &gps, 5,
                              nullptr, kSensorIoCore) != pdPASS) {
    ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
  }
}
