#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_13
#define AUDIO_I2S_GPIO_WS GPIO_NUM_10
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_11
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_53
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_8
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO        GPIO_NUM_35

#define DISPLAY_WIDTH  (320)
#define DISPLAY_HEIGHT (240)
#define PIN_NUM_LCD_RST            GPIO_NUM_33
#define PIN_NUM_LCD_CS             GPIO_NUM_32
#define PIN_NUM_LCD_RS             GPIO_NUM_31
#define PIN_NUM_LCD_WR             GPIO_NUM_30
#define PIN_NUM_LCD_RD             -1
#define PIN_NUM_LCD_D0             GPIO_NUM_20
#define PIN_NUM_LCD_D1             GPIO_NUM_21
#define PIN_NUM_LCD_D2             GPIO_NUM_22
#define PIN_NUM_LCD_D3             GPIO_NUM_23
#define PIN_NUM_LCD_D4             GPIO_NUM_26
#define PIN_NUM_LCD_D5             GPIO_NUM_27
#define PIN_NUM_LCD_D6             GPIO_NUM_28
#define PIN_NUM_LCD_D7             GPIO_NUM_29

#define DISPLAY_BACKLIGHT_PIN      GPIO_NUM_4 // Adjust if there is a real backlight pin
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false


#define LCD_BIT_PER_PIXEL          (16)
#define DELAY_TIME_MS                      (3000)


#define DISPLAY_SWAP_XY true
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define PIN_BTN_A GPIO_NUM_49
#define PIN_BTN_B GPIO_NUM_2
#define PIN_BTN_C GPIO_NUM_5
#define PIN_BTN_D GPIO_NUM_4

// Joystick - user-specified physical wiring
#define PIN_JOY_UP    GPIO_NUM_48
#define PIN_JOY_DOWN  GPIO_NUM_52
#define PIN_JOY_LEFT  GPIO_NUM_47
#define PIN_JOY_RIGHT GPIO_NUM_51
#define PIN_JOY_PRESS GPIO_NUM_50
#define PIN_AGENT_BTN GPIO_NUM_46

#endif // _BOARD_CONFIG_H_
