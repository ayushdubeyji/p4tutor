#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
#include "quiz_ui.h"
#include "display/screen_manager.h"
#include <esp_lvgl_port.h>
// #include "display/no_display.h"
#include "button.h"

#include "esp_video.h"
#include "esp_video_init.h"
#include "esp_cam_sensor_xclk.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <freertos/FreeRTOS.h>
#include <esp_ldo_regulator.h>

#include "config.h"
#include "lcd_init_cmds.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lvgl_port.h>
#include "esp_lcd_touch_gt911.h"
#define TAG "WaveshareEsp32p4"

class WaveshareEsp32p4 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button btn_a_, btn_b_, btn_c_, btn_d_;
    Button joy_up_, joy_down_, joy_left_, joy_right_, joy_press_;
    Button agent_btn_;
    SpiLcdDisplay* display_ = nullptr;
    EspVideo* camera_ = nullptr;
    sdmmc_card_t* sd_card_ = nullptr;
    bool sd_card_mounted_ = false;

    esp_err_t i2c_device_probe(uint8_t addr) {
        return i2c_master_probe(i2c_bus_, addr, 100);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }



    void InitializeLCD() {
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_handle_t disp_panel = NULL;

        esp_lcd_i80_bus_handle_t i80_bus = NULL;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = PIN_NUM_LCD_RS,
            .wr_gpio_num = PIN_NUM_LCD_WR,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                PIN_NUM_LCD_D0,
                PIN_NUM_LCD_D1,
                PIN_NUM_LCD_D2,
                PIN_NUM_LCD_D3,
                PIN_NUM_LCD_D4,
                PIN_NUM_LCD_D5,
                PIN_NUM_LCD_D6,
                PIN_NUM_LCD_D7,
            },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = PIN_NUM_LCD_CS,
            .pclk_hz = 5 * 1000 * 1000,
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_config, &disp_panel));

        esp_lcd_panel_reset(disp_panel);
        esp_lcd_panel_init(disp_panel);
        esp_lcd_panel_invert_color(disp_panel, false);
        esp_lcd_panel_swap_xy(disp_panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(disp_panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(disp_panel, true);

        display_ = new SpiLcdDisplay(io, disp_panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }
    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,                            
            .flags =
            {
                .disable_control_phase = 1,
            }
	    };
	    if (ESP_OK == i2c_device_probe(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS)) {
            ESP_LOGI(TAG, "Touch panel found at address 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS);
        } else if (ESP_OK == i2c_device_probe(ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP)) {
            ESP_LOGI(TAG, "Touch panel found at address 0x%02X", ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
            tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        } else {
            ESP_LOGE(TAG, "Touch panel not found on I2C bus");
            ESP_LOGE(TAG, "Tried addresses: 0x%02X and 0x%02X", 
                     ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 
                     ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP);
            return;
        }

        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }
    void InitializeCameraPower() {
        // Power on MIPI CSI/DSI PHY
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = 3,
            .voltage_mv = 2500,
        };
        esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
        esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "MIPI PHY LDO channel 3 acquired and powered on");
        } else {
            ESP_LOGE(TAG, "Failed to acquire MIPI PHY LDO channel 3: %s", esp_err_to_name(err));
        }
    }

    void InitializeCamera() {
        esp_video_init_csi_config_t base_csi_config = {
            .sccb_config = {
                .init_sccb = false,
                .i2c_handle = i2c_bus_,
                .freq = 400000,
            },
            .reset_pin = GPIO_NUM_NC,
            .pwdn_pin  = GPIO_NUM_NC,
        };

        esp_video_init_config_t cam_config = {
            .csi      = &base_csi_config,
        };

        camera_ = new EspVideo(cam_config);
    }
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        boot_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        agent_btn_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        agent_btn_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });

        // FIX C3: Wrap LVGL calls with display lock — OnClick fires from timer task
        // TTP buttons: active_high. Use blocking lock (-1) to avoid silent drop.
        btn_a_.OnClick([]() { if (lvgl_port_lock(-1)) { QuizUI::GetInstance().HandleButtonA(); lvgl_port_unlock(); } });
        btn_b_.OnClick([]() { if (lvgl_port_lock(-1)) { QuizUI::GetInstance().HandleButtonB(); lvgl_port_unlock(); } });
        btn_c_.OnClick([]() { if (lvgl_port_lock(-1)) { QuizUI::GetInstance().HandleButtonC(); lvgl_port_unlock(); } });
        btn_d_.OnClick([]() { if (lvgl_port_lock(-1)) { QuizUI::GetInstance().HandleButtonD(); lvgl_port_unlock(); } });

        joy_up_.OnClick([]()    { if (lvgl_port_lock(-1)) { ScreenManager::GetInstance().HandleJoyUp();    lvgl_port_unlock(); } });
        joy_down_.OnClick([]()  { if (lvgl_port_lock(-1)) { ScreenManager::GetInstance().HandleJoyDown();  lvgl_port_unlock(); } });
        joy_left_.OnClick([]()  { if (lvgl_port_lock(-1)) { /* ScreenManager left not implemented yet */  lvgl_port_unlock(); } });
        joy_right_.OnClick([]() { if (lvgl_port_lock(-1)) { /* ScreenManager right not implemented yet */ lvgl_port_unlock(); } });
        joy_press_.OnClick([]() { if (lvgl_port_lock(-1)) { ScreenManager::GetInstance().HandleJoySelect(); lvgl_port_unlock(); } });

        // Long-press: joy_press opens settings from quiz; agent_btn returns home
        joy_press_.OnLongPress([]()  { if (lvgl_port_lock(-1)) { QuizUI::GetInstance().HandleJoyPressLong();  lvgl_port_unlock(); } });
        agent_btn_.OnLongPress([]()  { if (lvgl_port_lock(-1)) { QuizUI::GetInstance().HandleAgentLongPress(); lvgl_port_unlock(); } });
    }

    static esp_err_t custom_sdmmc_host_init() {
        esp_err_t err = sdmmc_host_init();
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
            return ESP_OK; // Already initialized by esp_hosted Wi-Fi module
        }
        return err;
    }

    void InitializeSdCard() {
        ESP_LOGI(TAG, "Initializing SD card");

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.init = &custom_sdmmc_host_init;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;

        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = 4,
        };
        sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
        esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
        if (pwr_err == ESP_OK) {
            host.pwr_ctrl_handle = pwr_ctrl_handle;
        } else {
            ESP_LOGW(TAG, "Failed to init LDO power control: %s", esp_err_to_name(pwr_err));
        }

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = GPIO_NUM_43;
        slot.cmd = GPIO_NUM_44;
        slot.d0 = GPIO_NUM_39;
        slot.d1 = GPIO_NUM_40;
        slot.d2 = GPIO_NUM_41;
        slot.d3 = GPIO_NUM_42;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.width = 4;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 64 * 1024,
        };

        esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_config, &sd_card_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        } else {
            sd_card_mounted_ = true;
            ESP_LOGI(TAG, "SD card mounted successfully");
        }
    }

public:
    WaveshareEsp32p4() :
        boot_button_(BOOT_BUTTON_GPIO),
        btn_a_(PIN_BTN_A, true), btn_b_(PIN_BTN_B, true), btn_c_(PIN_BTN_C, true), btn_d_(PIN_BTN_D, true),
        joy_up_(PIN_JOY_UP), joy_down_(PIN_JOY_DOWN), joy_left_(PIN_JOY_LEFT), joy_right_(PIN_JOY_RIGHT), joy_press_(PIN_JOY_PRESS),
        agent_btn_(PIN_AGENT_BTN) {
        InitializeCodecI2c();
        InitializeLCD();
        InitializeCameraPower();
        InitializeTouch();
        InitializeCamera();
        InitializeButtons();
        InitializeSdCard();
        GetBacklight()->RestoreBrightness();
    }

    ~WaveshareEsp32p4() {
        if (sd_card_mounted_) {
            esp_vfs_fat_sdcard_unmount("/sdcard", sd_card_);
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_1, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

};

DECLARE_BOARD(WaveshareEsp32p4);
