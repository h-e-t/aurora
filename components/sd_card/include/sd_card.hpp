#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

struct SdCardConfig {
  spi_host_device_t spi_host = SPI2_HOST;
  gpio_num_t chip_select = GPIO_NUM_2;
  gpio_num_t mosi = GPIO_NUM_23;
  gpio_num_t miso = GPIO_NUM_19;
  gpio_num_t clock = GPIO_NUM_18;
  int max_frequency_khz = 10000;
  bool flush_each_write = true;
};

// SD-card session writer. Each card mount creates a unique run folder so a
// restart or reconnect never truncates a previous run. Calls are serialized.
class SdCard {
public:
  explicit SdCard(const SdCardConfig &config = {});
  ~SdCard();

  SdCard(const SdCard &) = delete;
  SdCard &operator=(const SdCard &) = delete;

  // session_id may be a safe filename fragment such as 20261107T173000Z.
  // When empty, a monotonically numbered folder is selected on the card.
  esp_err_t init(std::string_view session_id = {});

  // Create a new file in this session. Existing files are never overwritten.
  esp_err_t create_file(std::string_view filename);
  // Like create_file(), but remembers and restores the header after a
  // reconnect.
  esp_err_t start_file(std::string_view filename, std::string_view header);
  esp_err_t write(std::string_view text);
  esp_err_t write_bytes(const void *data, size_t size);
  esp_err_t write_line(std::string_view text);
  // Read from any relative path under /sdcard, including previous run folders.
  esp_err_t read_file(std::string_view relative_path, void *buffer,
                      size_t capacity, size_t &bytes_read);
  esp_err_t flush();

  const char *session_path() const { return session_path_; }

private:
  esp_err_t mount_locked();
  esp_err_t ensure_mounted_locked();
  esp_err_t create_session_directory(std::string_view session_id);
  esp_err_t open_active_file_locked();
  esp_err_t append_locked(const void *data, size_t size, bool newline);
  esp_err_t drain_pending_locked();
  esp_err_t queue_pending_locked(const void *data, size_t size, bool newline);
  esp_err_t sync_locked();
  void disconnect_locked();
  void mark_offline_locked();

  SdCardConfig config_;
  sdmmc_card_t *card_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  int file_descriptor_ = -1;
  size_t header_size_ = 0;
  bool initialized_ = false;
  bool mounted_ = false;
  bool spi_bus_initialized_ = false;
  bool has_active_file_ = false;
  bool offline_warning_logged_ = false;
  int64_t retry_after_us_ = 0;
  static constexpr size_t kPendingBufferCapacity = 8192;
  std::array<uint8_t, kPendingBufferCapacity> pending_buffer_{};
  size_t pending_size_ = 0;
  char mount_path_[16] = "/sdcard";
  char session_path_[96]{};
  char session_id_[65]{};
  char active_filename_[49]{};
  char active_header_[256]{};
};
