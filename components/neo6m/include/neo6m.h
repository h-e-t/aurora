#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sensor.hpp"
#include "soc/gpio_num.h"

inline constexpr size_t GPS_BUFFER_SIZE = 2048;
inline constexpr size_t GPS_NMEA_LINE_MAX = 128;

class GpsFix {
public:
  GpsFix() = default;

  bool valid() const { return valid_; }

  bool has_position() const { return has_position_; }
  double latitude_degrees() const { return latitude_degrees_; }
  double longitude_degrees() const { return longitude_degrees_; }

  bool has_altitude() const { return has_altitude_; }
  double altitude_meters() const { return altitude_meters_; }
  double altitude_feet() const { return altitude_meters_ * 3.280839895; }

  bool has_speed() const { return has_speed_; }
  double speed_knots() const { return speed_knots_; }
  double speed_meters_per_second() const { return speed_knots_ * 0.51444444; }
  double speed_kilometers_per_hour() const { return speed_knots_ * 1.852; }
  double speed_miles_per_hour() const { return speed_knots_ * 1.15077945; }

  bool has_course() const { return has_course_; }
  double course_degrees() const { return course_degrees_; }
  double course_radians() const {
    return course_degrees_ * 0.017453292519943295;
  }

  bool has_satellite_count() const { return has_satellite_count_; }
  uint8_t satellite_count() const { return satellite_count_; }

  bool has_hdop() const { return has_hdop_; }
  double hdop() const { return hdop_; }

private:
  friend class Neo6M;

  bool valid_ = false;
  bool has_position_ = false;
  double latitude_degrees_ = 0;
  double longitude_degrees_ = 0;
  bool has_altitude_ = false;
  double altitude_meters_ = 0;
  bool has_speed_ = false;
  double speed_knots_ = 0;
  bool has_course_ = false;
  double course_degrees_ = 0;
  bool has_satellite_count_ = false;
  uint8_t satellite_count_ = 0;
  bool has_hdop_ = false;
  double hdop_ = 0;
};

class Neo6M : public sensors::Sensor<GpsFix> {
public:
  static constexpr uart_port_t UART_PORT = UART_NUM_2;
  static constexpr gpio_num_t RX_PIN = GPIO_NUM_16;
  static constexpr gpio_num_t TX_PIN = GPIO_NUM_17;

  // Diagnostic helper: returns a complete NMEA sentence without CR/LF.
  esp_err_t read_sentence(char *out, size_t capacity, TickType_t timeout);

protected:
  esp_err_t do_init() override;
  esp_err_t acquire_latest(GpsFix &out, bool &available) override;
  bool is_same_reading(const GpsFix &lhs, const GpsFix &rhs) const override;

private:
  bool parse_sentence(char *sentence);
  bool parse_rmc(char **fields, size_t count);
  bool parse_gga(char **fields, size_t count);

  GpsFix latest_fix_{};
  bool have_fix_ = false;
  char sentence_buffer_[GPS_NMEA_LINE_MAX]{};
  size_t sentence_length_ = 0;
  bool discarding_overlong_sentence_ = false;
};
