#pragma once
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sensor.hpp"
#include "soc/gpio_num.h"
#include <cstddef>
#include <cstdint>

#define GPS_BUFFER_SIZE 2048
#define GPS_NMEA_LINE_MAX 128

struct GpsFix {
  bool valid = false;
  float lat = 0;
  float lon = 0;
  float alt = 0;
  float vel_n = 0;
  float vel_e = 0;
  float vel_d = 0;
};

class Neo6M : public sensors::Sensor<GpsFix> {
public:
  static constexpr uart_port_t UART_PORT = UART_NUM_2;
  static constexpr gpio_num_t RX_PIN = GPIO_NUM_16;
  static constexpr gpio_num_t TX_PIN = GPIO_NUM_17;

  esp_err_t read_sentence(char *out, size_t capacity, TickType_t timeout);

protected:
  esp_err_t do_init() override;
  esp_err_t acquire_latest(GpsFix &out, bool &available) override;
  bool is_same_reading(const GpsFix &lhs, const GpsFix &rhs) const override;

private:
  GpsFix latest_fix_{};
  bool have_fix_ = false;
  char sentence_buffer_[GPS_NMEA_LINE_MAX]{};
  size_t sentence_length_ = 0;
  bool discarding_overlong_sentence_ = false;
};
