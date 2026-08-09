#include "home_ui.h"
#include "esp_log.h"
#include "application.h"
#include "quiz_ui.h"

#define TAG "HomeUI"

void HomeUI::Initialize() {
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_text_font(screen_, LV_FONT_DEFAULT, 0);  // ensure labels never inherit NULL font
    
    flex_container_ = lv_obj_create(screen_);
    lv_obj_set_size(flex_container_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(flex_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(flex_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    const char* app_names[] = {"Voice Agent", "Quiz Practice", "Settings"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* btn = lv_btn_create(flex_container_);
        lv_obj_set_width(btn, LV_PCT(80));
        
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, app_names[i]);
        lv_obj_center(label);
        
        apps_.push_back({app_names[i], btn});
    }
    
    UpdateSelection();
    
    ScreenManager::GetInstance().SetActiveScreen(this);
}

void HomeUI::Show() {
    if (screen_) {
        lv_scr_load(screen_);
    }
}

void HomeUI::Hide() {
}

void HomeUI::HandleJoyUp() {
    if (apps_.empty()) return;
    selected_index_ = (selected_index_ - 1 + apps_.size()) % apps_.size();
    UpdateSelection();
}

void HomeUI::HandleJoyDown() {
    if (apps_.empty()) return;
    selected_index_ = (selected_index_ + 1) % apps_.size();
    UpdateSelection();
}

void HomeUI::HandleJoySelect() {
    if (apps_.empty()) return;
    ESP_LOGI(TAG, "Selected: %s", apps_[selected_index_].name.c_str());
    
    if (apps_[selected_index_].name == "Quiz Practice") {
        ScreenManager::GetInstance().SetActiveScreen(&QuizUI::GetInstance());
    } else if (apps_[selected_index_].name == "Voice Agent") {
        // Voice agent is handled by the main application / overlay
        Application::GetInstance().StartListening();
    }
}

void HomeUI::UpdateSelection() {
    for (size_t i = 0; i < apps_.size(); i++) {
        if (i == selected_index_) {
            lv_obj_set_style_bg_color(apps_[i].obj, lv_color_hex(0x00A8FF), 0);
        } else {
            lv_obj_set_style_bg_color(apps_[i].obj, lv_color_hex(0xCCCCCC), 0);
        }
    }
    if (!apps_.empty()) {
        lv_obj_scroll_to_view(apps_[selected_index_].obj, LV_ANIM_ON);
    }
}
