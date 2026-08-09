#pragma once

#include <cstdint>
#include <functional>

class PomodoroTimer {
public:
    enum class State {
        kIdle,
        kWork,
        kBreak
    };

    static PomodoroTimer& GetInstance() {
        static PomodoroTimer instance;
        return instance;
    }

    void Start();
    void Stop();
    
    State GetState() const { return state_; }
    int GetRemainingMinutes() const { return remaining_min_; }
    
    void TickMinute(); // Called every minute

private:
    PomodoroTimer() = default;
    ~PomodoroTimer() = default;

    State state_ = State::kIdle;
    int remaining_min_ = 0;
};
