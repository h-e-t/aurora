#include "sd_card.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

namespace {
constexpr char kTag[] = "sd_card";
constexpr size_t kMaxFilenameLength = 48;
constexpr int64_t kRetryDelayUs = 2'000'000;

esp_err_t errno_to_esp_err() {
  if (errno == ENOENT) {
    return ESP_ERR_NOT_FOUND;
  }
  if (errno == EEXIST) {
    return ESP_ERR_INVALID_STATE;
  }
  if (errno == ENOMEM) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_FAIL;
}

bool is_safe_name(std::string_view name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  for (const char character : name) {
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '_' || character == '-' || character == '.';
    if (!safe) {
      return false;
    }
  }
  return true;
}

bool is_safe_relative_path(std::string_view path) {
  size_t begin = 0;
  while (begin < path.size()) {
    const size_t end = path.find('/', begin);
    const size_t length =
        end == std::string_view::npos ? path.size() - begin : end - begin;
    if (!is_safe_name(path.substr(begin, length))) {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    begin = end + 1;
  }
  return false;
}

esp_err_t write_all(int descriptor, const void *data, size_t size) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  size_t written = 0;
  while (written < size) {
    const ssize_t result = ::write(descriptor, bytes + written, size - written);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result == 0) {
      return ESP_FAIL;
    }
    if (result < 0) {
      return errno_to_esp_err();
    }
    written += static_cast<size_t>(result);
  }
  return ESP_OK;
}
} // namespace

SdCard::SdCard(const SdCardConfig &config)
    : config_(config), mutex_(xSemaphoreCreateMutex()) {}

SdCard::~SdCard() {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  disconnect_locked();
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
    vSemaphoreDelete(mutex_);
  }
}

esp_err_t SdCard::init(std::string_view session_id) {
  if (mutex_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  if (config_.max_frequency_khz < 400 || config_.max_frequency_khz > 20000 ||
      session_id.size() > sizeof(session_id_) - 1 ||
      (!session_id.empty() && !is_safe_name(session_id))) {
    return ESP_ERR_INVALID_ARG;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (initialized_) {
    xSemaphoreGive(mutex_);
    return ESP_ERR_INVALID_STATE;
  }
  initialized_ = true;
  if (!session_id.empty()) {
    std::memcpy(session_id_, session_id.data(), session_id.size());
    session_id_[session_id.size()] = '\0';
  }

  const esp_err_t result = mount_locked();
  if (result != ESP_OK) {
    retry_after_us_ = esp_timer_get_time() + kRetryDelayUs;
    offline_warning_logged_ = true;
    ESP_LOGW(kTag, "SD card not available yet: %s; will retry on later writes",
             esp_err_to_name(result));
  }
  // A missing card is a supported offline state. The caller can continue and
  // this component retries when later writes arrive.
  xSemaphoreGive(mutex_);
  return ESP_OK;
}

esp_err_t SdCard::mount_locked() {
  spi_bus_config_t bus_config{};
  bus_config.mosi_io_num = config_.mosi;
  bus_config.miso_io_num = config_.miso;
  bus_config.sclk_io_num = config_.clock;
  bus_config.quadwp_io_num = -1;
  bus_config.quadhd_io_num = -1;
  bus_config.max_transfer_sz = 4000;

  esp_err_t result =
      spi_bus_initialize(config_.spi_host, &bus_config, SDSPI_DEFAULT_DMA);
  if (result != ESP_OK) {
    return result;
  }
  spi_bus_initialized_ = true;

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = config_.spi_host;
  host.max_freq_khz = config_.max_frequency_khz;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = config_.chip_select;
  slot_config.host_id = config_.spi_host;

  esp_vfs_fat_sdmmc_mount_config_t mount_config{};
  mount_config.format_if_mount_failed = false;
  mount_config.max_files = 4;
  mount_config.allocation_unit_size = 16 * 1024;

  result = esp_vfs_fat_sdspi_mount(mount_path_, &host, &slot_config,
                                   &mount_config, &card_);
  if (result != ESP_OK) {
    spi_bus_free(config_.spi_host);
    spi_bus_initialized_ = false;
    return result;
  }
  mounted_ = true;

  const std::string_view requested_id(session_id_);
  result = create_session_directory(requested_id);
  if (result != ESP_OK) {
    disconnect_locked();
    return result;
  }

  if (has_active_file_) {
    result = open_active_file_locked();
    if (result != ESP_OK) {
      disconnect_locked();
      return result;
    }
  }

  offline_warning_logged_ = false;
  ESP_LOGI(kTag, "SD card ready; logging session %s", session_path_);
  return ESP_OK;
}

esp_err_t SdCard::ensure_mounted_locked() {
  if (mounted_) {
    return ESP_OK;
  }
  if (!initialized_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (esp_timer_get_time() < retry_after_us_) {
    return ESP_ERR_NOT_FOUND;
  }

  const esp_err_t result = mount_locked();
  if (result != ESP_OK) {
    retry_after_us_ = esp_timer_get_time() + kRetryDelayUs;
    if (!offline_warning_logged_) {
      ESP_LOGW(kTag, "SD card unavailable; writes will retry: %s",
               esp_err_to_name(result));
      offline_warning_logged_ = true;
    }
    return ESP_ERR_NOT_FOUND;
  }
  return ESP_OK;
}

esp_err_t SdCard::create_session_directory(std::string_view session_id) {
  char candidate[sizeof(session_path_)]{};
  if (!session_id.empty()) {
    for (unsigned int suffix = 0; suffix < 1000; ++suffix) {
      const int length =
          suffix == 0
              ? snprintf(candidate, sizeof(candidate), "%s/run_%.*s",
                         mount_path_, static_cast<int>(session_id.size()),
                         session_id.data())
              : snprintf(candidate, sizeof(candidate), "%s/run_%.*s_%03u",
                         mount_path_, static_cast<int>(session_id.size()),
                         session_id.data(), suffix);
      if (length < 0 || static_cast<size_t>(length) >= sizeof(candidate)) {
        return ESP_ERR_INVALID_SIZE;
      }
      if (mkdir(candidate, 0775) == 0) {
        std::strncpy(session_path_, candidate, sizeof(session_path_) - 1);
        return ESP_OK;
      }
      if (errno != EEXIST) {
        return errno_to_esp_err();
      }
    }
    return ESP_ERR_NO_MEM;
  }

  for (unsigned int run = 0; run < 1000000; ++run) {
    const int length =
        snprintf(candidate, sizeof(candidate), "%s/run_%06u", mount_path_, run);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(candidate)) {
      return ESP_ERR_INVALID_SIZE;
    }
    if (mkdir(candidate, 0775) == 0) {
      std::strncpy(session_path_, candidate, sizeof(session_path_) - 1);
      return ESP_OK;
    }
    if (errno != EEXIST) {
      return errno_to_esp_err();
    }
  }
  return ESP_ERR_NO_MEM;
}

esp_err_t SdCard::create_file(std::string_view filename) {
  return start_file(filename, {});
}

esp_err_t SdCard::start_file(std::string_view filename,
                             std::string_view header) {
  if (filename.size() > kMaxFilenameLength || !is_safe_name(filename) ||
      header.size() > sizeof(active_header_)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (mutex_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (file_descriptor_ >= 0) {
    sync_locked();
    close(file_descriptor_);
    file_descriptor_ = -1;
  }
  std::memcpy(active_filename_, filename.data(), filename.size());
  active_filename_[filename.size()] = '\0';
  if (!header.empty()) {
    std::memcpy(active_header_, header.data(), header.size());
  }
  header_size_ = header.size();
  has_active_file_ = true;

  esp_err_t result = ensure_mounted_locked();
  if (result == ESP_OK && file_descriptor_ < 0) {
    result = open_active_file_locked();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE &&
        result != ESP_ERR_INVALID_SIZE) {
      mark_offline_locked();
      result = ESP_ERR_NOT_FOUND;
    }
  }
  if (result == ESP_ERR_NOT_FOUND) {
    result = ESP_OK;
  }
  xSemaphoreGive(mutex_);
  return result;
}

esp_err_t SdCard::open_active_file_locked() {
  char path[sizeof(session_path_) + kMaxFilenameLength + 2]{};
  const int length =
      snprintf(path, sizeof(path), "%s/%s", session_path_, active_filename_);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    return ESP_ERR_INVALID_SIZE;
  }

  file_descriptor_ = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (file_descriptor_ < 0) {
    return errno_to_esp_err();
  }
  if (header_size_ != 0) {
    const esp_err_t result =
        write_all(file_descriptor_, active_header_, header_size_);
    if (result != ESP_OK || fsync(file_descriptor_) != 0) {
      close(file_descriptor_);
      file_descriptor_ = -1;
      return result != ESP_OK ? result : errno_to_esp_err();
    }
    constexpr char newline = '\n';
    if (write_all(file_descriptor_, &newline, sizeof(newline)) != ESP_OK ||
        fsync(file_descriptor_) != 0) {
      close(file_descriptor_);
      file_descriptor_ = -1;
      return ESP_FAIL;
    }
  }
  return ESP_OK;
}

esp_err_t SdCard::write(std::string_view text) {
  return write_bytes(text.data(), text.size());
}

esp_err_t SdCard::write_bytes(const void *data, size_t size) {
  if (data == nullptr && size != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (mutex_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const esp_err_t result = append_locked(data, size, false);
  xSemaphoreGive(mutex_);
  return result;
}

esp_err_t SdCard::write_line(std::string_view text) {
  if (mutex_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const esp_err_t result = append_locked(text.data(), text.size(), true);
  xSemaphoreGive(mutex_);
  return result;
}

esp_err_t SdCard::append_locked(const void *data, size_t size, bool newline) {
  if (!initialized_ || !has_active_file_) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t result = ensure_mounted_locked();
  if (result == ESP_ERR_NOT_FOUND) {
    return queue_pending_locked(data, size, newline);
  }
  if (result != ESP_OK) {
    return result;
  }

  if (pending_size_ != 0) {
    result = drain_pending_locked();
    if (result != ESP_OK) {
      mark_offline_locked();
      return queue_pending_locked(data, size, newline);
    }
  }

  result = write_all(file_descriptor_, data, size);
  if (result == ESP_OK && newline) {
    constexpr char line_ending = '\n';
    result = write_all(file_descriptor_, &line_ending, sizeof(line_ending));
  }
  if (result == ESP_OK && config_.flush_each_write) {
    result = sync_locked();
  }
  if (result != ESP_OK) {
    mark_offline_locked();
    return queue_pending_locked(data, size, newline);
  }
  return ESP_OK;
}

esp_err_t SdCard::queue_pending_locked(const void *data, size_t size,
                                       bool newline) {
  const size_t remaining = pending_buffer_.size() - pending_size_;
  if (size > remaining || (newline && size == remaining)) {
    return ESP_ERR_NO_MEM;
  }
  if (size != 0) {
    std::memcpy(pending_buffer_.data() + pending_size_, data, size);
    pending_size_ += size;
  }
  if (newline) {
    pending_buffer_[pending_size_++] = '\n';
  }
  return ESP_OK;
}

esp_err_t SdCard::drain_pending_locked() {
  if (pending_size_ == 0) {
    return ESP_OK;
  }
  if (!mounted_ || file_descriptor_ < 0) {
    return ESP_ERR_INVALID_STATE;
  }
  const esp_err_t result =
      write_all(file_descriptor_, pending_buffer_.data(), pending_size_);
  if (result != ESP_OK || fsync(file_descriptor_) != 0) {
    return result != ESP_OK ? result : errno_to_esp_err();
  }
  pending_size_ = 0;
  return ESP_OK;
}

esp_err_t SdCard::read_file(std::string_view filename, void *buffer,
                            size_t capacity, size_t &bytes_read) {
  bytes_read = 0;
  if (filename.empty() || filename.size() > 96 ||
      !is_safe_relative_path(filename) ||
      (buffer == nullptr && capacity != 0)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (mutex_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  char path[sizeof(session_path_) + 100]{};
  const int length =
      snprintf(path, sizeof(path), "%s/%.*s", mount_path_,
               static_cast<int>(filename.size()), filename.data());
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    return ESP_ERR_INVALID_SIZE;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  esp_err_t result = ensure_mounted_locked();
  if (result != ESP_OK) {
    xSemaphoreGive(mutex_);
    return result;
  }
  if (pending_size_ != 0) {
    result = drain_pending_locked();
    if (result != ESP_OK) {
      mark_offline_locked();
      xSemaphoreGive(mutex_);
      return ESP_ERR_NOT_FOUND;
    }
  }

  const int descriptor = open(path, O_RDONLY);
  bool io_failed = false;
  if (descriptor < 0) {
    result = errno_to_esp_err();
    io_failed = errno != ENOENT;
  } else {
    auto *bytes = static_cast<unsigned char *>(buffer);
    while (bytes_read < capacity) {
      const ssize_t count =
          ::read(descriptor, bytes + bytes_read, capacity - bytes_read);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        result = errno_to_esp_err();
        io_failed = true;
        break;
      }
      if (count == 0) {
        break;
      }
      bytes_read += static_cast<size_t>(count);
    }
    close(descriptor);
  }
  if (io_failed) {
    mark_offline_locked();
    result = ESP_ERR_NOT_FOUND;
  }
  xSemaphoreGive(mutex_);
  return result;
}

esp_err_t SdCard::sync_locked() {
  if (file_descriptor_ < 0) {
    return ESP_ERR_INVALID_STATE;
  }
  return fsync(file_descriptor_) == 0 ? ESP_OK : errno_to_esp_err();
}

esp_err_t SdCard::flush() {
  if (mutex_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  esp_err_t result = ensure_mounted_locked();
  if (result == ESP_OK) {
    result = drain_pending_locked();
    if (result == ESP_OK) {
      result = sync_locked();
    }
    if (result != ESP_OK) {
      mark_offline_locked();
      result = ESP_ERR_NOT_FOUND;
    }
  }
  xSemaphoreGive(mutex_);
  return result;
}

void SdCard::mark_offline_locked() {
  if (mounted_ && !offline_warning_logged_) {
    ESP_LOGW(kTag,
             "SD write failed; closing this session and waiting for card");
    offline_warning_logged_ = true;
  }
  disconnect_locked();
  retry_after_us_ = esp_timer_get_time() + kRetryDelayUs;
}

void SdCard::disconnect_locked() {
  if (file_descriptor_ >= 0) {
    close(file_descriptor_);
    file_descriptor_ = -1;
  }
  if (mounted_) {
    esp_vfs_fat_sdcard_unmount(mount_path_, card_);
    card_ = nullptr;
    mounted_ = false;
  }
  if (spi_bus_initialized_) {
    spi_bus_free(config_.spi_host);
    spi_bus_initialized_ = false;
  }
}
