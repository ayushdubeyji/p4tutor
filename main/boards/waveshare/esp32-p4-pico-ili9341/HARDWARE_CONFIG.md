# ESP32-P4-Pico ILI9341 Hardware Configuration Guide

This document records the exact hardware configuration and quirks required to drive the Waveshare ESP32-P4-Pico with an 8-bit parallel (i80) ILI9341 display.

## 1. Screen Orientation and LVGL Rotation

The ILI9341 driver (`espressif__esp_lcd_ili9341`) handles hardware orientation via the MADCTL register. However, combining hardware mirroring (`esp_lcd_panel_mirror`) with hardware rotation (`esp_lcd_panel_swap_xy`) often leads to conflicting transformations when interfaced with LVGL, especially when using a custom flush callback.

**Correct Implementation:**
- Initialize the panel with hardware landscape mode: `esp_lcd_panel_swap_xy(disp_panel, true);`
- **DO NOT** use `esp_lcd_panel_mirror()`.
- Instead, apply a 180-degree rotation purely in software via LVGL:
  ```cpp
  lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_180);
  ```
- LVGL's software rotation natively pre-computes the flipped coordinates before passing the `area` to the flush callback, guaranteeing the display renders correctly right-side up without conflicting with the driver's MADCTL state.

## 2. i80 Bus Color Byte Swapping

The ESP32-P4 RISC-V core is little-endian, meaning a 16-bit RGB565 color (e.g., `0x1E3F`) is stored in memory as `[0x3F, 0x1E]`. 
The DMA engine reads this memory sequentially and sends it over the 8-bit parallel bus.
However, the ILI9341 display expects the high byte of the pixel first. 

**Correct Implementation:**
- In the `esp_lcd_panel_io_i80_config_t`, you **MUST** set `.swap_color_bytes = 1`.
- This tells the driver/DMA to swap the byte order before transmission so the display receives `0x1E` then `0x3F`, rendering the correct colors instead of washed-out white/distorted graphics.

## 3. High-Performance PSRAM Allocation

To achieve 50+ FPS without tearing, the display buffer must bypass the small internal SRAM.
- We allocate two full-screen buffers in PSRAM using `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)`.
- We use a custom `p4_flush_cb` to send the entire buffer via `esp_lcd_panel_draw_bitmap`.
- A binary semaphore (`lcd_trans_sem`) tied to `on_color_trans_done` is strictly required to prevent the DMA pipeline from overwriting the active transmission buffer.

## 4. Audio Codec (ES8311) I2C Initialization

The ESP32-P4 strictly enforces I2C clock initialization in ESP-IDF v6.
- The `esp_codec_dev` legacy I2C wrapper will crash if the I2C master bus clock is not running.
- **Fix:** We must add a dummy I2C device (e.g., address `0x7F`) to the bus and perform a 1-byte `i2c_master_transmit` to force the hardware to configure the clock registers *before* the ES8311 driver attaches to `0x18`.
- **LDO Power:** The peripheral rail (LDO channel 4) must be acquired and set to `3300mV` using `esp_ldo_acquire_channel` to ensure the ES8311 chip receives power.

## 5. Avatar UART Bridge (S3/P4)

The board uses `UART 1` for serial communication with the Avatar controller.
- `TX`: GPIO 34
- `RX`: GPIO 36
- Baud Rate: `115200`
- Ensure the physical jumper wires match these pins exactly, or the ESP32-P4 will fail to send animation commands to the secondary MCU.
