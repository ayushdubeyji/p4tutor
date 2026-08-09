#pragma once

#include "screen_manager.h"
#include "lvgl.h"
#include <vector>
#include <string>

class HomeUI : public Screen {
public:
    static HomeUI& GetInstance() {
        static HomeUI instance;
        return instance;
    }

    void Initialize();
    void Show() override;
    void Hide() override;
    
    void HandleJoyUp() override;
    void HandleJoyDown() override;
    void HandleJoySelect() override;

private:
    HomeUI() = default;
    
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* flex_container_ = nullptr;
    
    struct AppItem {
        std::string name;
        lv_obj_t* obj;
    };
    std::vector<AppItem> apps_;
    int selected_index_ = 0;
    
    void UpdateSelection();
};
