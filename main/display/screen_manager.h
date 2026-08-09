#pragma once

#include <memory>
#include <vector>
#include <mutex>

class Screen {
public:
    virtual ~Screen() = default;
    virtual void Show() = 0;
    virtual void Hide() = 0;
    
    virtual void HandleJoyUp() {}
    virtual void HandleJoyDown() {}
    virtual void HandleJoySelect() {}
};

class ScreenManager {
public:
    static ScreenManager& GetInstance() {
        static ScreenManager instance;
        return instance;
    }

    void SetActiveScreen(Screen* screen) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_screen_) {
            active_screen_->Hide();
        }
        active_screen_ = screen;
        if (active_screen_) {
            active_screen_->Show();
        }
    }

    void HandleJoyUp() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_screen_) active_screen_->HandleJoyUp();
    }

    void HandleJoyDown() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_screen_) active_screen_->HandleJoyDown();
    }

    void HandleJoySelect() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_screen_) active_screen_->HandleJoySelect();
    }

private:
    ScreenManager() = default;
    ScreenManager(const ScreenManager&) = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;

    Screen* active_screen_ = nullptr;
    std::mutex mutex_;
};
