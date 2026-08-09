#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include "lvgl.h"

// Same as SrsCard but for storage
struct SrsRecord {
    uint32_t card_id;
    uint32_t due_timestamp;
    float ease_factor;
    uint16_t interval_days;
    uint8_t repetition_n;
    uint16_t total_reviews;
    uint16_t total_correct;
} __attribute__((packed));

struct SrsSessionStats {
    uint32_t total_reviews = 0;
    uint32_t total_correct = 0;
    uint32_t current_streak = 0;
    uint32_t best_streak = 0;
    uint32_t cards_due_today = 0;
    uint32_t cards_mastered = 0; // interval >= 21
    uint32_t cards_learning = 0; // interval < 7
};

class SrsDatabase {
public:
    static SrsDatabase& GetInstance() {
        static SrsDatabase instance;
        return instance;
    }

    void Initialize(const char* mount_point);
    SrsRecord GetCard(uint32_t card_id);
    void UpdateCard(const SrsRecord& rec);
    std::vector<uint32_t> GetDueCardIds();
    SrsSessionStats GetStats();
    std::vector<int> GetDailyReviewCounts(int days);
    int GetDueCount();
    void SaveSession(uint32_t card_id, bool correct);
    
    void ShowAnalyticsUI(lv_obj_t* parent);

private:
    SrsDatabase() = default;
    ~SrsDatabase() = default;
    SrsDatabase(const SrsDatabase&) = delete;
    SrsDatabase& operator=(const SrsDatabase&) = delete;

    std::string mount_point_;
    std::string bin_path_;
    std::string json_path_;
    std::vector<SrsRecord> records_;

    void LoadDatabase();
    void SaveDatabase();
    void LoadDailyCounts();
    void SaveDailyCounts(const std::vector<int>& counts);
};
