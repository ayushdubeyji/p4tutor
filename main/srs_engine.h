#pragma once
#include <cstdint>
#include <vector>
#include <ctime>

#define TAG_SRS "SrsEngine"

struct SrsCard {
    uint32_t card_id;         // Matches quiz question index
    uint32_t due_timestamp;   // Unix time when next review is due  
    float    ease_factor;     // SM-2 EF, initial=2.5, min=1.3
    uint16_t interval_days;   // Days until next review
    uint8_t  repetition_n;    // Consecutive successful recalls
    uint8_t  pad;             // Alignment padding (14 bytes total)
};

enum class SrsGrade : uint8_t {
    kBlackout = 0,   // Complete failure
    kBadRecall = 1,  // Wrong but remembered seeing it
    kHard = 2,       // Wrong but knew correct on seeing
    kGood = 3,       // Correct with difficulty  
    kEasy = 4,       // Correct with hesitation
    kPerfect = 5     // Perfect recall
};

class SrsEngine {
public:
    static SrsEngine& GetInstance();
    
    // Apply SM-2 algorithm to a card after a review
    SrsCard Review(SrsCard card, SrsGrade grade);
    
    // Convert quiz boolean result to SRS grade  
    static SrsGrade BoolToGrade(bool correct, int time_taken_ms, int timer_limit_ms);
    
    // Initialize a brand new card
    static SrsCard NewCard(uint32_t card_id);
    
private:
    SrsEngine() = default;
};
