#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <esp_timer.h>
#include "lvgl.h"

// ──────────────────────────────────────────────────────────────────────────────
// Data structures
// ──────────────────────────────────────────────────────────────────────────────

struct QuizQuestion {
    std::string q;
    std::vector<std::string> opts;
    int ans = 0;
};

struct QuizSettings {
    int  timer_seconds  = 0;    // 0 = off, else 10/20/30
    bool sound_enabled  = true; // visual pulse (audio future)
    bool show_correct   = true; // reveal correct on wrong pick
    int  brightness_pct = 75;
    int  volume_pct     = 70;
};

struct QuizStats {
    int total_answered  = 0;
    int total_correct   = 0;
    int current_streak  = 0;
    int best_streak     = 0;
    int session_score   = 0;  // correct this session
    int session_total   = 0;  // answered this session
};

enum class QuizMode {
    kHome,
    kQuiz,
    kSettings,
    kProgress,
    kResult,
};

// ──────────────────────────────────────────────────────────────────────────────
// QuizUI
// ──────────────────────────────────────────────────────────────────────────────

class QuizUI {
public:
    static QuizUI& GetInstance() {
        static QuizUI instance;
        return instance;
    }

    void Initialize();
    void Show(lv_obj_t* default_screen);
    void Hide();
    void GoHome();
    bool IsVisible() const { return is_visible_; }

    // TTP Buttons (A/B/C/D)
    void HandleButtonA();
    void HandleButtonB();
    void HandleButtonC();
    void HandleButtonD();

    // Joystick
    void HandleJoyUp();
    void HandleJoyDown();
    void HandleJoyLeft();
    void HandleJoyRight();
    void HandleJoyPress();
    void HandleJoyPressLong();   // open settings from quiz
    void HandleAgentLongPress(); // go home

    void OnTimerTick();  // called from esp_timer, updates countdown

private:
    QuizUI();
    ~QuizUI();

    // ── LVGL objects ──────────────────────────────────────────────────────────

    lv_obj_t* screen_         = nullptr;
    lv_obj_t* default_screen_ = nullptr;

    // Panels (full-screen children of screen_)
    lv_obj_t* home_panel_     = nullptr;
    lv_obj_t* quiz_panel_     = nullptr;
    lv_obj_t* settings_panel_ = nullptr;
    lv_obj_t* progress_panel_ = nullptr;

    // --- Home panel ---
    lv_obj_t* h_title_lbl_    = nullptr;
    lv_obj_t* h_sub_lbl_      = nullptr;
    lv_obj_t* h_arc_          = nullptr;
    lv_obj_t* h_arc_lbl_      = nullptr;
    lv_obj_t* h_stats_lbl_    = nullptr;
    lv_obj_t* h_stat_bar_     = nullptr;
    static const int kMenuCount = 4;
    lv_obj_t* h_menu_[kMenuCount]     = {nullptr};
    lv_obj_t* h_menu_lbl_[kMenuCount] = {nullptr};
    lv_obj_t* h_footer_lbl_   = nullptr;

    // --- Quiz panel ---
    lv_obj_t* q_topbar_       = nullptr;
    lv_obj_t* q_title_lbl_    = nullptr;
    lv_obj_t* q_progress_lbl_ = nullptr;
    lv_obj_t* q_prog_bar_     = nullptr;
    lv_obj_t* q_score_lbl_    = nullptr;
    lv_obj_t* q_timer_bar_    = nullptr;
    lv_obj_t* q_timer_lbl_    = nullptr;
    lv_obj_t* q_card_         = nullptr;
    lv_obj_t* question_lbl_   = nullptr;
    lv_obj_t* opt_cards_[4]   = {nullptr};
    lv_obj_t* opt_badge_[4]   = {nullptr};
    lv_obj_t* opt_lbls_[4]    = {nullptr};
    lv_obj_t* q_feedback_bar_ = nullptr;
    lv_obj_t* q_feedback_lbl_ = nullptr;
    lv_obj_t* q_nav_lbl_      = nullptr;

    // --- Settings panel ---
    static const int kSettingsCount = 6;
    lv_obj_t* s_items_[kSettingsCount] = {nullptr};
    lv_obj_t* s_vals_[kSettingsCount]  = {nullptr};
    // 0=Timer 1=Sound 2=ShowCorrect 3=Brightness 4=Volume 5=Back

    // --- Progress panel ---
    lv_obj_t* p_answered_lbl_ = nullptr;
    lv_obj_t* p_correct_lbl_  = nullptr;
    lv_obj_t* p_streak_lbl_   = nullptr;
    lv_obj_t* p_accuracy_lbl_ = nullptr;
    lv_obj_t* p_bar_          = nullptr;

    // ── State ─────────────────────────────────────────────────────────────────

    // Lazy-build flags — panels are built on first use to conserve LVGL heap
    bool quiz_built_     = false;
    bool settings_built_ = false;
    bool progress_built_ = false;

    std::vector<QuizQuestion> questions_;
    size_t  q_idx_          = 0;
    int     selected_ans_   = -1;
    int     joy_cursor_     = -1;  // 0-3 = option highlighted by joystick
    bool    ans_revealed_   = false;
    bool    mid_quiz_       = false;

    QuizMode mode_      = QuizMode::kHome;
    int      home_sel_  = 0;       // selected home menu item
    int      s_sel_     = 0;       // selected settings item
    bool     s_from_quiz_ = false; // settings opened from quiz (close → quiz)

    QuizSettings settings_;
    QuizStats    stats_;
    bool is_visible_ = false;

    // Timer
    esp_timer_handle_t  q_timer_    = nullptr;
    int                 timer_rem_  = 0;

    // ── Color palette ─────────────────────────────────────────────────────────
    static constexpr uint32_t kBg        = 0xF2F2F7; // iOS System Grey
    static constexpr uint32_t kCard      = 0xFFFFFF; // Pure White
    static constexpr uint32_t kPrimary   = 0x007AFF; // iOS Blue
    static constexpr uint32_t kAccent    = 0x5856D6; // iOS Indigo
    static constexpr uint32_t kText      = 0x000000; // Black Text
    static constexpr uint32_t kSubtext   = 0x8E8E93; // iOS Grey Text
    static constexpr uint32_t kCorrect   = 0x34C759; // iOS Green
    static constexpr uint32_t kWrong     = 0xFF3B30; // iOS Red
    static constexpr uint32_t kCorrectBg = 0xE5F9E7; 
    static constexpr uint32_t kWrongBg   = 0xFFEBEA; 
    static constexpr uint32_t kOptDef    = 0xFFFFFF; 
    static constexpr uint32_t kOptCur    = 0x007AFF; 
    static constexpr uint32_t kWarn      = 0xFF9500; // iOS Orange

    // ── Private methods ───────────────────────────────────────────────────────

    void LoadQuestions();

    // Panel building
    void BuildHomePanel();
    void BuildQuizPanel();
    void BuildSettingsPanel();
    void BuildProgressPanel();

    // Panel switching
    void SwitchToPanel(lv_obj_t* panel);

    // Home
    void ShowHome();
    void UpdateHomeStats();
    void HomeNavigate(int delta);
    void HomeSelect();

    // Quiz
    void EnterQuiz();
    void DisplayCurrentQuestion();
    void MoveCursor(int delta);
    void ConfirmCursorSelection();
    void SelectAnswer(int idx);
    void HighlightOpt(int idx, uint32_t card_bg, uint32_t border, uint32_t badge, uint32_t text_col);
    void ResetAllOpts();
    void FlashOpt(int idx);
    void NextQuestion();
    void PrevQuestion();
    void SetFeedback(const char* msg, uint32_t bg);
    void SetFeedback(const std::string& msg, uint32_t bg);
    void UpdateQuizProgress();

    // Timer
    void StartTimer();
    void StopTimer();
    void UpdateTimerUI();

    // Settings
    void OpenSettings(bool from_quiz);
    void CloseSettings();
    void RenderSettings();
    const char* TimerLabel() const;
    void ApplySettingsAction(int delta);

    // Progress
    void ShowProgress();
    void UpdateProgressUI();

    // Result
    void ShowResult();
};
