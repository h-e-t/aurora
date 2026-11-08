#include "neo6m.h"

esp_err_t Neo6M::do_init() {
  uart_config_t uart_config{};
  uart_config.baud_rate = 9600;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.rx_flow_ctrl_thresh = 0;

  esp_err_t result = uart_param_config(UART_PORT, &uart_config);
  if (result != ESP_OK) {
    return result;
  }

  result = uart_set_pin(UART_PORT, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE);
  if (result != ESP_OK) {
    return result;
  }

  result = uart_driver_install(UART_PORT, GPS_BUFFER_SIZE, 0, 0, nullptr, 0);
  if (result != ESP_OK) {
    return result;
  }

  sentence_length_ = 0;
  discarding_overlong_sentence_ = false;
  return ESP_OK;
}

esp_err_t Neo6M::read_sentence(char *out, size_t capacity, TickType_t timeout) {
  if (out == nullptr || capacity < 2) {
    return ESP_ERR_INVALID_ARG;
  }

  while (true) {
    uint8_t byte = 0;
    if (uart_read_bytes(UART_PORT, &byte, 1, timeout) <= 0) {
      return ESP_ERR_TIMEOUT;
    }

    if (byte == '\n') {
      if (discarding_overlong_sentence_) {
        discarding_overlong_sentence_ = false;
        sentence_length_ = 0;
        return ESP_ERR_INVALID_SIZE;
      }
      if (sentence_length_ == 0) {
        continue;
      }
      if (sentence_length_ >= capacity) {
        sentence_length_ = 0;
        return ESP_ERR_INVALID_SIZE;
      }

      for (size_t i = 0; i < sentence_length_; ++i) {
        out[i] = sentence_buffer_[i];
      }
      out[sentence_length_] = '\0';
      sentence_length_ = 0;
      return ESP_OK;
    }

    if (byte == '\r') {
      continue;
    }
    if (discarding_overlong_sentence_) {
      continue;
    }
    if (sentence_length_ >= sizeof(sentence_buffer_) - 1) {
      sentence_length_ = 0;
      discarding_overlong_sentence_ = true;
      continue;
    }
    sentence_buffer_[sentence_length_++] = static_cast<char>(byte);
  }
}

esp_err_t Neo6M::acquire_latest(GpsFix &out, bool &available) {
  (void)out;
  available = false;
  return ESP_ERR_NOT_SUPPORTED; // NMEA parsing will populate fixes later.
}

bool Neo6M::is_same_reading(const GpsFix &lhs, const GpsFix &rhs) const {
  return lhs.valid == rhs.valid && lhs.lat == rhs.lat && lhs.lon == rhs.lon &&
         lhs.alt == rhs.alt && lhs.vel_n == rhs.vel_n &&
         lhs.vel_e == rhs.vel_e && lhs.vel_d == rhs.vel_d;
}
