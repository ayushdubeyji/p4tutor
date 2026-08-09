#include "quiz_ui.h"
#include "application.h"
#include "assets/lang_config.h"
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_lvgl_port.h>
#include "assets.h"
#include "srs_database.h"
#include <sstream>
#include <fstream>
#include <math.h>
#include "cJSON.h"
#include "mcp_server.h"

#define TAG "QuizUI"

// ──────────────────────────────────────────────────────────────────────────────
// Static LVGL animation callbacks
// ──────────────────────────────────────────────────────────────────────────────
static void anim_opa_cb(void* obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0); }
static void anim_arc_cb(void* obj, int32_t v) { lv_arc_set_value((lv_obj_t*)obj, v); }
// static void anim_bar_cb(void* obj, int32_t v) { lv_bar_set_value((lv_obj_t*)obj, v, LV_ANIM_OFF); }
static void anim_x_cb(void* obj, int32_t v) { lv_obj_set_x((lv_obj_t*)obj, v); }
static void anim_y_cb(void* obj, int32_t v) { lv_obj_set_y((lv_obj_t*)obj, v); }
static void anim_zoom_cb(void* obj, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t*)obj, v, 0); }
static void quiz_timer_cb(void* arg) { static_cast<QuizUI*>(arg)->OnTimerTick(); }

// ──────────────────────────────────────────────────────────────────────────────
// Lifecycle & Theming
// ──────────────────────────────────────────────────────────────────────────────
QuizUI::QuizUI() {}
QuizUI::~QuizUI() {
    StopTimer();
    if (q_timer_) esp_timer_delete(q_timer_);
}

void QuizUI::ApplyThemeConfig() {
    if (settings_.theme_mode == 0) { // Light
        t_bg_ = 0xF6F8FA;
        t_card_ = 0xFFFFFF;
        t_primary_ = 0x08BD80;
        t_accent_ = 0x0969DA;
        t_text_ = 0x1F2328;
        t_subtext_ = 0x6E7781;
        t_correct_ = 0x34C759;
        t_wrong_ = 0xFF3B30;
    } else { // Dark
        t_bg_ = 0x0D1117;
        t_card_ = 0x161B22;
        t_primary_ = 0x238636;
        t_accent_ = 0x1F6FEB;
        t_text_ = 0xF0F6FC;
        t_subtext_ = 0x8B949E;
        t_correct_ = 0x30D158;
        t_wrong_ = 0xFF453A;
    }
    t_glass_opa_ = settings_.glass_effect ? LV_OPA_70 : LV_OPA_COVER;
}

void QuizUI::Initialize() {
    ESP_LOGI(TAG, "Initializing QuizUI (Dynamic Theme)");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/mcq", .partition_label = "mcq_data",
        .max_files = 5, .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        LoadQuestions();
        SrsDatabase::GetInstance().Initialize("/mcq");
    }

    esp_timer_create_args_t ta = { .callback = quiz_timer_cb, .arg = this, .name = "quiz" };
    esp_timer_create(&ta, &q_timer_);

    // Expose Quiz questions to the AI via MCP
    McpServer::GetInstance().AddTool("quiz.get_questions",
        "Retrieve the practice test questions loaded on the device. Useful for conducting an interactive test.",
        PropertyList({
            Property("offset", kPropertyTypeInteger, 0, 0, 1000),
            Property("limit", kPropertyTypeInteger, 5, 1, 50)
        }),
        [](const PropertyList& props) -> ReturnValue {
            return QuizUI::GetInstance().GetQuestionsJsonString(props["offset"].value<int>(), props["limit"].value<int>());
        });

    McpServer::GetInstance().AddTool("quiz.set_ui_state",
        "Navigate the physical screen of the device to a specific question, or select an option for the user.",
        PropertyList({
            Property("question_index", kPropertyTypeInteger, 0, 0, 1000),
            Property("select_option_index", kPropertyTypeInteger, -1, -1, 4)
        }),
        [](const PropertyList& props) -> ReturnValue {
            QuizUI::GetInstance().NavigateToQuestion(props["question_index"].value<int>());
            int opt = props["select_option_index"].value<int>();
            if (opt >= 0 && opt < 4) QuizUI::GetInstance().SelectAnswer(opt);
            return true;
        });

    McpServer::GetInstance().AddTool("quiz.exit_quiz",
        "Exit the practice test and return the user to the home screen.",
        PropertyList(),
        [](const PropertyList& props) -> ReturnValue {
            QuizUI::GetInstance().GoHome();
            return true;
        });

    McpServer::GetInstance().AddTool("quiz.start_quiz",
        "Open the quiz screen and start a practice test. Call this before navigating to questions.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (lvgl_port_lock(-1)) {
                QuizUI::GetInstance().Show();
                QuizUI::GetInstance().EnterQuiz();
                lvgl_port_unlock();
            }
            return true;
        });

    McpServer::GetInstance().AddTool("quiz.reveal_answer",
        "Highlight the correct answer for the current question on screen.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            if (lvgl_port_lock(-1)) {
                QuizUI::GetInstance().RevealAnswer();
                lvgl_port_unlock();
            }
            return true;
        });

    McpServer::GetInstance().AddTool("quiz.get_current_state",
        "Get the current quiz state: which question is displayed, score, and whether answer is revealed.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& q = QuizUI::GetInstance();
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "current_index", q.GetCurrentQuestionIndex());
            cJSON_AddBoolToObject(obj, "in_quiz", q.IsInQuiz());
            return obj;
        });

    RebuildUI();
}

std::string QuizUI::GetQuestionsJsonString(int offset, int limit) const {
    cJSON* root = cJSON_CreateArray();
    int end = std::min((int)questions_.size(), offset + limit);
    for (int i = offset; i < end; i++) {
        const auto& q = questions_[i];
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "question", q.q.c_str());
        cJSON_AddNumberToObject(item, "correct_index", q.ans);
        cJSON* opts = cJSON_CreateArray();
        for (const auto& opt : q.opts) {
            cJSON_AddItemToArray(opts, cJSON_CreateString(opt.c_str()));
        }
        cJSON_AddItemToObject(item, "options", opts);
        cJSON_AddItemToArray(root, item);
    }
    char* str = cJSON_PrintUnformatted(root);
    std::string res(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return res;
}

void QuizUI::NavigateToQuestion(int index) {
    if (index >= 0 && index < (int)questions_.size()) {
        if (!lvgl_port_lock(-1)) return;
        if (!is_visible_) Show();
        if (mode_ != QuizMode::kQuiz) EnterQuiz();
        ans_revealed_ = false;
        joy_cursor_ = 0;
        ResetAllOpts();
        q_idx_ = index;
        CycleBackgroundColor(q_idx_);
        DisplayCurrentQuestion();
        lvgl_port_unlock();
    }
}

void QuizUI::RevealAnswer() {
    if (!is_visible_ || mode_ != QuizMode::kQuiz || ans_revealed_) return;
    const QuizQuestion& q = questions_[q_idx_];
    ans_revealed_ = true;
    HighlightOpt(q.ans, t_correct_, t_correct_, t_correct_, 0xFFFFFF, LV_OPA_COVER);
    char fb[40]; snprintf(fb, sizeof(fb), LV_SYMBOL_OK " Correct answer: %c", 'A' + q.ans);
    SetFeedback(fb, t_correct_);
}

void QuizUI::DestroyPanels() {
    if (home_panel_) { lv_obj_del(home_panel_); home_panel_ = nullptr; }
    if (quiz_panel_) { lv_obj_del(quiz_panel_); quiz_panel_ = nullptr; }
    if (settings_panel_) { lv_obj_del(settings_panel_); settings_panel_ = nullptr; }
    if (progress_panel_) { lv_obj_del(progress_panel_); progress_panel_ = nullptr; }
    if (anim_layer_) { lv_obj_del(anim_layer_); anim_layer_ = nullptr; }
    if (screen_) { lv_obj_del(screen_); screen_ = nullptr; }
}

void QuizUI::CreateBackgroundBlobs() {
    anim_layer_ = lv_obj_create(screen_);
    lv_obj_set_size(anim_layer_, 320, 240);
    lv_obj_set_style_bg_opa(anim_layer_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(anim_layer_, 0, 0);
    lv_obj_clear_flag(anim_layer_, LV_OBJ_FLAG_SCROLLABLE);

    if (settings_.bg_anim == 0) return; // Off

    // Blob 1
    lv_obj_t* b1 = lv_obj_create(anim_layer_);
    lv_obj_set_size(b1, 200, 200);
    lv_obj_set_style_radius(b1, 100, 0);
    lv_obj_set_style_bg_color(b1, lv_color_hex(t_accent_), 0);
    lv_obj_set_style_bg_opa(b1, LV_OPA_30, 0);
    lv_obj_set_style_border_width(b1, 0, 0);
    
    // Blob 2
    lv_obj_t* b2 = lv_obj_create(anim_layer_);
    lv_obj_set_size(b2, 250, 250);
    lv_obj_set_style_radius(b2, 125, 0);
    lv_obj_set_style_bg_color(b2, lv_color_hex(t_primary_), 0);
    lv_obj_set_style_bg_opa(b2, LV_OPA_20, 0);
    lv_obj_set_style_border_width(b2, 0, 0);

    if (settings_.bg_anim == 1) { // Aurora Animated
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, b1); lv_anim_set_exec_cb(&a, anim_x_cb);
        lv_anim_set_values(&a, -50, 250); lv_anim_set_time(&a, 8000);
        lv_anim_set_playback_time(&a, 7000); lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE); lv_anim_start(&a);

        lv_anim_set_var(&a, b1); lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_values(&a, -50, 150); lv_anim_set_time(&a, 9500);
        lv_anim_set_playback_time(&a, 8500); lv_anim_start(&a);

        lv_anim_set_var(&a, b2); lv_anim_set_exec_cb(&a, anim_x_cb);
        lv_anim_set_values(&a, 250, -80); lv_anim_set_time(&a, 11000);
        lv_anim_set_playback_time(&a, 10000); lv_anim_start(&a);

        lv_anim_set_var(&a, b2); lv_anim_set_exec_cb(&a, anim_y_cb);
        lv_anim_set_values(&a, 100, -80); lv_anim_set_time(&a, 7500);
        lv_anim_set_playback_time(&a, 8500); lv_anim_start(&a);
    } else { // Static
        lv_obj_set_pos(b1, -10, -10);
        lv_obj_set_pos(b2, 100, 50);
    }
}

void QuizUI::RebuildUI() {
    if (!lvgl_port_lock(-1)) return;
    
    bool was_visible = is_visible_;
    ApplyThemeConfig();
    DestroyPanels();

    screen_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(t_bg_), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

    CreateBackgroundBlobs();

    BuildHomePanel();
    BuildQuizPanel();
    BuildSettingsPanel();
    BuildProgressPanel();

    if (was_visible) {
        lv_scr_load(screen_);
        switch (mode_) {
            case QuizMode::kHome: ShowHome(); break;
            case QuizMode::kQuiz: EnterQuiz(); break;
            case QuizMode::kSettings: OpenSettings(s_from_quiz_); break;
            case QuizMode::kProgress: ShowProgress(); break;
            case QuizMode::kResult: ShowResult(); break;
        }
    } else {
        SwitchToPanel(home_panel_);
    }
    
    lvgl_port_unlock();
}

void QuizUI::LoadQuestions() {
    std::ifstream f("/mcq/quiz.json");
    if (!f.is_open()) return;
    std::stringstream buf; buf << f.rdbuf(); f.close();
    cJSON* root = cJSON_Parse(buf.str().c_str());
    if (!root || !cJSON_IsArray(root)) { if (root) cJSON_Delete(root); return; }
    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        cJSON* qj = cJSON_GetObjectItem(item, "q");
        cJSON* aj = cJSON_GetObjectItem(item, "ans");
        cJSON* oj = cJSON_GetObjectItem(item, "opts");
        if (!cJSON_IsString(qj) || !cJSON_IsNumber(aj) || !cJSON_IsArray(oj)) continue;
        QuizQuestion q; q.q = qj->valuestring; q.ans = aj->valueint;
        int on = cJSON_GetArraySize(oj);
        for (int j = 0; j < on && j < 4; j++) {
            cJSON* o = cJSON_GetArrayItem(oj, j);
            if (cJSON_IsString(o) && o->valuestring) q.opts.push_back(o->valuestring);
        }
        if (!q.opts.empty()) questions_.push_back(q);
    }
    cJSON_Delete(root);
}

void QuizUI::PlayUISound(const std::string_view& sound) {
    if (settings_.sound_enabled) {
        Application::GetInstance().PlaySound(sound);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Build Helpers
// ──────────────────────────────────────────────────────────────────────────────
static lv_obj_t* make_panel(lv_obj_t* parent) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_size(p, 320, 240);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h, uint32_t track, uint32_t ind) {
    lv_obj_t* b = lv_bar_create(parent);
    lv_obj_set_size(b, w, h); lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, 100); lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, lv_color_hex(track), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ind), LV_PART_INDICATOR);
    return b;
}

static lv_obj_t* make_hdr(lv_obj_t* parent, int h, bool bottom_border, uint32_t card_bg, lv_opa_t opa, int theme_mode) {
    lv_obj_t* hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, 320, h); lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(card_bg), 0);
    lv_obj_set_style_bg_opa(hdr, opa, 0);
    uint32_t bcol = theme_mode == 0 ? 0xE5E5EA : 0x2C2C2E;
    lv_obj_set_style_border_color(hdr, lv_color_hex(bcol), 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_side(hdr, bottom_border ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    return hdr;
}

static void apply_glass_card(lv_obj_t* card, uint32_t card_bg, lv_opa_t opa, int theme_mode) {
    lv_obj_set_style_bg_color(card, lv_color_hex(card_bg), 0);
    lv_obj_set_style_bg_opa(card, opa, 0);
    lv_obj_set_style_shadow_width(card, 0, 0); 
    uint32_t bcol = theme_mode == 0 ? 0xE5E5EA : 0x2C2C2E;
    lv_obj_set_style_border_color(card, lv_color_hex(bcol), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 16, 0);
}

// ──────────────────────────────────────────────────────────────────────────────
// Build: Panels
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::BuildHomePanel() {
    home_panel_ = make_panel(screen_);
    lv_obj_t* hdr = make_hdr(home_panel_, 70, true, t_card_, t_glass_opa_, settings_.theme_mode);
    
    h_title_lbl_ = lv_label_create(hdr);
    lv_label_set_text(h_title_lbl_, LV_SYMBOL_PLAY "  EduBoard");
    lv_obj_align(h_title_lbl_, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_text_color(h_title_lbl_, lv_color_hex(t_text_), 0);

    h_sub_lbl_ = lv_label_create(hdr);
    lv_label_set_text(h_sub_lbl_, "Smart Learning Assistant");
    lv_obj_align(h_sub_lbl_, LV_ALIGN_TOP_LEFT, 10, 28);
    lv_obj_set_style_text_color(h_sub_lbl_, lv_color_hex(t_subtext_), 0);

    h_arc_ = lv_arc_create(hdr);
    lv_obj_set_size(h_arc_, 48, 48); lv_obj_align(h_arc_, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_arc_set_range(h_arc_, 0, 100); lv_arc_set_bg_angles(h_arc_, 135, 45);
    uint32_t tb = settings_.theme_mode == 0 ? 0xE5E5EA : 0x3A3A3C;
    lv_obj_set_style_arc_color(h_arc_, lv_color_hex(tb), LV_PART_MAIN);
    lv_obj_set_style_arc_color(h_arc_, lv_color_hex(t_primary_), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(h_arc_, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(h_arc_, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(h_arc_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(h_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    
    h_arc_lbl_ = lv_label_create(hdr);
    lv_label_set_text(h_arc_lbl_, "0%");
    lv_obj_align(h_arc_lbl_, LV_ALIGN_TOP_RIGHT, -18, 25);
    lv_obj_set_style_text_color(h_arc_lbl_, lv_color_hex(t_primary_), 0);

    lv_obj_t* stats_row = lv_obj_create(home_panel_);
    lv_obj_set_size(stats_row, 320, 24); lv_obj_set_pos(stats_row, 0, 70);
    lv_obj_set_style_bg_opa(stats_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stats_row, 0, 0);
    h_stats_lbl_ = lv_label_create(stats_row);
    lv_obj_align(h_stats_lbl_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(h_stats_lbl_, lv_color_hex(t_subtext_), 0);

    h_stat_bar_ = make_bar(home_panel_, 0, 94, 320, 4, tb, t_accent_);

    const char* m_labels[] = { LV_SYMBOL_PLAY "  Start Quiz", LV_SYMBOL_LIST "  Progress", LV_SYMBOL_SETTINGS "  Settings", LV_SYMBOL_VOLUME_MAX "  AI Tutor" };
    int xs[] = {10, 165, 10, 165}; int ys[] = {105, 105, 150, 150};
    for (int i = 0; i < kMenuCount; i++) {
        h_menu_[i] = lv_obj_create(home_panel_);
        lv_obj_set_size(h_menu_[i], 145, 40); lv_obj_set_pos(h_menu_[i], xs[i], ys[i]);
        apply_glass_card(h_menu_[i], t_card_, t_glass_opa_, settings_.theme_mode);
        lv_obj_clear_flag(h_menu_[i], LV_OBJ_FLAG_SCROLLABLE);
        h_menu_lbl_[i] = lv_label_create(h_menu_[i]);
        lv_label_set_text(h_menu_lbl_[i], m_labels[i]);
        lv_obj_align(h_menu_lbl_[i], LV_ALIGN_LEFT_MID, 8, 0);
    }

    lv_obj_t* footer = make_hdr(home_panel_, 20, false, t_card_, t_glass_opa_, settings_.theme_mode);
    lv_obj_set_pos(footer, 0, 220);
    h_footer_lbl_ = lv_label_create(footer);
    lv_label_set_text(h_footer_lbl_, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " nav  " LV_SYMBOL_RIGHT " open  Hold Agent=Home");
    lv_obj_align(h_footer_lbl_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(h_footer_lbl_, lv_color_hex(t_subtext_), 0);
}

void QuizUI::BuildQuizPanel() {
    quiz_panel_ = make_panel(screen_);
    lv_obj_t* topbar = make_hdr(quiz_panel_, 30, true, t_card_, t_glass_opa_, settings_.theme_mode);
    q_title_lbl_ = lv_label_create(topbar);
    lv_label_set_text(q_title_lbl_, "GMAT Quiz");
    lv_obj_align(q_title_lbl_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(q_title_lbl_, lv_color_hex(t_text_), 0);
    
    q_progress_lbl_ = lv_label_create(topbar);
    lv_obj_align(q_progress_lbl_, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(q_progress_lbl_, lv_color_hex(t_subtext_), 0);

    uint32_t tb = settings_.theme_mode == 0 ? 0xE5E5EA : 0x3A3A3C;
    q_prog_bar_ = make_bar(quiz_panel_, 0, 30, 320, 4, tb, t_primary_);

    q_score_lbl_ = lv_label_create(quiz_panel_);
    lv_obj_set_pos(q_score_lbl_, 0, 36); lv_obj_set_size(q_score_lbl_, 316, 16);
    lv_obj_set_style_text_color(q_score_lbl_, lv_color_hex(t_subtext_), 0);
    lv_obj_set_style_text_align(q_score_lbl_, LV_TEXT_ALIGN_RIGHT, 0);

    q_card_ = lv_obj_create(quiz_panel_);
    lv_obj_set_size(q_card_, 308, 54); lv_obj_set_pos(q_card_, 6, 46);
    apply_glass_card(q_card_, t_card_, t_glass_opa_, settings_.theme_mode);
    lv_obj_set_style_pad_all(q_card_, 6, 0);
    lv_obj_clear_flag(q_card_, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_style_bg_opa(q_card_, LV_OPA_40, 0);
    lv_obj_set_style_border_width(q_card_, 1, 0);
    lv_obj_set_style_border_color(q_card_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(q_card_, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(q_card_, 30, 0);
    lv_obj_set_style_shadow_color(q_card_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(q_card_, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(q_card_, 8, 0);
    lv_obj_set_style_radius(q_card_, 16, 0);

    question_lbl_ = lv_label_create(q_card_);
    lv_label_set_long_mode(question_lbl_, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(question_lbl_, 296, 44);
    lv_obj_set_style_text_color(question_lbl_, lv_color_hex(t_text_), 0);
    lv_obj_set_style_text_font(question_lbl_, &lv_font_montserrat_14, 0);

    q_timer_bar_ = make_bar(quiz_panel_, 6, 101, 308, 5, tb, t_accent_);

    const char* letters[] = {"A", "B", "C", "D"};
    int opt_xs[] = {6, 164, 6, 164}; int opt_ys[] = {112, 112, 164, 164};
    for (int i = 0; i < 4; i++) {
        opt_cards_[i] = lv_obj_create(quiz_panel_);
        lv_obj_set_size(opt_cards_[i], 150, 46); lv_obj_set_pos(opt_cards_[i], opt_xs[i], opt_ys[i]);
        apply_glass_card(opt_cards_[i], t_card_, t_glass_opa_, settings_.theme_mode);
        // Premium option card depth
        lv_obj_set_style_shadow_width(opt_cards_[i], 12, 0);
        lv_obj_set_style_shadow_color(opt_cards_[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(opt_cards_[i], LV_OPA_20, 0);
        lv_obj_set_style_shadow_ofs_y(opt_cards_[i], 4, 0);
        lv_obj_set_style_radius(opt_cards_[i], 12, 0);
        lv_obj_set_style_transform_pivot_x(opt_cards_[i], 75, 0);
        lv_obj_set_style_transform_pivot_y(opt_cards_[i], 23, 0);
        lv_obj_set_style_pad_all(opt_cards_[i], 0, 0);
        lv_obj_clear_flag(opt_cards_[i], LV_OBJ_FLAG_SCROLLABLE);

        opt_badge_[i] = lv_obj_create(opt_cards_[i]);
        lv_obj_set_size(opt_badge_[i], 24, 24); lv_obj_align(opt_badge_[i], LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_bg_color(opt_badge_[i], lv_color_hex(t_accent_), 0);
        lv_obj_set_style_radius(opt_badge_[i], 12, 0);
        lv_obj_set_style_border_width(opt_badge_[i], 0, 0);
        lv_obj_t* bl = lv_label_create(opt_badge_[i]);
        lv_label_set_text(bl, letters[i]); lv_obj_center(bl);
        lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);

        opt_lbls_[i] = lv_label_create(opt_cards_[i]);
        lv_label_set_long_mode(opt_lbls_[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(opt_lbls_[i], 114); lv_obj_align(opt_lbls_[i], LV_ALIGN_LEFT_MID, 32, 0);
        lv_obj_set_style_text_font(opt_lbls_[i], &lv_font_montserrat_14, 0);
    }

    q_feedback_bar_ = make_hdr(quiz_panel_, 20, false, t_card_, t_glass_opa_, settings_.theme_mode);
    lv_obj_set_pos(q_feedback_bar_, 0, 220);
    q_feedback_lbl_ = lv_label_create(q_feedback_bar_);
    lv_obj_align(q_feedback_lbl_, LV_ALIGN_LEFT_MID, 6, 0);
}

void QuizUI::BuildSettingsPanel() {
    settings_panel_ = make_panel(screen_);
    lv_obj_t* hdr = make_hdr(settings_panel_, 32, true, t_card_, t_glass_opa_, settings_.theme_mode);
    lv_obj_t* hl = lv_label_create(hdr);
    lv_label_set_text(hl, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(t_text_), 0);

    const char* s_names[] = {"Quiz Timer", "Sound FX", "Show Correct", "Theme Mode", "Glass Effect", "Bg Anim", "Brightness", "Volume", LV_SYMBOL_LEFT "  Back"};
    int s_xs[] = {8, 164, 8, 164, 8, 164, 8, 164, 8};
    int s_ys[] = {40, 40, 85, 85, 130, 130, 175, 175, 212};
    for (int i = 0; i < kSettingsCount; i++) {
        s_items_[i] = lv_obj_create(settings_panel_);
        lv_obj_set_size(s_items_[i], 148, 38);
        if (i == 8) lv_obj_set_size(s_items_[i], 304, 26);
        lv_obj_set_pos(s_items_[i], s_xs[i], s_ys[i]);
        apply_glass_card(s_items_[i], t_card_, t_glass_opa_, settings_.theme_mode);
        lv_obj_set_style_pad_all(s_items_[i], 4, 0);
        lv_obj_clear_flag(s_items_[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* n = lv_label_create(s_items_[i]);
        lv_label_set_text(n, s_names[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 4, 0);
        if (i < 8) {
            s_vals_[i] = lv_label_create(s_items_[i]);
            lv_obj_align(s_vals_[i], LV_ALIGN_RIGHT_MID, -4, 0);
            lv_obj_set_style_text_color(s_vals_[i], lv_color_hex(t_accent_), 0);
        }
    }
}

void QuizUI::BuildProgressPanel() {
    progress_panel_ = make_panel(screen_);
    lv_obj_t* hdr = make_hdr(progress_panel_, 36, true, t_card_, t_glass_opa_, settings_.theme_mode);
    lv_obj_t* hl = lv_label_create(hdr);
    lv_label_set_text(hl, LV_SYMBOL_LIST "  Progress Report");
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(t_text_), 0);

    lv_obj_t* card = lv_obj_create(progress_panel_);
    lv_obj_set_size(card, 304, 174); lv_obj_set_pos(card, 8, 40);
    apply_glass_card(card, t_card_, t_glass_opa_, settings_.theme_mode);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    p_answered_lbl_ = lv_label_create(card); lv_obj_set_pos(p_answered_lbl_, 0, 4);
    lv_obj_set_style_text_color(p_answered_lbl_, lv_color_hex(t_text_), 0);

    p_correct_lbl_ = lv_label_create(card); lv_obj_set_pos(p_correct_lbl_, 150, 4);
    lv_obj_set_style_text_color(p_correct_lbl_, lv_color_hex(t_correct_), 0);

    p_accuracy_lbl_ = lv_label_create(card); lv_obj_set_pos(p_accuracy_lbl_, 0, 34);
    lv_obj_set_style_text_color(p_accuracy_lbl_, lv_color_hex(t_accent_), 0);

    p_streak_lbl_ = lv_label_create(card); lv_obj_set_pos(p_streak_lbl_, 0, 64);
    lv_obj_set_style_text_color(p_streak_lbl_, lv_color_hex(t_subtext_), 0);

    lv_obj_t* chart = lv_chart_create(card);
    lv_obj_set_size(chart, 280, 70); lv_obj_set_pos(chart, 0, 85);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 7); lv_chart_set_div_line_count(chart, 3, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_chart_series_t* ser = lv_chart_add_series(chart, lv_color_hex(t_primary_), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_next_value(chart, ser, 20); lv_chart_set_next_value(chart, ser, 45);
    lv_chart_set_next_value(chart, ser, 30); lv_chart_set_next_value(chart, ser, 80);
    lv_chart_set_next_value(chart, ser, 60); lv_chart_set_next_value(chart, ser, 95);
    lv_chart_set_next_value(chart, ser, 100);
}

// ──────────────────────────────────────────────────────────────────────────────
// Panel Switching
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::SwitchToPanel(lv_obj_t* panel) {
    lv_obj_t* all[] = {home_panel_, quiz_panel_, settings_panel_, progress_panel_};
    for (auto p : all) if (p) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    if (!panel) return;
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, panel);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 250); lv_anim_start(&a);
}

void QuizUI::Show(lv_obj_t* default_screen) {
    if (!screen_) return;
    if (!lvgl_port_lock(-1)) return;
    
    if (default_screen && default_screen != screen_) default_screen_ = default_screen;
    lv_scr_load_anim(screen_, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
    is_visible_ = true;
    if (mid_quiz_) { SwitchToPanel(quiz_panel_); mode_ = QuizMode::kQuiz; }
    else ShowHome();
    
    lvgl_port_unlock();
}

void QuizUI::Hide() {
    StopTimer(); if (mode_ == QuizMode::kQuiz) mid_quiz_ = true;
    if (!lvgl_port_lock(-1)) return;
    
    if (is_visible_ && default_screen_ && default_screen_ != screen_)
        lv_scr_load_anim(default_screen_, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 300, 0, false);
    is_visible_ = false;
    
    lvgl_port_unlock();
}

void QuizUI::GoHome() {
    if (!is_visible_) return; 
    StopTimer(); 
    mid_quiz_ = false; 
    ShowHome();
}

// ──────────────────────────────────────────────────────────────────────────────
// Home
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::ShowHome() {
    mode_ = QuizMode::kHome; home_sel_ = 0; SwitchToPanel(home_panel_);
    UpdateHomeStats(); HomeNavigate(0);
}

void QuizUI::UpdateHomeStats() {
    int acc = stats_.total_answered > 0 ? (stats_.total_correct * 100 / stats_.total_answered) : 0;
    char buf[52]; snprintf(buf, sizeof(buf), "Answered: %d/%d   Accuracy: %d%%", stats_.total_answered, (int)questions_.size(), acc);
    lv_label_set_text(h_stats_lbl_, buf);
    char ab[8]; snprintf(ab, sizeof(ab), "%d%%", acc); lv_label_set_text(h_arc_lbl_, ab);
    lv_bar_set_value(h_stat_bar_, questions_.empty() ? 0 : (stats_.total_answered * 100 / (int)questions_.size()), LV_ANIM_ON);
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, h_arc_); lv_anim_set_exec_cb(&a, anim_arc_cb);
    lv_anim_set_values(&a, 0, acc); lv_anim_set_time(&a, 900); lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
}

void QuizUI::HomeNavigate(int delta) {
    if (delta != 0) PlayUISound(Lang::Sounds::OGG_POPUP);
    home_sel_ = (home_sel_ + delta + kMenuCount) % kMenuCount;
    for (int i = 0; i < kMenuCount; i++) {
        if (i == home_sel_) {
            lv_obj_set_style_bg_color(h_menu_[i], lv_color_hex(t_primary_), 0);
            lv_obj_set_style_bg_opa(h_menu_[i], LV_OPA_COVER, 0); 
            lv_obj_set_style_text_color(h_menu_lbl_[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(h_menu_[i], lv_color_hex(t_card_), 0);
            lv_obj_set_style_bg_opa(h_menu_[i], t_glass_opa_, 0);
            lv_obj_set_style_text_color(h_menu_lbl_[i], lv_color_hex(t_text_), 0);
        }
    }
}

void QuizUI::HomeSelect() {
    PlayUISound(Lang::Sounds::OGG_SUCCESS);
    switch (home_sel_) {
        case 0: EnterQuiz(); break;
        case 1: ShowProgress(); break;
        case 2: OpenSettings(false); break;
        case 3: Application::GetInstance().ToggleChatState(); break;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Quiz
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::EnterQuiz() {
    mode_ = QuizMode::kQuiz; mid_quiz_ = true; joy_cursor_ = -1;
    SwitchToPanel(quiz_panel_); DisplayCurrentQuestion();
}

void QuizUI::DisplayCurrentQuestion() {
    if (questions_.empty()) { lv_label_set_text(question_lbl_, "No questions loaded."); return; }
    if (q_idx_ >= (int)questions_.size()) q_idx_ = questions_.size() - 1;
    const QuizQuestion& q = questions_[q_idx_];
    lv_label_set_text(question_lbl_, q.q.c_str());
    for (int i = 0; i < 4; i++) lv_label_set_text(opt_lbls_[i], i < (int)q.opts.size() ? q.opts[i].c_str() : "-");
    ResetAllOpts(); SetFeedback("A-D answer   " LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " cursor", t_card_);
    ans_revealed_ = false; selected_ans_ = -1; joy_cursor_ = -1;
    UpdateQuizProgress();
    if (settings_.timer_seconds > 0) { lv_obj_clear_flag(q_timer_bar_, LV_OBJ_FLAG_HIDDEN); StartTimer(); }
    else lv_obj_add_flag(q_timer_bar_, LV_OBJ_FLAG_HIDDEN);
}

void QuizUI::UpdateQuizProgress() {
    char buf[12]; snprintf(buf, sizeof(buf), "%d/%d", (int)(q_idx_ + 1), (int)questions_.size());
    lv_label_set_text(q_progress_lbl_, buf);
    lv_bar_set_value(q_prog_bar_, (int)((q_idx_ + 1) * 100 / questions_.size()), LV_ANIM_ON);
    char s[24]; snprintf(s, sizeof(s), "Score: %d", stats_.session_score);
    lv_label_set_text(q_score_lbl_, s);
}

void QuizUI::ResetAllOpts() {
    uint32_t b = settings_.theme_mode == 0 ? 0xE5E5EA : 0x2C2C2E;
    for (int i = 0; i < 4; i++) {
        HighlightOpt(i, t_card_, b, t_accent_, t_text_, t_glass_opa_);
        // Reset scale
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, opt_cards_[i]); lv_anim_set_exec_cb(&a, anim_zoom_cb);
        lv_anim_set_values(&a, 270, 256);
        lv_anim_set_time(&a, 150); lv_anim_start(&a);
    }
}

void QuizUI::HighlightOpt(int i, uint32_t card_bg, uint32_t border, uint32_t badge, uint32_t tcol, lv_opa_t opa) {
    if (i < 0 || i >= 4) return;
    lv_obj_set_style_bg_color(opt_cards_[i], lv_color_hex(card_bg), 0);
    lv_obj_set_style_bg_opa(opt_cards_[i], opa, 0);
    lv_obj_set_style_border_color(opt_cards_[i], lv_color_hex(border), 0);
    lv_obj_set_style_bg_color(opt_badge_[i], lv_color_hex(badge), 0);
    lv_obj_set_style_text_color(opt_lbls_[i], lv_color_hex(tcol), 0);
}

void QuizUI::FlashOpt(int i) {
    lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, opt_cards_[i]); lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_time(&a, 150); lv_anim_set_playback_time(&a, 150);
    lv_anim_set_repeat_count(&a, 2); lv_anim_start(&a);
}

void QuizUI::MoveCursor(int delta) {
    if (!ans_revealed_) ResetAllOpts();
    PlayUISound(Lang::Sounds::OGG_POPUP);
    joy_cursor_ = (joy_cursor_ + delta + 4) % 4;
    if (!ans_revealed_) {
        HighlightOpt(joy_cursor_, t_primary_, t_primary_, 0xFFFFFF, 0xFFFFFF, LV_OPA_COVER);
        // Pop out scale animation for highlighted option
        lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, opt_cards_[joy_cursor_]); lv_anim_set_exec_cb(&a, anim_zoom_cb);
        lv_anim_set_values(&a, 256, 270);
        lv_anim_set_time(&a, 150); lv_anim_set_path_cb(&a, lv_anim_path_overshoot); lv_anim_start(&a);
    }
}

void QuizUI::ConfirmCursorSelection() {
    if (joy_cursor_ >= 0 && !ans_revealed_) SelectAnswer(joy_cursor_);
    else if (ans_revealed_) NextQuestion();
}

void QuizUI::SelectAnswer(int idx) {
    if (!is_visible_ || mode_ != QuizMode::kQuiz || ans_revealed_) return;
    StopTimer(); ans_revealed_ = true; selected_ans_ = idx;
    FlashOpt(idx);
    const QuizQuestion& q = questions_[q_idx_];
    bool correct = (idx == q.ans);
    if (correct) {
        PlayUISound(Lang::Sounds::OGG_SUCCESS);
        stats_.session_score++; stats_.total_correct++; stats_.current_streak++;
        stats_.best_streak = std::max(stats_.best_streak, stats_.current_streak);
        HighlightOpt(idx, t_correct_, t_correct_, t_correct_, 0xFFFFFF, LV_OPA_COVER);
        char fb[40]; snprintf(fb, sizeof(fb), LV_SYMBOL_OK "  Correct!  Streak: %d", stats_.current_streak);
        SetFeedback(fb, t_correct_);
    } else {
        PlayUISound(Lang::Sounds::OGG_EXCLAMATION);
        stats_.current_streak = 0;
        HighlightOpt(idx, t_wrong_, t_wrong_, t_wrong_, 0xFFFFFF, LV_OPA_COVER);
        if (settings_.show_correct) HighlightOpt(q.ans, t_correct_, t_correct_, t_correct_, 0xFFFFFF, LV_OPA_COVER);
        char fb[40]; snprintf(fb, sizeof(fb), LV_SYMBOL_CLOSE "  Wrong!  Correct: %c", 'A' + q.ans);
        SetFeedback(fb, t_wrong_);
    }
    stats_.total_answered++; stats_.session_total++; UpdateQuizProgress();
    // Persist result to SRS database for Anki algorithm
    SrsDatabase::GetInstance().SaveSession(static_cast<uint32_t>(q_idx_), correct);
}

void QuizUI::SetFeedback(const char* msg, uint32_t bg) {
    lv_label_set_text(q_feedback_lbl_, msg);
    lv_obj_set_style_bg_color(q_feedback_bar_, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(q_feedback_lbl_, lv_color_hex(bg == t_card_ ? t_subtext_ : 0xFFFFFF), 0);
}

void QuizUI::CycleBackgroundColor(int index) {
    // Smooth Hue shift per question. Hues from 0 to 360 mapped to HEX. 
    // We'll keep it simple by interpolating a few base elegant colors.
    uint32_t colors[] = { 0x0A84FF, 0x30D158, 0xFF9F0A, 0xFF375F, 0x5E5CE6, 0x32ADE6 };
    int c_idx = index % (sizeof(colors)/sizeof(colors[0]));
    uint32_t color = colors[c_idx];
    
    // Animate background color smoothly if in Solid mode. In Glass/Aurora mode, aurora does the job.
    if (settings_.bg_anim == 0) { // Static Solid
        lv_obj_set_style_bg_color(screen_, lv_color_hex(color), 0);
    }
}

void QuizUI::NextQuestion() {
    if (q_idx_ + 1 < questions_.size()) { 
        // Swipe left transition
        lv_anim_t a_out; lv_anim_init(&a_out);
        lv_anim_set_var(&a_out, q_card_);
        lv_anim_set_values(&a_out, lv_obj_get_x(q_card_), -LV_HOR_RES);
        lv_anim_set_time(&a_out, 200);
        lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in);
        lv_anim_start(&a_out);
        
        q_idx_++; 
        CycleBackgroundColor(q_idx_);
        DisplayCurrentQuestion(); 
        
        lv_anim_t a_in; lv_anim_init(&a_in);
        lv_anim_set_var(&a_in, q_card_);
        lv_anim_set_values(&a_in, LV_HOR_RES, 6); // Original pos is 6
        lv_anim_set_time(&a_in, 300);
        lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
        lv_anim_start(&a_in);
    }
    else ShowResult();
}
void QuizUI::PrevQuestion() { 
    if (q_idx_ > 0) { 
        // Swipe right transition
        lv_anim_t a_out; lv_anim_init(&a_out);
        lv_anim_set_var(&a_out, q_card_);
        lv_anim_set_values(&a_out, lv_obj_get_x(q_card_), LV_HOR_RES);
        lv_anim_set_time(&a_out, 200);
        lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in);
        lv_anim_start(&a_out);
        
        q_idx_--; 
        CycleBackgroundColor(q_idx_);
        DisplayCurrentQuestion(); 
        
        lv_anim_t a_in; lv_anim_init(&a_in);
        lv_anim_set_var(&a_in, q_card_);
        lv_anim_set_values(&a_in, -LV_HOR_RES, 6);
        lv_anim_set_time(&a_in, 300);
        lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
        lv_anim_start(&a_in);
    } 
}

void QuizUI::StartTimer() {
    if (!q_timer_ || settings_.timer_seconds <= 0) return;
    StopTimer(); timer_rem_ = settings_.timer_seconds; UpdateTimerUI();
    esp_timer_start_periodic(q_timer_, 1000000ULL);
}
void QuizUI::StopTimer() { if (q_timer_) esp_timer_stop(q_timer_); }

void QuizUI::UpdateTimerUI() {
    int pct = (timer_rem_ * 100) / std::max(1, settings_.timer_seconds);
    lv_bar_set_value(q_timer_bar_, pct, LV_ANIM_ON);
    uint32_t c = timer_rem_ > settings_.timer_seconds / 2 ? t_primary_ : timer_rem_ > 5 ? 0xFF9500 : t_wrong_;
    lv_obj_set_style_bg_color(q_timer_bar_, lv_color_hex(c), LV_PART_INDICATOR);
}

void QuizUI::OnTimerTick() {
    if (!is_visible_ || mode_ != QuizMode::kQuiz) { StopTimer(); return; }
    timer_rem_--;
    if (lvgl_port_lock(-1)) {
        UpdateTimerUI();
        if (timer_rem_ <= 0) {
            StopTimer();
            if (!ans_revealed_) {
                PlayUISound(Lang::Sounds::OGG_EXCLAMATION); ans_revealed_ = true;
                if (settings_.show_correct) HighlightOpt(questions_[q_idx_].ans, t_correct_, t_correct_, t_correct_, 0xFFFFFF, LV_OPA_COVER);
                SetFeedback(LV_SYMBOL_CLOSE "  Time's Up!", t_wrong_);
                stats_.total_answered++; stats_.session_total++; stats_.current_streak = 0; UpdateQuizProgress();
            }
        }
        lvgl_port_unlock();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Settings
// ──────────────────────────────────────────────────────────────────────────────

const char* QuizUI::TimerLabel() const {
    switch (settings_.timer_seconds) { case 10: return "10s"; case 20: return "20s"; case 30: return "30s"; default: return "Off"; }
}

void QuizUI::ApplySettingsAction(int delta) {
    PlayUISound(Lang::Sounds::OGG_POPUP);
    bool rebuild_needed = false;
    switch (s_sel_) {
        case 0: { static const int v[] = {0, 10, 20, 30}; static int t = 0; t = (t + (delta > 0 ? 1 : 3)) % 4; settings_.timer_seconds = v[t]; break; }
        case 1: settings_.sound_enabled = !settings_.sound_enabled; break;
        case 2: settings_.show_correct = !settings_.show_correct; break;
        case 3: settings_.theme_mode = (settings_.theme_mode + 1) % 2; rebuild_needed = true; break;
        case 4: settings_.glass_effect = !settings_.glass_effect; rebuild_needed = true; break;
        case 5: settings_.bg_anim = (settings_.bg_anim + (delta > 0 ? 1 : 2)) % 3; rebuild_needed = true; break;
        case 6: settings_.brightness_pct = std::max(10, std::min(100, settings_.brightness_pct + delta * 10)); break;
        case 7: settings_.volume_pct = std::max(0, std::min(100, settings_.volume_pct + delta * 10)); break;
    }
    if (rebuild_needed) RebuildUI();
    else RenderSettings();
}

void QuizUI::RenderSettings() {
    const char* oo[] = {"Off", "On"};
    const char* themes[] = {"Light", "Dark"};
    const char* bga[] = {"Off", "Aurora", "Static"};
    lv_label_set_text(s_vals_[0], TimerLabel());
    lv_label_set_text(s_vals_[1], oo[settings_.sound_enabled]);
    lv_label_set_text(s_vals_[2], oo[settings_.show_correct]);
    lv_label_set_text(s_vals_[3], themes[settings_.theme_mode]);
    lv_label_set_text(s_vals_[4], oo[settings_.glass_effect]);
    lv_label_set_text(s_vals_[5], bga[settings_.bg_anim]);
    
    char b[8];
    snprintf(b, sizeof(b), "%d%%", settings_.brightness_pct); lv_label_set_text(s_vals_[6], b);
    snprintf(b, sizeof(b), "%d%%", settings_.volume_pct);     lv_label_set_text(s_vals_[7], b);

    for (int i = 0; i < kSettingsCount; i++) {
        if (i == s_sel_) {
            lv_obj_set_style_bg_color(s_items_[i], lv_color_hex(t_primary_), 0);
            lv_obj_set_style_bg_opa(s_items_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_items_[i], 0), lv_color_hex(0xFFFFFF), 0);
            if (i < 8) lv_obj_set_style_text_color(s_vals_[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(s_items_[i], lv_color_hex(t_card_), 0);
            lv_obj_set_style_bg_opa(s_items_[i], t_glass_opa_, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(s_items_[i], 0), lv_color_hex(t_text_), 0);
            if (i < 8) lv_obj_set_style_text_color(s_vals_[i], lv_color_hex(t_accent_), 0);
        }
    }
}

void QuizUI::OpenSettings(bool from_quiz) {
    s_from_quiz_ = from_quiz; mode_ = QuizMode::kSettings;
    SwitchToPanel(settings_panel_); RenderSettings();
}
void QuizUI::CloseSettings() { if (s_from_quiz_) EnterQuiz(); else ShowHome(); }

// ──────────────────────────────────────────────────────────────────────────────
// Progress
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::ShowProgress() {
    mode_ = QuizMode::kProgress; SwitchToPanel(progress_panel_); UpdateProgressUI();
}

void QuizUI::UpdateProgressUI() {
    int acc = stats_.total_answered > 0 ? (stats_.total_correct * 100 / stats_.total_answered) : 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "Total Answered: %d / %d", stats_.total_answered, (int)questions_.size()); lv_label_set_text(p_answered_lbl_, buf);
    snprintf(buf, sizeof(buf), "Correct: %d", stats_.total_correct); lv_label_set_text(p_correct_lbl_, buf);
    snprintf(buf, sizeof(buf), "Accuracy: %d%%", acc); lv_label_set_text(p_accuracy_lbl_, buf);
    snprintf(buf, sizeof(buf), "Best: %d  |  Current: %d", stats_.best_streak, stats_.current_streak); lv_label_set_text(p_streak_lbl_, buf);
}

// ──────────────────────────────────────────────────────────────────────────────
// Result
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::ShowResult() {
    StopTimer(); mode_ = QuizMode::kResult; mid_quiz_ = false;
    PlayUISound(Lang::Sounds::OGG_SUCCESS);
    lv_obj_t* all[] = {home_panel_, settings_panel_, progress_panel_};
    for (auto p : all) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(quiz_panel_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ov = lv_obj_create(quiz_panel_);
    lv_obj_set_size(ov, 320, 240); lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(t_bg_), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0); lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    int acc = stats_.session_total > 0 ? (stats_.session_score * 100 / stats_.session_total) : 0;
    auto ml = [&](const char* t, uint32_t c, lv_align_t a, int xo, int yo) {
        lv_obj_t* l = lv_label_create(ov); lv_label_set_text(l, t); lv_obj_align(l, a, xo, yo);
        lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    };
    ml(LV_SYMBOL_OK, t_correct_, LV_ALIGN_TOP_MID, 0, 30);
    ml("Quiz Complete!", t_primary_, LV_ALIGN_TOP_MID, 0, 60);
    char buf[40]; snprintf(buf, sizeof(buf), "Score: %d / %d", stats_.session_score, stats_.session_total);
    ml(buf, t_text_, LV_ALIGN_TOP_MID, 0, 95);
    snprintf(buf, sizeof(buf), "Accuracy: %d%%", acc);
    ml(buf, t_accent_, LV_ALIGN_TOP_MID, 0, 125);
    snprintf(buf, sizeof(buf), "Best Streak: %d", stats_.best_streak);
    ml(buf, t_subtext_, LV_ALIGN_TOP_MID, 0, 153);
    ml(LV_SYMBOL_RIGHT " restart   " LV_SYMBOL_LEFT " home", t_subtext_, LV_ALIGN_BOTTOM_MID, 0, -10);

    stats_.session_score = 0; stats_.session_total = 0;
}

// ──────────────────────────────────────────────────────────────────────────────
// Inputs
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::HandleButtonA() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(0); }
void QuizUI::HandleButtonB() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(1); }
void QuizUI::HandleButtonC() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(2); }
void QuizUI::HandleButtonD() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(3); }

void QuizUI::HandleJoyUp() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeNavigate(-2); break;
        case QuizMode::kQuiz:     MoveCursor(-2); break;
        case QuizMode::kSettings: PlayUISound(Lang::Sounds::OGG_POPUP); s_sel_ = (s_sel_ + kSettingsCount - 2 + kSettingsCount) % kSettingsCount; RenderSettings(); break;
        default: break;
    }
}
void QuizUI::HandleJoyDown() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeNavigate(2); break;
        case QuizMode::kQuiz:     MoveCursor(2); break;
        case QuizMode::kSettings: PlayUISound(Lang::Sounds::OGG_POPUP); s_sel_ = (s_sel_ + 2) % kSettingsCount; RenderSettings(); break;
        default: break;
    }
}
void QuizUI::HandleJoyLeft() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeNavigate(-1); break;
        case QuizMode::kQuiz:     if (!ans_revealed_) MoveCursor(-1); else PrevQuestion(); break;
        case QuizMode::kSettings: ApplySettingsAction(-1); break;
        case QuizMode::kProgress: ShowHome(); break;
        case QuizMode::kResult:   ShowHome(); break;
        default: break;
    }
}
void QuizUI::HandleJoyRight() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeNavigate(1); break;
        case QuizMode::kQuiz:     if (!ans_revealed_) MoveCursor(1); else NextQuestion(); break;
        case QuizMode::kSettings: ApplySettingsAction(1); break;
        case QuizMode::kProgress: EnterQuiz(); break;
        case QuizMode::kResult:   q_idx_ = 0; SwitchToPanel(quiz_panel_); DisplayCurrentQuestion(); mode_ = QuizMode::kQuiz; mid_quiz_ = true; break;
        default: break;
    }
}
void QuizUI::HandleJoyPress() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeSelect(); break;
        case QuizMode::kQuiz:     ConfirmCursorSelection(); break;
        case QuizMode::kSettings:
            if (s_sel_ == kSettingsCount - 1) CloseSettings();
            else ApplySettingsAction(1);
            break;
        case QuizMode::kProgress: ShowHome(); break;
        default: break;
    }
}
void QuizUI::HandleJoyPressLong() { if (is_visible_ && mode_ == QuizMode::kQuiz) OpenSettings(true); }
void QuizUI::HandleAgentLongPress() { if (is_visible_) GoHome(); }
