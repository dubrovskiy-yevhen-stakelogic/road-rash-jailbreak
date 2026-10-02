#pragma once
// The frame profiler's numbers (the pattern of the gt2-play project, MIT): application frames
// a second, the mean frame interval, the 1 % low and the worst interval, over half-second windows.
#include <algorithm>
#include <array>
#include <cstddef>

namespace rrgame {

class FrameProfiler {
public:
    void Reset() { *this = {}; }
    // `now` in seconds, once per presented frame.
    void Record(double now) {
        if (!started_) {
            last_ = now;
            started_ = true;
            return;
        }
        const double interval = now - last_;
        last_ = now;
        if (interval <= 0 || interval > 1.0) { // a stall (a load, the window moved): start again
            Reset();
            last_ = now;
            started_ = true;
            return;
        }
        recent_[recentNext_] = interval;
        recentNext_ = (recentNext_ + 1) % recent_.size();
        recentCount_ = std::min(recentCount_ + 1, recent_.size());
        elapsed_ += interval;
        ++intervals_;
        peak_ = std::max(peak_, interval);
        if (elapsed_ >= 0.5) {
            fps = static_cast<double>(intervals_) / elapsed_;
            frameMs = elapsed_ * 1000.0 / static_cast<double>(intervals_);
            maxMs = peak_ * 1000.0;
            auto sorted = recent_;
            std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(recentCount_));
            const size_t tail = std::max<size_t>(1, (recentCount_ + 99) / 100);
            double slowSum = 0;
            for (size_t i = recentCount_ - tail; i < recentCount_; ++i) slowSum += sorted[i];
            lowFps = static_cast<double>(tail) / slowSum;
            elapsed_ = peak_ = 0;
            intervals_ = 0;
        }
    }
    double fps = 0, frameMs = 0, maxMs = 0, lowFps = 0;

private:
    std::array<double, 256> recent_{};
    size_t recentNext_ = 0, recentCount_ = 0;
    bool started_ = false;
    double last_ = 0, elapsed_ = 0, peak_ = 0;
    unsigned intervals_ = 0;
};

} // namespace rrgame
