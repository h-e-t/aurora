#include "bmp280.h"
#include "neo6m.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.hpp"
#include "soc/gpio_num.h"

#include <cstdio>

namespace {
constexpr char kTag[] = "mekf_firmware";
constexpr char kGpsTag[] = "neo6m_test";
constexpr char kBmpTag[] = "bmp280_test";
constexpr BaseType_t kSensorIoCore = 1;
constexpr gpio_num_t kI2cSdaPin = GPIO_NUM_27;
constexpr gpio_num_t kI2cSclPin = GPIO_NUM_26;
constexpr char kCsvHeader[] =
    "uptime_us,source,valid_mask,latitude_deg,longitude_deg,altitude_m,"
    "speed_mps,course_deg,satellites,hdop,temperature_c,pressure_pa";

struct MonitorContext {
  Neo6M *gps;
  Bmp280 *barometer;
  SdCard *logger;
};

void gps_monitor_task(void *context) {
  auto *monitor = static_cast<MonitorContext *>(context);
  auto *gps = monitor->gps;
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
      if (monitor->logger != nullptr) {
        char record[192]{};
        const unsigned int valid_mask = 1U | (fix.has_altitude() ? 2U : 0U) |
                                        (fix.has_speed() ? 4U : 0U) |
                                        (fix.has_course() ? 8U : 0U) |
                                        (fix.has_satellite_count() ? 16U : 0U) |
                                        (fix.has_hdop() ? 32U : 0U);
        const int length = snprintf(
            record, sizeof(record),
            "%lld,gps,%03x,%.7f,%.7f,%.2f,%.3f,%.2f,%d,%.2f,,",
            static_cast<long long>(esp_timer_get_time()), valid_mask,
            fix.latitude_degrees(), fix.longitude_degrees(),
            fix.has_altitude() ? fix.altitude_meters() : 0.0,
            fix.has_speed() ? fix.speed_meters_per_second() : 0.0,
            fix.has_course() ? fix.course_degrees() : 0.0,
            fix.has_satellite_count() ? static_cast<int>(fix.satellite_count())
                                      : -1,
            fix.has_hdop() ? fix.hdop() : 0.0);
        if (length > 0 && static_cast<size_t>(length) < sizeof(record)) {
          const esp_err_t log_result = monitor->logger->write_line(record);
          if (log_result != ESP_OK && log_result != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(kTag, "Could not write GPS record: %s",
                     esp_err_to_name(log_result));
          }
        }
      }
    } else if (updated) {
      ESP_LOGW(kGpsTag, "GPS reports no valid position fix");
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void bmp280_monitor_task(void *context) {
  auto *monitor = static_cast<MonitorContext *>(context);
  auto *barometer = monitor->barometer;
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
      if (monitor->logger != nullptr) {
        char record[128]{};
        const int length =
            snprintf(record, sizeof(record), "%lld,bmp280,300,,,,,,,,%.2f,%.2f",
                     static_cast<long long>(esp_timer_get_time()),
                     reading.temperature_celsius(), reading.pressure_pascals());
        if (length > 0 && static_cast<size_t>(length) < sizeof(record)) {
          const esp_err_t log_result = monitor->logger->write_line(record);
          if (log_result != ESP_OK && log_result != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(kTag, "Could not write BMP280 record: %s",
                     esp_err_to_name(log_result));
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
} // namespace

extern "C" void app_main() {
  ESP_LOGI(kTag, "Native ESP-IDF MEKF firmware started");

  static SdCard logger;
  SdCard *logger_ptr = nullptr;
  esp_err_t sd_result = logger.init();
  if (sd_result == ESP_OK) {
    sd_result = logger.start_file("flight.csv", kCsvHeader);
  }
  if (sd_result == ESP_OK) {
    logger_ptr = &logger;
    if (logger.session_path()[0] != '\0') {
      ESP_LOGI(kTag, "SD logging session started at %s/flight.csv",
               logger.session_path());
    }
  } else {
    ESP_LOGE(kTag, "SD logging unavailable: %s", esp_err_to_name(sd_result));
  }

  static Neo6M gps;
  ESP_ERROR_CHECK(gps.init());
  // Keep GPS I/O and its logging away from the core reserved for IMU
  // propagation.
  static MonitorContext monitor_context{};
  monitor_context.gps = &gps;
  monitor_context.barometer = nullptr;
  monitor_context.logger = logger_ptr;
  if (xTaskCreatePinnedToCore(gps_monitor_task, "gps_monitor", 4096,
                              &monitor_context, 5, nullptr,
                              kSensorIoCore) != pdPASS) {
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
  monitor_context.barometer = &barometer;
  const esp_err_t bmp_result = barometer.init();
  if (bmp_result != ESP_OK) {
    ESP_LOGE(kBmpTag, "BMP280 initialization failed: %s",
             esp_err_to_name(bmp_result));
    return;
  }
  if (xTaskCreatePinnedToCore(bmp280_monitor_task, "bmp280_monitor", 4096,
                              &monitor_context, 4, nullptr,
                              kSensorIoCore) != pdPASS) {
    ESP_LOGE(kBmpTag, "Could not create BMP280 monitor task");
  }
}
