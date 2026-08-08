#include "quiz_ui.h"
#include "application.h"
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_lvgl_port.h>
#include <cJSON.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdio>

#define TAG "QuizUI"

// ──────────────────────────────────────────────────────────────────────────────
// Static LVGL animation callbacks (C function pointers required)
// ──────────────────────────────────────────────────────────────────────────────

static void anim_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}
static void anim_arc_cb(void* obj, int32_t v) {
    lv_arc_set_value((lv_obj_t*)obj, v);
}
static void anim_bar_cb(void* obj, int32_t v) {
    lv_bar_set_value((lv_obj_t*)obj, v, LV_ANIM_OFF);
}
static void anim_x_cb(void* obj, int32_t v) {
    lv_obj_set_x((lv_obj_t*)obj, v);
}
static void quiz_timer_cb(void* arg) {
    QuizUI* self = static_cast<QuizUI*>(arg);
    self->OnTimerTick();
}

// ──────────────────────────────────────────────────────────────────────────────
// UI factory helpers — keep object count low
// ──────────────────────────────────────────────────────────────────────────────

static lv_obj_t* make_panel(lv_obj_t* parent, uint32_t bg) {
    lv_obj_t* p = lv_obj_create(parent);
    if (!p) return nullptr;
    lv_obj_set_size(p, 320, 240);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h,
                           uint32_t track, uint32_t ind) {
    lv_obj_t* b = lv_bar_create(parent);
    if (!b) return nullptr;
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, lv_color_hex(track), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ind), LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
    return b;
}

static lv_obj_t* make_hdr(lv_obj_t* parent, uint32_t bg, int h = 36) {
    lv_obj_t* hdr = lv_obj_create(parent);
    if (!hdr) return nullptr;
    lv_obj_set_size(hdr, 320, h);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    return hdr;
}

// ──────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ──────────────────────────────────────────────────────────────────────────────

QuizUI::QuizUI() {}
QuizUI::~QuizUI() {
    StopTimer();
    if (q_timer_) esp_timer_delete(q_timer_);
}

void QuizUI::Initialize() {
    ESP_LOGI(TAG, "Initializing QuizUI (Landscape)");

    // SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/mcq", .partition_label = "mcq_data",
        .max_files = 5, .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) LoadQuestions();
    else ESP_LOGE(TAG, "SPIFFS: %s", esp_err_to_name(ret));

    // esp_timer for countdown
    esp_timer_create_args_t ta = { .callback = quiz_timer_cb, .arg = this, .name = "quiz" };
    esp_timer_create(&ta, &q_timer_);

    // Root screen
    screen_ = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

    // Only build home panel at startup — quiz/settings/progress are lazy
    BuildHomePanel();
    SwitchToPanel(home_panel_);

    ESP_LOGI(TAG, "QuizUI ready — %d questions", (int)questions_.size());
}

// ──────────────────────────────────────────────────────────────────────────────
// JSON loading
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::LoadQuestions() {
    std::ifstream f("/mcq/quiz.json");
    if (!f.is_open()) { ESP_LOGE(TAG, "quiz.json not found"); return; }
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
    ESP_LOGI(TAG, "Loaded %d questions", (int)questions_.size());
}

// ──────────────────────────────────────────────────────────────────────────────
// Build: Home panel (Landscape: 320x240)
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::BuildHomePanel() {
    home_panel_ = make_panel(screen_, kBg);
    if (!home_panel_) { ESP_LOGE(TAG, "OOM: home_panel_"); return; }

    // Header bar
    lv_obj_t* hdr = make_hdr(home_panel_, kPrimary, 70);

    h_title_lbl_ = lv_label_create(hdr);
    lv_label_set_text(h_title_lbl_, LV_SYMBOL_PLAY "  EduBoard");
    lv_obj_align(h_title_lbl_, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_text_color(h_title_lbl_, lv_color_hex(0xFFFFFF), 0);

    h_sub_lbl_ = lv_label_create(hdr);
    lv_label_set_text(h_sub_lbl_, "Smart Learning Assistant");
    lv_obj_align(h_sub_lbl_, LV_ALIGN_TOP_LEFT, 10, 28);
    lv_obj_set_style_text_color(h_sub_lbl_, lv_color_hex(0xBBDEFB), 0);

    // Score arc (top-right of header)
    h_arc_ = lv_arc_create(hdr);
    if (h_arc_) {
        lv_obj_set_size(h_arc_, 48, 48);
        lv_obj_align(h_arc_, LV_ALIGN_TOP_RIGHT, -8, 6);
        lv_arc_set_range(h_arc_, 0, 100);
        lv_arc_set_value(h_arc_, 0);
        lv_arc_set_bg_angles(h_arc_, 135, 45);
        lv_obj_set_style_arc_color(h_arc_, lv_color_hex(0x1E5799), LV_PART_MAIN);
        lv_obj_set_style_arc_color(h_arc_, lv_color_hex(0x69F0AE), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(h_arc_, 5, LV_PART_MAIN);
        lv_obj_set_style_arc_width(h_arc_, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(h_arc_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(h_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    }

    h_arc_lbl_ = lv_label_create(hdr);
    lv_label_set_text(h_arc_lbl_, "0%");
    lv_obj_align(h_arc_lbl_, LV_ALIGN_TOP_RIGHT, -18, 25);
    lv_obj_set_style_text_color(h_arc_lbl_, lv_color_hex(0x69F0AE), 0);

    // Stats row
    lv_obj_t* stats_row = lv_obj_create(home_panel_);
    if (stats_row) {
        lv_obj_set_size(stats_row, 320, 24);
        lv_obj_set_pos(stats_row, 0, 70);
        lv_obj_set_style_bg_color(stats_row, lv_color_hex(0xE3F2FD), 0);
        lv_obj_set_style_border_width(stats_row, 0, 0);
        lv_obj_set_style_radius(stats_row, 0, 0);
        lv_obj_set_style_pad_all(stats_row, 0, 0);
        lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_SCROLLABLE);
        h_stats_lbl_ = lv_label_create(stats_row);
        lv_label_set_text(h_stats_lbl_, "Answered: 0/0   Accuracy: 0%");
        lv_obj_align(h_stats_lbl_, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_text_color(h_stats_lbl_, lv_color_hex(kPrimary), 0);
    }

    // Thin progress bar
    h_stat_bar_ = make_bar(home_panel_, 0, 94, 320, 4, 0xBBDEFB, kAccent);

    // Menu items (4x, 2 columns)
    const char* menu_labels[] = {
        LV_SYMBOL_PLAY "  Start Quiz",
        LV_SYMBOL_LIST "  Progress",
        LV_SYMBOL_SETTINGS "  Settings",
        LV_SYMBOL_VOLUME_MAX "  AI Tutor",
    };
    int xs[] = {10, 165, 10, 165};
    int ys[] = {105, 105, 145, 145};
    for (int i = 0; i < kMenuCount; i++) {
        lv_obj_t* item = lv_obj_create(home_panel_);
        if (!item) continue;
        h_menu_[i] = item;
        lv_obj_set_size(item, 145, 34);
        lv_obj_set_pos(item, xs[i], ys[i]);
        lv_obj_set_style_bg_color(item, lv_color_hex(kCard), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(0x90CAF9), 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        h_menu_lbl_[i] = lv_label_create(item);
        lv_label_set_text(h_menu_lbl_[i], menu_labels[i]);
        lv_obj_align(h_menu_lbl_[i], LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_text_color(h_menu_lbl_[i], lv_color_hex(kText), 0);

        lv_obj_t* arrow = lv_label_create(item);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(kAccent), 0);
    }

    // Footer
    lv_obj_t* footer = make_hdr(home_panel_, kPrimary, 20);
    if (footer) {
        lv_obj_set_pos(footer, 0, 220);
        h_footer_lbl_ = lv_label_create(footer);
        lv_label_set_text(h_footer_lbl_, LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " nav  " LV_SYMBOL_RIGHT " open  Hold Agent=Home");
        lv_obj_align(h_footer_lbl_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(h_footer_lbl_, lv_color_hex(0xBBDEFB), 0);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Build: Quiz panel (Landscape: 320x240)
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::BuildQuizPanel() {
    quiz_panel_ = make_panel(screen_, kBg);
    if (!quiz_panel_) { ESP_LOGE(TAG, "OOM: quiz_panel_"); return; }
    quiz_built_ = true;

    // Top bar
    lv_obj_t* topbar = make_hdr(quiz_panel_, kPrimary, 30);
    q_title_lbl_    = lv_label_create(topbar);
    lv_label_set_text(q_title_lbl_, "GMAT Quiz");
    lv_obj_align(q_title_lbl_, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(q_title_lbl_, lv_color_hex(0xFFFFFF), 0);
    q_progress_lbl_ = lv_label_create(topbar);
    lv_label_set_text(q_progress_lbl_, "0/0");
    lv_obj_align(q_progress_lbl_, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(q_progress_lbl_, lv_color_hex(0xBBDEFB), 0);

    // Progress bar
    q_prog_bar_ = make_bar(quiz_panel_, 0, 30, 320, 4, 0xBBDEFB, 0x69F0AE);

    // Score label
    q_score_lbl_ = lv_label_create(quiz_panel_);
    lv_label_set_text(q_score_lbl_, "Score: 0");
    lv_obj_set_pos(q_score_lbl_, 0, 36);
    lv_obj_set_size(q_score_lbl_, 316, 16);
    lv_obj_set_style_text_color(q_score_lbl_, lv_color_hex(kSubtext), 0);
    lv_obj_set_style_text_align(q_score_lbl_, LV_TEXT_ALIGN_RIGHT, 0);

    // Question card
    q_card_ = lv_obj_create(quiz_panel_);
    if (q_card_) {
        lv_obj_set_size(q_card_, 308, 54);
        lv_obj_set_pos(q_card_, 6, 46);
        lv_obj_set_style_bg_color(q_card_, lv_color_hex(kCard), 0);
        lv_obj_set_style_border_color(q_card_, lv_color_hex(0xBBDEFB), 0);
        lv_obj_set_style_border_width(q_card_, 2, 0);
        lv_obj_set_style_radius(q_card_, 8, 0);
        lv_obj_set_style_pad_all(q_card_, 6, 0);
        lv_obj_clear_flag(q_card_, LV_OBJ_FLAG_SCROLLABLE);
        question_lbl_ = lv_label_create(q_card_);
        lv_label_set_long_mode(question_lbl_, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(question_lbl_, 296, 44);
        lv_obj_set_pos(question_lbl_, 0, 0);
        lv_obj_set_style_text_color(question_lbl_, lv_color_hex(kText), 0);
        lv_label_set_text(question_lbl_, questions_.empty() ? "No questions." : "...");
    }

    // Timer bar (hidden by default)
    q_timer_bar_ = make_bar(quiz_panel_, 6, 101, 308, 5, 0xEEEEEE, kAccent);
    if (q_timer_bar_) lv_obj_add_flag(q_timer_bar_, LV_OBJ_FLAG_HIDDEN);
    q_timer_lbl_ = lv_label_create(quiz_panel_);
    if (q_timer_lbl_) {
        lv_label_set_text(q_timer_lbl_, "");
        lv_obj_set_pos(q_timer_lbl_, 0, 100);
        lv_obj_set_size(q_timer_lbl_, 316, 12);
        lv_obj_set_style_text_color(q_timer_lbl_, lv_color_hex(kAccent), 0);
        lv_obj_set_style_text_align(q_timer_lbl_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(q_timer_lbl_, LV_OBJ_FLAG_HIDDEN);
    }

    // Options A/B/C/D (2 columns)
    const char* letters[] = {"A", "B", "C", "D"};
    int opt_xs[] = {6, 164, 6, 164};
    int opt_ys[] = {112, 112, 164, 164};
    for (int i = 0; i < 4; i++) {
        lv_obj_t* card = lv_obj_create(quiz_panel_);
        if (!card) continue;
        opt_cards_[i] = card;
        lv_obj_set_size(card, 150, 46);
        lv_obj_set_pos(card, opt_xs[i], opt_ys[i]);
        lv_obj_set_style_bg_color(card, lv_color_hex(kOptDef), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x90CAF9), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Badge circle
        lv_obj_t* badge = lv_obj_create(card);
        if (badge) {
            opt_badge_[i] = badge;
            lv_obj_set_size(badge, 24, 24);
            lv_obj_align(badge, LV_ALIGN_LEFT_MID, 4, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(kAccent), 0);
            lv_obj_set_style_border_width(badge, 0, 0);
            lv_obj_set_style_radius(badge, 12, 0);
            lv_obj_set_style_pad_all(badge, 0, 0);
            lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t* bl = lv_label_create(badge);
            lv_label_set_text(bl, letters[i]);
            lv_obj_center(bl);
            lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
        }

        opt_lbls_[i] = lv_label_create(card);
        lv_label_set_long_mode(opt_lbls_[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(opt_lbls_[i], 114);
        lv_obj_align(opt_lbls_[i], LV_ALIGN_LEFT_MID, 32, 0);
        lv_obj_set_style_text_color(opt_lbls_[i], lv_color_hex(kText), 0);
        lv_label_set_text(opt_lbls_[i], "");
    }

    // Feedback bar
    lv_obj_t* fb = make_hdr(quiz_panel_, kPrimary, 20);
    if (fb) {
        lv_obj_set_pos(fb, 0, 220);
        q_feedback_lbl_ = lv_label_create(fb);
        lv_label_set_text(q_feedback_lbl_, "A-D answer  " LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " cursor");
        lv_obj_align(q_feedback_lbl_, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_style_text_color(q_feedback_lbl_, lv_color_hex(0xBBDEFB), 0);
        q_feedback_bar_ = fb;
        q_nav_lbl_ = lv_label_create(fb);
        lv_label_set_text(q_nav_lbl_, LV_SYMBOL_RIGHT);
        lv_obj_align(q_nav_lbl_, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_style_text_color(q_nav_lbl_, lv_color_hex(0xFFFFFF), 0);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Build: Settings panel (Landscape: 320x240)
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::BuildSettingsPanel() {
    settings_panel_ = make_panel(screen_, kBg);
    if (!settings_panel_) { ESP_LOGE(TAG, "OOM: settings_panel_"); return; }
    settings_built_ = true;

    lv_obj_t* hdr = make_hdr(settings_panel_, kPrimary, 36);
    lv_obj_t* hl  = lv_label_create(hdr);
    lv_label_set_text(hl, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0xFFFFFF), 0);

    const char* s_names[] = {"Quiz Timer", "Sound FX", "Show Correct",
                              "Brightness", "Volume",   LV_SYMBOL_LEFT "  Back"};
    int s_xs[] = {8, 164, 8, 164, 8, 164};
    int s_ys[] = {44, 44, 94, 94, 144, 144};
    for (int i = 0; i < kSettingsCount; i++) {
        lv_obj_t* item = lv_obj_create(settings_panel_);
        if (!item) continue;
        s_items_[i] = item;
        lv_obj_set_size(item, 148, 40);
        lv_obj_set_pos(item, s_xs[i], s_ys[i]);
        lv_obj_set_style_bg_color(item, lv_color_hex(kCard), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(0x90CAF9), 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_pad_all(item, 4, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* n = lv_label_create(item);
        lv_label_set_text(n, s_names[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_style_text_color(n, lv_color_hex(kText), 0);

        if (i < kSettingsCount - 1) {
            s_vals_[i] = lv_label_create(item);
            lv_label_set_text(s_vals_[i], "-");
            lv_obj_align(s_vals_[i], LV_ALIGN_RIGHT_MID, -6, 0);
            lv_obj_set_style_text_color(s_vals_[i], lv_color_hex(kAccent), 0);
        }
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Build: Progress panel (Landscape: 320x240)
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::BuildProgressPanel() {
    progress_panel_ = make_panel(screen_, kBg);
    if (!progress_panel_) { ESP_LOGE(TAG, "OOM: progress_panel_"); return; }
    progress_built_ = true;

    lv_obj_t* hdr = make_hdr(progress_panel_, kPrimary, 36);
    lv_obj_t* hl  = lv_label_create(hdr);
    lv_label_set_text(hl, LV_SYMBOL_LIST "  Progress Report");
    lv_obj_align(hl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t* card = lv_obj_create(progress_panel_);
    if (card) {
        lv_obj_set_size(card, 304, 174);
        lv_obj_set_pos(card, 8, 40);
        lv_obj_set_style_bg_color(card, lv_color_hex(kCard), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xBBDEFB), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // 2 columns of stats
        p_answered_lbl_ = lv_label_create(card);
        lv_label_set_text(p_answered_lbl_, "Total Answered: 0 / 0");
        lv_obj_set_pos(p_answered_lbl_, 0, 4);
        lv_obj_set_style_text_color(p_answered_lbl_, lv_color_hex(kText), 0);

        p_correct_lbl_ = lv_label_create(card);
        lv_label_set_text(p_correct_lbl_, "Correct: 0");
        lv_obj_set_pos(p_correct_lbl_, 150, 4);
        lv_obj_set_style_text_color(p_correct_lbl_, lv_color_hex(kCorrect), 0);

        p_accuracy_lbl_ = lv_label_create(card);
        lv_label_set_text(p_accuracy_lbl_, "Accuracy: 0%");
        lv_obj_set_pos(p_accuracy_lbl_, 0, 34);
        lv_obj_set_style_text_color(p_accuracy_lbl_, lv_color_hex(kAccent), 0);

        p_streak_lbl_ = lv_label_create(card);
        lv_label_set_text(p_streak_lbl_, "Best Streak: 0  |  Current: 0");
        lv_obj_set_pos(p_streak_lbl_, 0, 64);
        lv_obj_set_style_text_color(p_streak_lbl_, lv_color_hex(kSubtext), 0);

        lv_obj_t* bl = lv_label_create(card); lv_label_set_text(bl, "Accuracy");
        lv_obj_set_pos(bl, 0, 100); lv_obj_set_style_text_color(bl, lv_color_hex(kSubtext), 0);

        p_bar_ = make_bar(card, 0, 120, 280, 10, 0xE3F2FD, kAccent);
    }

    lv_obj_t* back = lv_label_create(progress_panel_);
    lv_label_set_text(back, LV_SYMBOL_LEFT " Back  |  " LV_SYMBOL_RIGHT " Start Quiz");
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_color(back, lv_color_hex(kSubtext), 0);
}

// ──────────────────────────────────────────────────────────────────────────────
// Panel switching with fade animation
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::SwitchToPanel(lv_obj_t* panel) {
    lv_obj_t* all[] = {home_panel_, quiz_panel_, settings_panel_, progress_panel_};
    for (auto p : all) if (p) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    if (!panel) return;
    lv_obj_set_style_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, panel);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 200);
    lv_anim_start(&a);
}

// ──────────────────────────────────────────────────────────────────────────────
// Show / Hide
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::Show(lv_obj_t* default_screen) {
    if (!screen_) return;
    if (default_screen && default_screen != screen_) default_screen_ = default_screen;
    lv_scr_load_anim(screen_, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
    is_visible_ = true;
    if (mid_quiz_ && quiz_built_) { SwitchToPanel(quiz_panel_); mode_ = QuizMode::kQuiz; }
    else ShowHome();
}

void QuizUI::Hide() {
    StopTimer();
    if (mode_ == QuizMode::kQuiz) mid_quiz_ = true;
    if (is_visible_ && default_screen_ && default_screen_ != screen_)
        lv_scr_load_anim(default_screen_, LV_SCR_LOAD_ANIM_FADE_IN, 250, 0, false);
    is_visible_ = false;
}

void QuizUI::GoHome() {
    if (!is_visible_) return;
    StopTimer(); mid_quiz_ = false;
    ShowHome();
}

// ──────────────────────────────────────────────────────────────────────────────
// Home
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::ShowHome() {
    mode_ = QuizMode::kHome; home_sel_ = 0;
    SwitchToPanel(home_panel_);
    UpdateHomeStats();
    // Animate arc
    if (h_arc_) {
        int acc = stats_.total_answered > 0 ? (stats_.total_correct * 100 / stats_.total_answered) : 0;
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, h_arc_); lv_anim_set_exec_cb(&a, anim_arc_cb);
        lv_anim_set_values(&a, 0, acc); lv_anim_set_time(&a, 900);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
    }
    // Slide-in menu items (Landscape, start from 320 instead of 240)
    int xs[] = {10, 165, 10, 165};
    for (int i = 0; i < kMenuCount; i++) {
        if (!h_menu_[i]) continue;
        lv_obj_set_x(h_menu_[i], 320);
        lv_anim_t ma; lv_anim_init(&ma);
        lv_anim_set_var(&ma, h_menu_[i]); lv_anim_set_exec_cb(&ma, anim_x_cb);
        lv_anim_set_values(&ma, 320, xs[i]); lv_anim_set_time(&ma, 280);
        lv_anim_set_delay(&ma, (uint32_t)(i * 70));
        lv_anim_set_path_cb(&ma, lv_anim_path_ease_out); lv_anim_start(&ma);
    }
    HomeNavigate(0);
}

void QuizUI::UpdateHomeStats() {
    if (!h_stats_lbl_ || !h_arc_lbl_ || !h_stat_bar_) return;
    int acc = stats_.total_answered > 0 ? (stats_.total_correct * 100 / stats_.total_answered) : 0;
    char buf[52];
    snprintf(buf, sizeof(buf), "Answered: %d/%d   Accuracy: %d%%",
             stats_.total_answered, (int)questions_.size(), acc);
    lv_label_set_text(h_stats_lbl_, buf);
    char ab[8]; snprintf(ab, sizeof(ab), "%d%%", acc);
    lv_label_set_text(h_arc_lbl_, ab);
    lv_bar_set_value(h_stat_bar_,
        questions_.empty() ? 0 : (stats_.total_answered * 100 / (int)questions_.size()),
        LV_ANIM_ON);
}

void QuizUI::HomeNavigate(int delta) {
    home_sel_ = (home_sel_ + delta + kMenuCount) % kMenuCount;
    for (int i = 0; i < kMenuCount; i++) {
        if (!h_menu_[i] || !h_menu_lbl_[i]) continue;
        if (i == home_sel_) {
            lv_obj_set_style_bg_color(h_menu_[i], lv_color_hex(kPrimary), 0);
            lv_obj_set_style_border_width(h_menu_[i], 2, 0);
            lv_obj_set_style_text_color(h_menu_lbl_[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_bg_color(h_menu_[i], lv_color_hex(kCard), 0);
            lv_obj_set_style_border_width(h_menu_[i], 1, 0);
            lv_obj_set_style_text_color(h_menu_lbl_[i], lv_color_hex(kText), 0);
        }
    }
}

void QuizUI::HomeSelect() {
    switch (home_sel_) {
        case 0: EnterQuiz();         break;
        case 1: ShowProgress();      break;
        case 2: OpenSettings(false); break;
        case 3: Application::GetInstance().ToggleChatState(); break;
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Quiz
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::EnterQuiz() {
    if (!quiz_built_) BuildQuizPanel();
    mode_ = QuizMode::kQuiz; mid_quiz_ = true; joy_cursor_ = -1;
    SwitchToPanel(quiz_panel_);
    DisplayCurrentQuestion();
}

void QuizUI::DisplayCurrentQuestion() {
    if (!question_lbl_) return;
    if (questions_.empty()) {
        lv_label_set_text(question_lbl_, "No questions loaded.");
        for (int i = 0; i < 4; i++) if (opt_lbls_[i]) lv_label_set_text(opt_lbls_[i], "-");
        return;
    }
    if (q_idx_ >= questions_.size()) q_idx_ = questions_.size() - 1;
    const QuizQuestion& q = questions_[q_idx_];
    lv_label_set_text(question_lbl_, q.q.c_str());
    for (int i = 0; i < 4; i++) {
        if (!opt_lbls_[i]) continue;
        lv_label_set_text(opt_lbls_[i], i < (int)q.opts.size() ? q.opts[i].c_str() : "-");
    }
    ResetAllOpts();
    SetFeedback("A-D answer   " LV_SYMBOL_UP "/" LV_SYMBOL_DOWN " cursor", kPrimary);
    ans_revealed_ = false; selected_ans_ = -1; joy_cursor_ = -1;
    UpdateQuizProgress();
    if (settings_.timer_seconds > 0) {
        if (q_timer_bar_) lv_obj_clear_flag(q_timer_bar_, LV_OBJ_FLAG_HIDDEN);
        if (q_timer_lbl_) lv_obj_clear_flag(q_timer_lbl_, LV_OBJ_FLAG_HIDDEN);
        StartTimer();
    } else {
        if (q_timer_bar_) lv_obj_add_flag(q_timer_bar_, LV_OBJ_FLAG_HIDDEN);
        if (q_timer_lbl_) lv_obj_add_flag(q_timer_lbl_, LV_OBJ_FLAG_HIDDEN);
    }
}

void QuizUI::UpdateQuizProgress() {
    if (!q_progress_lbl_ || !q_prog_bar_ || questions_.empty()) return;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d/%d", (int)(q_idx_ + 1), (int)questions_.size());
    lv_label_set_text(q_progress_lbl_, buf);
    lv_bar_set_value(q_prog_bar_, (int)((q_idx_ + 1) * 100 / questions_.size()), LV_ANIM_ON);
    if (q_score_lbl_) {
        char s[24]; snprintf(s, sizeof(s), "Score: %d", stats_.session_score);
        lv_label_set_text(q_score_lbl_, s);
    }
}

void QuizUI::ResetAllOpts() {
    for (int i = 0; i < 4; i++)
        HighlightOpt(i, kOptDef, 0x90CAF9, kAccent, kText);
}

void QuizUI::HighlightOpt(int i, uint32_t card_bg, uint32_t border, uint32_t badge, uint32_t tcol) {
    if (i < 0 || i >= 4) return;
    if (opt_cards_[i]) {
        lv_obj_set_style_bg_color(opt_cards_[i], lv_color_hex(card_bg), 0);
        lv_obj_set_style_border_color(opt_cards_[i], lv_color_hex(border), 0);
    }
    if (opt_badge_[i]) lv_obj_set_style_bg_color(opt_badge_[i], lv_color_hex(badge), 0);
    if (opt_lbls_[i])  lv_obj_set_style_text_color(opt_lbls_[i], lv_color_hex(tcol), 0);
}

void QuizUI::FlashOpt(int i) {
    if (i < 0 || i >= 4 || !opt_cards_[i]) return;
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, opt_cards_[i]); lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_time(&a, 100); lv_anim_set_playback_time(&a, 100);
    lv_anim_set_repeat_count(&a, 2); lv_anim_start(&a);
}

void QuizUI::MoveCursor(int delta) {
    if (!ans_revealed_) ResetAllOpts();
    joy_cursor_ = (joy_cursor_ + delta + 4) % 4;
    if (!ans_revealed_) HighlightOpt(joy_cursor_, kOptCur, kAccent, kPrimary, kPrimary);
}

void QuizUI::ConfirmCursorSelection() {
    if (joy_cursor_ >= 0 && !ans_revealed_) SelectAnswer(joy_cursor_);
    else if (ans_revealed_) NextQuestion();
}

void QuizUI::SelectAnswer(int idx) {
    if (!is_visible_ || mode_ != QuizMode::kQuiz || ans_revealed_) return;
    if (q_idx_ >= questions_.size()) return;
    StopTimer(); ans_revealed_ = true; selected_ans_ = idx;
    FlashOpt(idx);
    const QuizQuestion& q = questions_[q_idx_];
    bool correct = (idx == q.ans);
    if (correct) {
        stats_.session_score++; stats_.total_correct++;
        stats_.current_streak++;
        stats_.best_streak = std::max(stats_.best_streak, stats_.current_streak);
        HighlightOpt(idx, kCorrectBg, kCorrect, kCorrect, kCorrect);
        char fb[40]; snprintf(fb, sizeof(fb), LV_SYMBOL_OK "  Correct!  Streak: %d", stats_.current_streak);
        SetFeedback(fb, kCorrect);
    } else {
        stats_.current_streak = 0;
        HighlightOpt(idx, kWrongBg, kWrong, kWrong, kWrong);
        if (settings_.show_correct) HighlightOpt(q.ans, kCorrectBg, kCorrect, kCorrect, kCorrect);
        char fb[40]; snprintf(fb, sizeof(fb), LV_SYMBOL_CLOSE "  Wrong!  Correct: %c", 'A' + q.ans);
        SetFeedback(fb, kWrong);
    }
    stats_.total_answered++; stats_.session_total++;
    UpdateQuizProgress();
}

void QuizUI::SetFeedback(const char* msg, uint32_t bg) {
    if (!q_feedback_lbl_ || !q_feedback_bar_) return;
    lv_label_set_text(q_feedback_lbl_, msg);
    lv_obj_set_style_bg_color(q_feedback_bar_, lv_color_hex(bg), 0);
}

void QuizUI::SetFeedback(const std::string& msg, uint32_t bg) { SetFeedback(msg.c_str(), bg); }

void QuizUI::NextQuestion() {
    if (q_idx_ + 1 < questions_.size()) { q_idx_++; DisplayCurrentQuestion(); }
    else ShowResult();
}

void QuizUI::PrevQuestion() {
    if (q_idx_ > 0) { q_idx_--; DisplayCurrentQuestion(); }
}

// ──────────────────────────────────────────────────────────────────────────────
// Timer
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::StartTimer() {
    if (!q_timer_ || settings_.timer_seconds <= 0) return;
    StopTimer(); timer_rem_ = settings_.timer_seconds;
    UpdateTimerUI();
    esp_timer_start_periodic(q_timer_, 1000000ULL);
}
void QuizUI::StopTimer() { if (q_timer_) esp_timer_stop(q_timer_); }

void QuizUI::UpdateTimerUI() {
    if (!q_timer_bar_ || !q_timer_lbl_) return;
    int pct = (timer_rem_ * 100) / std::max(1, settings_.timer_seconds);
    lv_bar_set_value(q_timer_bar_, pct, LV_ANIM_ON);
    uint32_t color = timer_rem_ > settings_.timer_seconds / 2 ? kAccent :
                     timer_rem_ > 5 ? kWarn : kWrong;
    lv_obj_set_style_bg_color(q_timer_bar_, lv_color_hex(color), LV_PART_INDICATOR);
    char tbuf[8]; snprintf(tbuf, sizeof(tbuf), "%ds", timer_rem_);
    lv_label_set_text(q_timer_lbl_, tbuf);
}

void QuizUI::OnTimerTick() {
    if (!is_visible_ || mode_ != QuizMode::kQuiz) { StopTimer(); return; }
    timer_rem_--;
    if (lvgl_port_lock(-1)) {
        UpdateTimerUI();
        if (timer_rem_ <= 0) {
            StopTimer();
            if (!ans_revealed_) {
                ans_revealed_ = true;
                if (settings_.show_correct && q_idx_ < questions_.size())
                    HighlightOpt(questions_[q_idx_].ans, kCorrectBg, kCorrect, kCorrect, kCorrect);
                SetFeedback(LV_SYMBOL_CLOSE "  Time's Up!", kWrong);
                stats_.total_answered++; stats_.session_total++;
                stats_.current_streak = 0; UpdateQuizProgress();
            }
        }
        lvgl_port_unlock();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Settings
// ──────────────────────────────────────────────────────────────────────────────

const char* QuizUI::TimerLabel() const {
    switch (settings_.timer_seconds) {
        case 10: return "10s"; case 20: return "20s"; case 30: return "30s";
        default: return "Off";
    }
}

void QuizUI::ApplySettingsAction(int delta) {
    switch (s_sel_) {
        case 0: {
            static const int vals[] = {0, 10, 20, 30};
            static int tidx = 0;
            tidx = (tidx + (delta > 0 ? 1 : 3)) % 4;
            settings_.timer_seconds = vals[tidx]; break;
        }
        case 1: settings_.sound_enabled  = !settings_.sound_enabled;  break;
        case 2: settings_.show_correct   = !settings_.show_correct;   break;
        case 3: settings_.brightness_pct = std::max(10, std::min(100, settings_.brightness_pct + delta * 10)); break;
        case 4: settings_.volume_pct     = std::max(0,  std::min(100, settings_.volume_pct     + delta * 10)); break;
        default: break;
    }
    RenderSettings();
}

void QuizUI::RenderSettings() {
    const char* oo[] = {"Off", "On"};
    if (s_vals_[0]) lv_label_set_text(s_vals_[0], TimerLabel());
    if (s_vals_[1]) lv_label_set_text(s_vals_[1], oo[settings_.sound_enabled]);
    if (s_vals_[2]) lv_label_set_text(s_vals_[2], oo[settings_.show_correct]);
    char b[8];
    if (s_vals_[3]) { snprintf(b, sizeof(b), "%d%%", settings_.brightness_pct); lv_label_set_text(s_vals_[3], b); }
    if (s_vals_[4]) { snprintf(b, sizeof(b), "%d%%", settings_.volume_pct);     lv_label_set_text(s_vals_[4], b); }
    for (int i = 0; i < kSettingsCount; i++) {
        if (!s_items_[i]) continue;
        if (i == s_sel_) {
            lv_obj_set_style_bg_color(s_items_[i], lv_color_hex(kPrimary), 0);
            lv_obj_set_style_border_width(s_items_[i], 2, 0);
        } else {
            lv_obj_set_style_bg_color(s_items_[i], lv_color_hex(kCard), 0);
            lv_obj_set_style_border_width(s_items_[i], 1, 0);
        }
    }
}

void QuizUI::OpenSettings(bool from_quiz) {
    if (!settings_built_) BuildSettingsPanel();
    s_from_quiz_ = from_quiz; mode_ = QuizMode::kSettings; s_sel_ = 0;
    SwitchToPanel(settings_panel_); RenderSettings();
}

void QuizUI::CloseSettings() {
    if (s_from_quiz_) EnterQuiz(); else ShowHome();
}

// ──────────────────────────────────────────────────────────────────────────────
// Progress
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::ShowProgress() {
    if (!progress_built_) BuildProgressPanel();
    mode_ = QuizMode::kProgress;
    SwitchToPanel(progress_panel_);
    UpdateProgressUI();
}

void QuizUI::UpdateProgressUI() {
    int acc = stats_.total_answered > 0 ? (stats_.total_correct * 100 / stats_.total_answered) : 0;
    char buf[48];
    if (p_answered_lbl_) {
        snprintf(buf, sizeof(buf), "Total Answered: %d / %d", stats_.total_answered, (int)questions_.size());
        lv_label_set_text(p_answered_lbl_, buf);
    }
    if (p_correct_lbl_) { snprintf(buf, sizeof(buf), "Correct: %d", stats_.total_correct); lv_label_set_text(p_correct_lbl_, buf); }
    if (p_accuracy_lbl_) { snprintf(buf, sizeof(buf), "Accuracy: %d%%", acc); lv_label_set_text(p_accuracy_lbl_, buf); }
    if (p_streak_lbl_) { snprintf(buf, sizeof(buf), "Best: %d  |  Current: %d", stats_.best_streak, stats_.current_streak); lv_label_set_text(p_streak_lbl_, buf); }
    if (p_bar_) {
        lv_anim_t a; lv_anim_init(&a);
        lv_anim_set_var(&a, p_bar_); lv_anim_set_exec_cb(&a, anim_bar_cb);
        lv_anim_set_values(&a, 0, acc); lv_anim_set_time(&a, 800);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out); lv_anim_start(&a);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Result
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::ShowResult() {
    StopTimer(); mode_ = QuizMode::kResult; mid_quiz_ = false;
    // Overlay on top of quiz_panel_
    lv_obj_t* all[] = {home_panel_, settings_panel_, progress_panel_};
    for (auto p : all) if (p) lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    if (quiz_panel_) lv_obj_clear_flag(quiz_panel_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ov = lv_obj_create(quiz_panel_);
    if (!ov) return;
    lv_obj_set_size(ov, 320, 240); lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0); lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);

    int acc = stats_.session_total > 0 ? (stats_.session_score * 100 / stats_.session_total) : 0;
    auto ml = [&](const char* t, uint32_t c, lv_align_t a, int xo, int yo) {
        lv_obj_t* l = lv_label_create(ov);
        lv_label_set_text(l, t); lv_obj_align(l, a, xo, yo);
        lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    };
    ml(LV_SYMBOL_OK, kCorrect, LV_ALIGN_TOP_MID, 0, 30);
    ml("Quiz Complete!", kPrimary, LV_ALIGN_TOP_MID, 0, 60);
    char buf[40];
    snprintf(buf, sizeof(buf), "Score: %d / %d", stats_.session_score, stats_.session_total);
    ml(buf, kText, LV_ALIGN_TOP_MID, 0, 95);
    snprintf(buf, sizeof(buf), "Accuracy: %d%%", acc);
    ml(buf, kAccent, LV_ALIGN_TOP_MID, 0, 125);
    snprintf(buf, sizeof(buf), "Best Streak: %d", stats_.best_streak);
    ml(buf, kSubtext, LV_ALIGN_TOP_MID, 0, 153);
    ml(LV_SYMBOL_RIGHT " restart   " LV_SYMBOL_LEFT " home", kSubtext, LV_ALIGN_BOTTOM_MID, 0, -10);

    stats_.session_score = 0; stats_.session_total = 0;
}

// ──────────────────────────────────────────────────────────────────────────────
// TTP Button handlers
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::HandleButtonA() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(0); }
void QuizUI::HandleButtonB() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(1); }
void QuizUI::HandleButtonC() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(2); }
void QuizUI::HandleButtonD() { if (is_visible_ && mode_ == QuizMode::kQuiz) SelectAnswer(3); }

// ──────────────────────────────────────────────────────────────────────────────
// Joystick handlers
// ──────────────────────────────────────────────────────────────────────────────

void QuizUI::HandleJoyUp() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeNavigate(-1);  break;
        case QuizMode::kQuiz:     MoveCursor(-1);    break;
        case QuizMode::kSettings: s_sel_ = (s_sel_ + kSettingsCount - 1) % kSettingsCount; RenderSettings(); break;
        default: break;
    }
}

void QuizUI::HandleJoyDown() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeNavigate(1);   break;
        case QuizMode::kQuiz:     MoveCursor(1);     break;
        case QuizMode::kSettings: s_sel_ = (s_sel_ + 1) % kSettingsCount; RenderSettings(); break;
        default: break;
    }
}

void QuizUI::HandleJoyLeft() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kQuiz:     if (ans_revealed_) PrevQuestion(); break;
        case QuizMode::kSettings: ApplySettingsAction(-1); break;
        case QuizMode::kProgress: ShowHome(); break;
        case QuizMode::kResult:   ShowHome(); break;
        default: break;
    }
}

void QuizUI::HandleJoyRight() {
    if (!is_visible_) return;
    switch (mode_) {
        case QuizMode::kHome:     HomeSelect(); break;
        case QuizMode::kQuiz:     if (ans_revealed_) NextQuestion(); break;
        case QuizMode::kSettings: ApplySettingsAction(1); break;
        case QuizMode::kProgress: EnterQuiz(); break;
        case QuizMode::kResult:
            q_idx_ = 0; SwitchToPanel(quiz_panel_);
            DisplayCurrentQuestion(); mode_ = QuizMode::kQuiz; mid_quiz_ = true;
            break;
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

void QuizUI::HandleJoyPressLong() {
    if (!is_visible_ || mode_ != QuizMode::kQuiz) return;
    OpenSettings(true);
}

void QuizUI::HandleAgentLongPress() {
    if (is_visible_) GoHome();
}
