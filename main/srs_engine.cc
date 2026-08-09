#include "srs_engine.h"
#include <esp_log.h>
#include <cmath>
#include <algorithm>

SrsEngine& SrsEngine::GetInstance() {
    static SrsEngine instance;
    return instance;
}

SrsCard SrsEngine::Review(SrsCard card, SrsGrade grade) {
    uint8_t q = static_cast<uint8_t>(grade);
    
    if (grade >= SrsGrade::kGood) {
        if (card.repetition_n == 0) {
            card.interval_days = 1;
        } else if (card.repetition_n == 1) {
            card.interval_days = 6;
        } else {
            card.interval_days = std::round(card.interval_days * card.ease_factor);
        }
        card.repetition_n++;
    } else {
        card.repetition_n = 0;
        card.interval_days = 1;
    }
    
    card.ease_factor = card.ease_factor + (0.1f - (5 - q) * (0.08f + (5 - q) * 0.02f));
    if (card.ease_factor < 1.3f) {
        card.ease_factor = 1.3f;
    }
    
    card.due_timestamp = std::time(nullptr) + card.interval_days * 86400;
    
    ESP_LOGD(TAG_SRS, "Review card_id=%lu grade=%u n=%u I=%u EF=%.2f due=%lu", 
             (unsigned long)card.card_id, q, card.repetition_n, card.interval_days, 
             card.ease_factor, (unsigned long)card.due_timestamp);
             
    return card;
}

SrsGrade SrsEngine::BoolToGrade(bool correct, int time_taken_ms, int timer_limit_ms) {
    bool fast = time_taken_ms <= (timer_limit_ms / 2);
    if (correct) {
        return fast ? SrsGrade::kPerfect : SrsGrade::kGood;
    } else {
        return fast ? SrsGrade::kHard : SrsGrade::kBlackout;
    }
}

SrsCard SrsEngine::NewCard(uint32_t card_id) {
    SrsCard card;
    card.card_id = card_id;
    card.due_timestamp = std::time(nullptr);
    card.ease_factor = 2.5f;
    card.interval_days = 0;
    card.repetition_n = 0;
    card.pad = 0;
    return card;
}
