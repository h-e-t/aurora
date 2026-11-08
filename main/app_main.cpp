#include "bmp280.h"
#include "neo6m.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_num.h"

namespace {
constexpr char kTag[] = "mekf_firmware";
constexpr char kGpsTag[] = "neo6m_test";
constexpr char kBmpTag[] = "bmp280_test";
constexpr BaseType_t kSensorIoCore = 1;
constexpr gpio_num_t kI2cSdaPin = GPIO_NUM_27;
constexpr gpio_num_t kI2cSclPin = GPIO_NUM_26;

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

void bmp280_monitor_task(void *context) {
  auto *barometer = static_cast<Bmp280 *>(context);
  Bmp280Reading reading;

  ESP_LOGI(kBmpTag, "Reading BMP280 over I2C");
  while (true) {
    bool updated = false;
    const esp_err_t result = barometer->read_latest(reading, updated);
    if (result != ESP_OK) {
      ESP_LOGW(kBmpTag, "BMP280 read error: %s", esp_err_to_name(result));
    } else if (updated) {
      ESP_LOGI(kBmpTag, "temperature=%.2f C (%.2f F) pressure=%.2f hPa",
               reading.temperature_celsius(), reading.temperature_fahrenheit(),
               reading.pressure_hectopascals());
    }
    vTaskDelay(pdMS_TO_TICKS(500));
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

  // Share this master bus with the future MPU6050 component. These pins match
  // the board's BMP_SDA (GPIO27) and BMP_SCL (GPIO26) labels.
  i2c_master_bus_config_t i2c_config{};
  i2c_config.i2c_port = I2C_NUM_0;
  i2c_config.sda_io_num = kI2cSdaPin;
  i2c_config.scl_io_num = kI2cSclPin;
  i2c_config.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_config.glitch_ignore_cnt = 7;
  i2c_config.flags.enable_internal_pullup = true;

  i2c_master_bus_handle_t i2c_bus = nullptr;
  const esp_err_t i2c_result = i2c_new_master_bus(&i2c_config, &i2c_bus);
  if (i2c_result != ESP_OK) {
    ESP_LOGE(kBmpTag, "I2C bus initialization failed: %s",
             esp_err_to_name(i2c_result));
    return;
  }

  // CSB high selects I2C; SDO low selects address 0x76.
  static Bmp280 barometer{i2c_bus, Bmp280::ADDRESS_SDO_LOW};
  const esp_err_t bmp_result = barometer.init();
  if (bmp_result != ESP_OK) {
    ESP_LOGE(kBmpTag, "BMP280 initialization failed: %s",
             esp_err_to_name(bmp_result));
    return;
  }
  if (xTaskCreatePinnedToCore(bmp280_monitor_task, "bmp280_monitor", 4096,
                              &barometer, 4, nullptr,
                              kSensorIoCore) != pdPASS) {
    ESP_LOGE(kBmpTag, "Could not create BMP280 monitor task");
  }
}
