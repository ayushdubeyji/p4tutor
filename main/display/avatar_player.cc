#include "avatar_player.h"
#include <esp_log.h>
#include <string.h>
#include "esp_lvgl_port.h"

static const char* TAG = "AvatarPlayer";

#include <esp_heap_caps.h>

AvatarPlayer::AvatarPlayer() {
    jpeg_cfg_ = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_cfg_.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    
    in_buf_ = (uint8_t*)heap_caps_malloc(IN_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in_buf_) in_buf_ = (uint8_t*)malloc(IN_BUF_SIZE); // fallback
    
    frame_buf_ = (uint8_t*)heap_caps_malloc(MAX_FRAME_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buf_) frame_buf_ = (uint8_t*)malloc(MAX_FRAME_SIZE); // fallback
}

AvatarPlayer::~AvatarPlayer() {
    Stop();
    if (in_buf_) free(in_buf_);
    if (frame_buf_) free(frame_buf_);
    if (out_buf_) free(out_buf_);
}

void AvatarPlayer::Start() {
    if (is_playing_) {
        return;
    }

    is_playing_ = true;
    state_changed_ = true;
    // Priority 2 — below Wi-Fi (23) and audio, above idle
    xTaskCreatePinnedToCore(RenderTaskStub, "avatar_render", 8192, this, 2, &render_task_handle_, 1);
    ESP_LOGI(TAG, "AvatarPlayer started");
}

void AvatarPlayer::Stop() {
    if (!is_playing_) {
        return;
    }

    is_playing_ = false;
    if (render_task_handle_ != nullptr) {
        // Wait for the task to finish if it's running
        // Note: Task deletes itself
        while (render_task_handle_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    
    if (jpeg_dec_) {
        jpeg_dec_close(jpeg_dec_);
        jpeg_dec_ = nullptr;
    }
    
    if (img_obj_) {
        lvgl_port_lock(-1);
        lv_obj_del(img_obj_);
        lvgl_port_unlock();
        img_obj_ = nullptr;
    }
    
    ESP_LOGI(TAG, "AvatarPlayer stopped");
}

void AvatarPlayer::SetSpeaking(bool speaking) {
    if (is_speaking_ != speaking) {
        is_speaking_ = speaking;
        state_changed_ = true;
    }
}

void AvatarPlayer::SetEmotion(const char* emotion) {
    if (emotion && strncmp(current_emotion_, emotion, sizeof(current_emotion_)) != 0) {
        strncpy(current_emotion_, emotion, sizeof(current_emotion_) - 1);
        current_emotion_[sizeof(current_emotion_) - 1] = '\0';
        state_changed_ = true;
    }
}

bool AvatarPlayer::OpenFile(const char* path) {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    file_ = fopen(path, "rb");
    if (!file_) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return false;
    }
    in_buf_len_ = 0;
    in_buf_pos_ = 0;
    return true;
}

bool AvatarPlayer::ReadNextFrame(uint8_t** frame_buf_ptr, size_t* frame_size) {
    *frame_size = 0;
    if (!in_buf_ || !frame_buf_) return false;
    
    size_t f_size = 0;
    bool found_soi = false;
    bool found_eoi = false;
    
    while (!found_eoi) {
        if (in_buf_pos_ >= in_buf_len_) {
            in_buf_len_ = fread(in_buf_, 1, IN_BUF_SIZE, file_);
            in_buf_pos_ = 0;
            if (in_buf_len_ == 0) {
                return false; // EOF or error
            }
        }
        
        uint8_t c = in_buf_[in_buf_pos_++];
        
        if (!found_soi) {
            if (f_size == 0 && c == 0xFF) {
                frame_buf_[f_size++] = c;
            } else if (f_size == 1) {
                frame_buf_[f_size++] = c;
                if (c == 0xD8) {
                    found_soi = true;
                } else {
                    f_size = 0; // reset
                }
            }
        } else {
            frame_buf_[f_size++] = c;
            if (f_size >= 2 && frame_buf_[f_size - 2] == 0xFF && frame_buf_[f_size - 1] == 0xD9) {
                found_eoi = true;
                *frame_size = f_size;
                *frame_buf_ptr = frame_buf_;
                return true;
            }
            if (f_size >= MAX_FRAME_SIZE) {
                ESP_LOGE(TAG, "Frame too large");
                return false;
            }
        }
    }
    return false;
}

void AvatarPlayer::RenderFrame() {
    if (state_changed_ || !file_) {
        state_changed_ = false;
        
        if (jpeg_dec_) {
            jpeg_dec_close(jpeg_dec_);
            jpeg_dec_ = nullptr;
        }

        char path[64];
        if (is_speaking_) {
            // Check if specific emotion video exists, fallback to general speak
            snprintf(path, sizeof(path), "/sdcard/avatar_%s_speak.mjpeg", current_emotion_);
            FILE* test_f = fopen(path, "rb");
            if (test_f) {
                fclose(test_f);
            } else {
                snprintf(path, sizeof(path), "/sdcard/avatar_speak.mjpeg");
            }
        } else {
            // Check if specific emotion idle exists, fallback to general idle
            snprintf(path, sizeof(path), "/sdcard/avatar_%s.mjpeg", current_emotion_);
            FILE* test_f = fopen(path, "rb");
            if (test_f) {
                fclose(test_f);
            } else {
                snprintf(path, sizeof(path), "/sdcard/avatar_idle.mjpeg");
            }
        }

        if (!OpenFile(path)) {
            // File not found — back off 2s so we don't spin and starve Wi-Fi
            state_changed_ = true;
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    uint8_t* frame_data = nullptr;
    size_t frame_size = 0;
    
    if (!ReadNextFrame(&frame_data, &frame_size)) {
        // EOF — loop back to beginning
        fseek(file_, 0, SEEK_SET);
        in_buf_len_ = 0;
        in_buf_pos_ = 0;
        vTaskDelay(pdMS_TO_TICKS(33)); // Yield before looping
        return;
    }

    if (!jpeg_dec_) {
        if (jpeg_dec_open(&jpeg_cfg_, &jpeg_dec_) != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open jpeg decoder");
            return;
        }
    }

    jpeg_dec_io_t io = {};
    io.inbuf = frame_data;
    io.inbuf_len = frame_size;

    jpeg_dec_header_info_t header_info;
    if (jpeg_dec_parse_header(jpeg_dec_, &io, &header_info) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to parse jpeg header");
        return;
    }

    int needed_out_size = 0;
    jpeg_dec_get_outbuf_len(jpeg_dec_, &needed_out_size);

    if (out_buf_size_ < needed_out_size) {
        if (out_buf_) free(out_buf_);
        out_buf_ = (uint8_t*)heap_caps_malloc(needed_out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out_buf_) out_buf_ = (uint8_t*)malloc(needed_out_size);
        out_buf_size_ = needed_out_size;
    }
    
    io.outbuf = out_buf_;
    io.out_size = needed_out_size;

    if (jpeg_dec_process(jpeg_dec_, &io) == JPEG_ERR_OK) {
        lvgl_port_lock(-1);
        // Validate parent_screen_ — it can become stale if the display
        // refreshes its theme and swaps in a new screen object.
        lv_obj_t* target_screen = nullptr;
        if (parent_screen_ && lv_obj_is_valid(parent_screen_)) {
            target_screen = parent_screen_;
        } else {
            // Fall back to current active screen; also reset img_obj_ so
            // it gets recreated with the correct parent below.
            target_screen = lv_scr_act();
            if (img_obj_) { lv_obj_del(img_obj_); img_obj_ = nullptr; }
        }
        // If img_obj_ exists but its parent is a different (possibly deleted)
        // screen, delete it so it gets recreated on the correct parent.
        if (img_obj_ && lv_obj_is_valid(img_obj_)) {
            if (lv_obj_get_parent(img_obj_) != target_screen) {
                lv_obj_del(img_obj_); img_obj_ = nullptr;
            }
        } else {
            img_obj_ = nullptr;
        }
        if (!img_obj_) {
            img_obj_ = lv_image_create(target_screen);
            lv_obj_add_flag(img_obj_, LV_OBJ_FLAG_IGNORE_LAYOUT);
            // Fixed small size: 120x80 in bottom-left corner
            lv_obj_set_size(img_obj_, 120, 80);
            lv_obj_align(img_obj_, LV_ALIGN_BOTTOM_LEFT, 4, -4);
            lv_obj_set_style_radius(img_obj_, 6, 0);
            lv_obj_set_style_border_width(img_obj_, 0, 0);
            // Scale image to fit in the widget box
            lv_image_set_inner_align(img_obj_, LV_IMAGE_ALIGN_STRETCH);
        }
        
        img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        img_dsc_.header.flags = 0;
        img_dsc_.header.w = header_info.width;
        img_dsc_.header.h = header_info.height;
        img_dsc_.header.stride = header_info.width * 2;
        img_dsc_.data_size = needed_out_size;
        img_dsc_.data = out_buf_;
        
        lv_image_set_src(img_obj_, &img_dsc_);
        lv_obj_invalidate(img_obj_);
        lvgl_port_unlock();
    }
}

void AvatarPlayer::RenderTaskStub(void* arg) {
    AvatarPlayer* player = static_cast<AvatarPlayer*>(arg);
    while (player->is_playing_) {
        TickType_t start_tick = xTaskGetTickCount();
        player->RenderFrame();
        
        TickType_t end_tick = xTaskGetTickCount();
        uint32_t elapsed_ms = (end_tick - start_tick) * portTICK_PERIOD_MS;
        if (elapsed_ms < 33) {
            vTaskDelay(pdMS_TO_TICKS(33 - elapsed_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1)); // Yield
        }
    }
    player->render_task_handle_ = nullptr;
    vTaskDelete(NULL);
}
