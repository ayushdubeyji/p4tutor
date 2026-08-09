#ifndef AVATAR_PLAYER_H
#define AVATAR_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include "lvgl.h"
#include "esp_jpeg_dec.h"

class AvatarPlayer {
public:
    AvatarPlayer();
    ~AvatarPlayer();

    void Start();
    void Stop();
    void SetSpeaking(bool speaking);
    void SetEmotion(const char* emotion);
    void SetParentScreen(lv_obj_t* screen) { parent_screen_ = screen; }

private:
    void RenderFrame();
    static void RenderTaskStub(void* arg);
    bool OpenFile(const char* path);
    bool ReadNextFrame(uint8_t** frame_buf, size_t* frame_size);

    TaskHandle_t render_task_handle_ = nullptr;
    bool is_playing_ = false;
    bool is_speaking_ = false;
    bool state_changed_ = true;
    char current_emotion_[32] = "neutral";

    lv_obj_t* parent_screen_ = nullptr;  // Which LVGL screen to render on
    FILE* file_ = nullptr;
    jpeg_dec_handle_t jpeg_dec_ = nullptr;
    jpeg_dec_config_t jpeg_cfg_;

    lv_obj_t* img_obj_ = nullptr;
    lv_image_dsc_t img_dsc_{};

    uint8_t* in_buf_ = nullptr;
    size_t in_buf_len_ = 0;
    size_t in_buf_pos_ = 0;
    static const size_t IN_BUF_SIZE = 32 * 1024; // 32KB chunk buffer

    uint8_t* frame_buf_ = nullptr;
    size_t frame_buf_size_ = 0;
    static const size_t MAX_FRAME_SIZE = 64 * 1024; // Max JPEG frame size

    uint8_t* out_buf_ = nullptr;
    int out_buf_size_ = 0;
};

#endif // AVATAR_PLAYER_H
