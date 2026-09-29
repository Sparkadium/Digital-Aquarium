#pragma once
#include <Arduino_GFX_Library.h>
#include <stdlib.h>
#include "driver/spi_master.h"
#include "esp_memory_utils.h"

// ESP32-C6 SPI2 owner from panel initialization onward: no live Arduino/IDF handoff.
// One framebuffer, four bounded DMA transfers (the C6 has an 18-bit transfer length). Only simulation/cache preparation may overlap DMA.
class GlowtankDmaBus final : public Arduino_DataBus {
 public:
  bool begin(int32_t hz = SPI_DEFAULT_FREQ, int8_t mode = GFX_NOT_DEFINED) override {
    if (dev_) return error_ == ESP_OK;
    pinMode(LCD_DC, OUTPUT); pinMode(LCD_CS, OUTPUT);
    digitalWrite(LCD_DC, HIGH); digitalWrite(LCD_CS, HIGH);
    spi_bus_config_t bc = {};
    bc.mosi_io_num = LCD_MOSI; bc.miso_io_num = -1; bc.sclk_io_num = LCD_SCK;
    bc.quadwp_io_num = bc.quadhd_io_num = -1;
    bc.data4_io_num = bc.data5_io_num = bc.data6_io_num = bc.data7_io_num = -1;
    bc.max_transfer_sz = ChunkBytes;
    error_ = spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO);
    if (error_ != ESP_OK) return false;
    ownsBus_ = true;
    spi_device_interface_config_t dc = {};
    dc.clock_speed_hz = hz;
    dc.mode = mode == GFX_NOT_DEFINED ? SPI_MODE0 : mode;
    dc.spics_io_num = -1; // Manual CS spans window commands, data and the complete frame.
    dc.queue_size = FrameChunks;
    dc.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;
    error_ = spi_bus_add_device(SPI2_HOST, &dc, &dev_);
    if (error_ != ESP_OK) { close(); return false; }
    return true;
  }
  ~GlowtankDmaBus() { close(); }
  bool ok() const { return error_ == ESP_OK; }
  esp_err_t error() const { return error_; }
  void beginWrite() override {
    if (!ok()) return;
    error_ = spi_device_acquire_bus(dev_, portMAX_DELAY);
    if (!ok()) return;
    acquired_ = true;
    digitalWrite(LCD_CS, LOW);
  }
  void endWrite() override {
    if (!waitFrame() && pending_) return;
    digitalWrite(LCD_CS, HIGH);
    if (acquired_) { spi_device_release_bus(dev_); acquired_ = false; }
  }
  void writeCommand(uint8_t c) override { writeCommandBytes(&c, 1); }
  void writeCommand16(uint16_t c) override {
    uint8_t b[2] = { uint8_t(c >> 8), uint8_t(c) }; writeCommandBytes(b, 2);
  }
  void writeCommandBytes(uint8_t *b, uint32_t n) override {
    if (!ok()) return;
    digitalWrite(LCD_DC, LOW); writeBytes(b, n); digitalWrite(LCD_DC, HIGH);
  }
  void write(uint8_t c) override { writeBytes(&c, 1); }
  void write16(uint16_t c) override {
    uint8_t b[2] = { uint8_t(c >> 8), uint8_t(c) }; writeBytes(b, 2);
  }
  void writeBytes(uint8_t *b, uint32_t n) override {
    if (!ok() || !n) return;
    while (n && ok()) {
      uint32_t k = n < ChunkBytes ? n : ChunkBytes;
      spi_transaction_t t = {}; t.length = k * 8;
      if (k <= sizeof(t.tx_data)) { t.flags = SPI_TRANS_USE_TXDATA; memcpy(t.tx_data, b, k); }
      else t.tx_buffer = b;
      error_ = spi_device_polling_transmit(dev_, &t);
      b += k; n -= k;
    }
  }
  void writeRepeat(uint16_t p, uint32_t n) override {
    alignas(4) uint16_t buf[128];
    for (auto &v : buf) v = uint16_t((p << 8) | (p >> 8));
    while (n && ok()) { uint32_t k = n < 128 ? n : 128; writeBytes((uint8_t *)buf, k * 2); n -= k; }
  }
  void writePixels(uint16_t *p, uint32_t n) override {
    alignas(4) uint16_t buf[128];
    while (n && ok()) {
      uint32_t k = n < 128 ? n : 128;
      for (uint32_t i = 0; i < k; i++) buf[i] = uint16_t((p[i] << 8) | (p[i] >> 8));
      writeBytes((uint8_t *)buf, k * 2); p += k; n -= k;
    }
  }
  bool queueFrame(uint16_t *fb) {
    if (!ok() || pending_ || !acquired_) return false;
    // Never silently allocate a second framebuffer for DMA.
    if ((uintptr_t(fb) & 3) || !esp_ptr_dma_capable(fb)) { error_ = ESP_ERR_INVALID_ARG; return false; }
    uint8_t *p = (uint8_t *)fb;
    uint32_t left = FrameBytes; completed_ = 0;
    for (int i = 0; left; i++) {
      uint32_t n = left < ChunkBytes ? left : ChunkBytes;
      frame_[i] = spi_transaction_t();
      frame_[i].length = n * 8; frame_[i].tx_buffer = p;
      error_ = spi_device_queue_trans(dev_, &frame_[i], portMAX_DELAY);
      if (!ok()) return false;
      pending_++; // Count only transactions the driver accepted.
      p += n; left -= n;
    }
    return true;
  }
  bool waitFrame() {
    while (pending_) {
      spi_transaction_t *done = nullptr;
      esp_err_t e = spi_device_get_trans_result(dev_, &done, portMAX_DELAY);
      if (e != ESP_OK || done != &frame_[completed_]) { error_ = e == ESP_OK ? ESP_FAIL : e; return false; }
      pending_--; completed_++;
    }
    return ok();
  }
 private:
  spi_device_handle_t dev_ = nullptr;
  static const uint32_t ChunkBytes = 32000;
  static const uint32_t FrameBytes = LCD_W * LCD_H * 2;
  static const int FrameChunks = (FrameBytes + ChunkBytes - 1) / ChunkBytes;
  static_assert(ChunkBytes * 8 <= (1u << 18), "C6 SPI transfer length overflow");
  static_assert(ChunkBytes % 4 == 0 && FrameBytes % 4 == 0, "DMA requires word alignment");
  spi_transaction_t frame_[FrameChunks] = {};
  int pending_ = 0, completed_ = 0;
  esp_err_t error_ = ESP_OK;
  bool ownsBus_ = false, acquired_ = false;
  void close() {
    if (pending_) waitFrame();
    if (pending_) abort(); // A destructor must never free a transaction still owned by DMA.
    if (acquired_) { digitalWrite(LCD_CS, HIGH); spi_device_release_bus(dev_); acquired_ = false; }
    if (dev_) { spi_bus_remove_device(dev_); dev_ = nullptr; }
    if (ownsBus_) { spi_bus_free(SPI2_HOST); ownsBus_ = false; }
  }
};
