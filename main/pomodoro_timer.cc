#include "pomodoro_timer.h"
#include <esp_log.h>

static const char* TAG = "Pomodoro";

void PomodoroTimer::Start() {
    state_ = State::kWork;
    remaining_min_ = 25; // 25 mins work
    ESP_LOGI(TAG, "Pomodoro Started: Work %d mins", remaining_min_);
}

void PomodoroTimer::Stop() {
    state_ = State::kIdle;
    remaining_min_ = 0;
    ESP_LOGI(TAG, "Pomodoro Stopped");
}

void PomodoroTimer::TickMinute() {
    if (state_ == State::kIdle) return;
    
    if (remaining_min_ > 0) {
        remaining_min_--;
    }
    
    if (remaining_min_ <= 0) {
        if (state_ == State::kWork) {
            state_ = State::kBreak;
            remaining_min_ = 5; // 5 mins break
            ESP_LOGI(TAG, "Pomodoro: Break Time %d mins", remaining_min_);
        } else if (state_ == State::kBreak) {
            state_ = State::kIdle;
            remaining_min_ = 0;
            ESP_LOGI(TAG, "Pomodoro: Cycle Finished");
        }
    }
}
