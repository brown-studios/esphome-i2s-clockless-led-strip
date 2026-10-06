#include "i2s_clockless_led_strip.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"

#include <esp_attr.h>
#include <driver/i2s_tdm.h>

namespace esphome::i2s_clockless_led_strip {

constexpr const char *const TAG = "i2s_clockless_led_strip";

constexpr const char *const ERROR_ALLOCATION = "Allocation error";
constexpr const char *const ERROR_I2S = "I2S error";

// These values are hardcoded for an LED strip at 800 kbps.
//
// Each bit of LED strip data gets expanded into 3 bits of I2C data, where a 0-bit expands into 100
// (1/3 duty cycle) and a 1-bit expands into 110 (2/3 duty cycle.  Each byte of LED strip data becomes
// one 24-bit I2S sample to be transmitted at a rate of 100000 samples per second in I2C TDM mode.
constexpr size_t I2S_BYTES_PER_SAMPLE = 3;
constexpr uint32_t I2S_SAMPLE_RATE_HZ = 100000;

// The number of I2S samples to write after the LED data to encode a 50 microsecond LED strip reset signal.
constexpr uint32_t I2S_RESET_SAMPLES = 50 * 1000000 / I2S_SAMPLE_RATE_HZ;

constexpr size_t calc_i2s_samples_with_padding(size_t color_data_bytes) {
  // Round up to next multiple of 3 as required by the ESP-IDF programming guide for `dma_frame_num`
  // when using 24-bit samples.
  return (color_data_bytes + I2S_RESET_SAMPLES + 2) / 3 * 3;
}

I2SClocklessLedStrip::I2SClocklessLedStrip(uint8_t pin, uint16_t num_leds, light::ChannelColors channel_colors)
    : pin_(pin),
      num_leds_(num_leds),
      channel_colors_(channel_colors),
      color_data_bytes_(num_leds * channel_colors_.bytes_per_led()) {
}

void I2SClocklessLedStrip::dump_config() {
  ESP_LOGCONFIG(TAG,
      "I2S Clockless LED Strip:\n"
      "  Pin: %u",
      this->pin_);
  char channel_colors[5];
  ESP_LOGCONFIG(TAG,
      "  Channel colors: %s\n"
      "  Number of LEDs: %u",
      this->channel_colors_.to_string(channel_colors), this->num_leds_);
}

float I2SClocklessLedStrip::get_setup_priority() const {
  return setup_priority::IO;
}

void I2SClocklessLedStrip::setup() {
  const size_t i2s_data_bytes = this->color_data_bytes_ * I2S_BYTES_PER_SAMPLE;

  RAMAllocator<uint8_t> allocator;
  if ((this->i2s_data_ = allocator.allocate(i2s_data_bytes)) == nullptr ||
      (this->color_data_ = allocator.allocate(this->color_data_bytes_)) == nullptr ||
      (this->effect_data_ = allocator.allocate(this->num_leds_)) == nullptr) {
    allocator.deallocate(this->i2s_data_, i2s_data_bytes);
    allocator.deallocate(this->color_data_, this->color_data_bytes_);
    allocator.deallocate(this->effect_data_, this->num_leds_);
    this->mark_failed(LOG_STR(ERROR_ALLOCATION));
    return;
  }
  memset(this->i2s_data_, 0, i2s_data_bytes);
  memset(this->color_data_, 0, this->color_data_bytes_);
  memset(this->effect_data_, 0, this->num_leds_);

  i2s_chan_config_t chan_config = {
      .id = I2S_NUM_AUTO,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = calc_i2s_samples_with_padding(this->color_data_bytes_),
      .auto_clear_after_cb = false,
      .auto_clear_before_cb = false,
      .allow_pd = false,
      .intr_priority = 0,
  };
  esp_err_t err;
  if ((err = i2s_new_channel(&chan_config, &this->tx_handle_, NULL)) != ESP_OK) {
    ESP_LOGE(TAG, "Error in i2s_new_channel: %s", esp_err_to_name(err));
    this->mark_failed(LOG_STR(ERROR_I2S));
    return;
  }

  i2s_tdm_config_t tdm_config = {
      .clk_cfg =
          {
              .sample_rate_hz = I2S_SAMPLE_RATE_HZ,
              .clk_src = I2S_CLK_SRC_DEFAULT,
              .mclk_multiple = I2S_MCLK_MULTIPLE_384,
          },
      .slot_cfg =
          {
              .data_bit_width = I2S_DATA_BIT_WIDTH_24BIT,
              .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
              .slot_mode = I2S_SLOT_MODE_MONO,
              .slot_mask = I2S_TDM_SLOT0,
              .ws_width = 1,
              .big_endian = true,
              .total_slot = I2S_TDM_AUTO_SLOT_NUM,
          },
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = I2S_GPIO_UNUSED,
              .ws = I2S_GPIO_UNUSED,
              .dout = gpio_num_t(this->pin_),
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  if ((err = i2s_channel_init_tdm_mode(this->tx_handle_, &tdm_config)) != ESP_OK) {
    ESP_LOGE(TAG, "Error in i2s_channel_init_tdm_mode: %s", esp_err_to_name(err));
    this->mark_failed(LOG_STR(ERROR_I2S));
    return;
  }

  i2s_event_callbacks_t event_callbacks = {
      .on_sent = i2s_on_sent_callback,
  };
  if ((err = i2s_channel_register_event_callback(this->tx_handle_, &event_callbacks, this)) != ESP_OK) {
    ESP_LOGE(TAG, "Error in i2s_channel_register_event_callback: %s", esp_err_to_name(err));
    this->mark_failed(LOG_STR(ERROR_I2S));
    return;
  }

  if ((err = i2s_channel_enable(this->tx_handle_)) != ESP_OK) {
    ESP_LOGE(TAG, "Error in i2s_channel_enable: %s", esp_err_to_name(err));
    this->mark_failed(LOG_STR(ERROR_I2S));
    return;
  }
}

light::LightTraits I2SClocklessLedStrip::get_traits() {
  auto traits = light::LightTraits();
  if (this->channel_colors_.has_white()) {
    traits.set_supported_color_modes({light::ColorMode::RGB_WHITE, light::ColorMode::WHITE});
  } else {
    traits.set_supported_color_modes({light::ColorMode::RGB});
  }
  return traits;
}

void I2SClocklessLedStrip::write_state(light::LightState *state) {
  if (this->is_failed())
    return;

  if (this->i2s_data_ready_.load(std::memory_order_acquire)) {
    // Busy sending the last frame, try again later.
    this->schedule_show();
    return;
  }

  const size_t color_data_bytes = this->color_data_bytes_;
  uint8_t *color_data = this->color_data_;
  uint8_t *i2s_data = this->i2s_data_;
  for (size_t i = 0; i < color_data_bytes; i++) {
    const uint8_t color_byte = *(color_data++);
    *(i2s_data++) = 0b10010010 | ((color_byte & 0x80) >> 1) | ((color_byte & 0x40) >> 3) | ((color_byte & 0x20) >> 5);
    *(i2s_data++) = 0b01001001 | ((color_byte & 0x10) << 1) | ((color_byte & 0x08) >> 1);
    *(i2s_data++) = 0b00100100 | ((color_byte & 0x04) << 5) | ((color_byte & 0x02) << 3) | ((color_byte & 0x01) << 1);
  }

  this->i2s_data_ready_.store(true, std::memory_order_release);
  this->mark_shown_();
}

bool IRAM_ATTR HOT I2SClocklessLedStrip::i2s_on_sent_callback(
    i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
  const auto self = static_cast<I2SClocklessLedStrip *>(user_ctx);

  const size_t i2s_data_bytes = self->color_data_bytes_ * I2S_BYTES_PER_SAMPLE;
  if (self->i2s_data_ready_.load(std::memory_order_acquire)) {
    memcpy(event->dma_buf, self->i2s_data_, i2s_data_bytes);
    self->i2s_data_ready_.store(false, std::memory_order_release);
  } else {
    memset(event->dma_buf, 0, i2s_data_bytes);
  }
  return false;
}

void I2SClocklessLedStrip::clear_effect_data() {
  memset(this->effect_data_, 0, this->num_leds_);
}

light::ESPColorView I2SClocklessLedStrip::get_view_internal(int32_t index) const {
  const light::ChannelColors &colors = this->channel_colors_;
  uint8_t *led = this->color_data_ + (index * colors.bytes_per_led());
  return {led + colors.r, led + colors.g, led + colors.b, colors.has_white() ? led + colors.w : nullptr,
      &this->effect_data_[index], &this->correction_};
}

}  // namespace esphome::i2s_clockless_led_strip

#endif  // USE_ESP32
