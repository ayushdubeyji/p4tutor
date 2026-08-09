#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <esp_timer.h>
#include <lvgl.h>
#include "pomodoro_timer.h"
#include "display/screen_manager.h"

struct QuizQuestion {
    std::string q;
    std::vector<std::string> opts;
    int ans; // 0-3
};

struct QuizSettings {
    int timer_seconds = 0;       // 0 = off, else 10, 20, 30
    bool sound_enabled = true;   // UI sounds
    bool show_correct = true;    // Reveal correct answer on wrong
    int brightness_pct = 70;
    int volume_pct = 60;
    
    // Theme options
    int theme_mode = 0;          // 0 = Light, 1 = Dark
    bool glass_effect = true;    // Translucent cards vs solid
    int bg_anim = 1;             // 0 = Off, 1 = Aurora Blobs, 2 = Static
};

struct QuizStats {
    int total_answered = 0;
    int total_correct = 0;
    int current_streak = 0;
    int best_streak = 0;
    int session_score = 0;
    int session_total = 0;
};

enum class QuizMode {
    kHome, kQuiz, kSettings, kProgress, kResult
};

class QuizUI : public Screen {
public:
    static QuizUI& GetInstance() {
        static QuizUI instance;
        return instance;
    }

    QuizUI();
    ~QuizUI();

    void Initialize();
    void Show(lv_obj_t* default_screen);
    void Show() override { Show(nullptr); }
    void Hide() override;
    void GoHome();
    void EnterQuiz();
    void RevealAnswer();

    // AI MCP Integration Methods
    std::string GetQuestionsJsonString(int offset, int limit) const;
    void NavigateToQuestion(int index);
    void ScheduleReminder(int minutes);
    
    // Internal quiz state getters
    bool IsInQuiz() const { return mode_ == QuizMode::kQuiz; }
    int GetCurrentQuestionIndex() const { return q_idx_; }

    // Buttons
    void HandleButtonA(); void HandleButtonB(); void HandleButtonC(); void HandleButtonD();
    void HandleJoyUp() override; void HandleJoyDown() override; void HandleJoyLeft(); void HandleJoyRight();
    void HandleJoySelect() override { HandleJoyPress(); }
    void HandleJoyPress(); void HandleJoyPressLong(); void HandleAgentLongPress();

    // Timers
    void OnTimerTick();
    void OnSchedTick();

    void UpdatePomodoroOverlay();
    
private:
    void RebuildUI();
    void ApplyThemeConfig();
    void DestroyPanels();
    void CreateBackgroundBlobs();

    void LoadQuestions();
    void SwitchToPanel(lv_obj_t* panel);
    void PlayUISound(const std::string_view& sound);

    void BuildHomePanel();
    void ShowHome();
    void UpdateHomeStats();
    void HomeNavigate(int delta);
    void HomeSelect();

    void BuildQuizPanel();
    void DisplayCurrentQuestion();
    void UpdateQuizProgress();
    void MoveCursor(int delta);
    void ConfirmCursorSelection();
    void SelectAnswer(int idx);
    void HighlightOpt(int idx, uint32_t card_bg, uint32_t border, uint32_t badge, uint32_t text_col, lv_opa_t opa = LV_OPA_COVER);
    void ResetAllOpts();
    void FlashOpt(int idx);
    void NextQuestion();
    void PrevQuestion();
    void ShowResult();
    void SetFeedback(const char* msg, uint32_t bg);

    void BuildSettingsPanel();
    void OpenSettings(bool from_quiz);
    void CloseSettings();
    void ApplySettingsAction(int delta);
    void RenderSettings();
    const char* TimerLabel() const;
    void CycleBackgroundColor(int index);

    void BuildProgressPanel();
    void ShowProgress();
    void UpdateProgressUI();
    void StartTimer();
    void StopTimer();
    void UpdateTimerUI();

    // State
    bool is_visible_ = false;
    QuizMode mode_ = QuizMode::kHome;
    lv_obj_t* default_screen_ = nullptr;
    QuizSettings settings_;
    QuizStats stats_;
    std::vector<QuizQuestion> questions_;
    size_t q_idx_ = 0;
    bool mid_quiz_ = false;
    int timer_rem_ = 0;
    esp_timer_handle_t q_timer_ = nullptr;
    
    // Theme Colors
    uint32_t t_bg_, t_card_, t_primary_, t_accent_, t_text_, t_subtext_;
    uint32_t t_correct_ = 0x34C759;
    uint32_t t_wrong_ = 0xFF3B30;
    lv_opa_t t_glass_opa_;

    // System
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* anim_layer_ = nullptr;
    esp_timer_handle_t sched_timer_ = nullptr;
    int sched_rem_ = 0;
    
    // Panels
    lv_obj_t* home_panel_ = nullptr;
    lv_obj_t* quiz_panel_ = nullptr;
    lv_obj_t* settings_panel_ = nullptr;
    lv_obj_t* progress_panel_ = nullptr;
    lv_obj_t* pomodoro_overlay_ = nullptr;
    lv_obj_t* pomodoro_lbl_ = nullptr;

    // Home
    static const int kMenuCount = 4;
    int home_sel_ = 0;
    lv_obj_t* h_menu_[kMenuCount];
    lv_obj_t* h_menu_lbl_[kMenuCount];
    lv_obj_t* h_title_lbl_; lv_obj_t* h_sub_lbl_; lv_obj_t* h_arc_; lv_obj_t* h_arc_lbl_;
    lv_obj_t* h_stats_lbl_; lv_obj_t* h_stat_bar_; lv_obj_t* h_footer_lbl_;
    lv_obj_t* h_xp_lbl_;
    
    // Quiz
    bool ans_revealed_ = false;
    int selected_ans_ = -1;
    int joy_cursor_ = -1;
    lv_obj_t* q_title_lbl_; lv_obj_t* q_progress_lbl_; lv_obj_t* q_prog_bar_;
    lv_obj_t* q_score_lbl_; lv_obj_t* q_card_; lv_obj_t* question_lbl_;
    lv_obj_t* opt_cards_[4]; lv_obj_t* opt_lbls_[4]; lv_obj_t* opt_badge_[4];
    lv_obj_t* q_timer_bar_; lv_obj_t* q_feedback_bar_; lv_obj_t* q_feedback_lbl_;

    // Settings
    static const int kSettingsCount = 9;
    int s_sel_ = 0;
    bool s_from_quiz_ = false;
    lv_obj_t* s_items_[kSettingsCount];
    lv_obj_t* s_vals_[kSettingsCount];

    // Progress
    lv_obj_t* p_answered_lbl_; lv_obj_t* p_correct_lbl_;
    lv_obj_t* p_accuracy_lbl_; lv_obj_t* p_streak_lbl_;
};
