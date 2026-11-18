#pragma once

#include <cstddef>
#include <cstdint>

namespace nuphy {

// Custom Halo96 V2 firmware sends a start marker, two digits and a release
// in successive Consumer Control reports. See docs/nuphy-halo96-v2.md.
class BatteryFrame {
public:
    int Consume(const std::uint8_t* report, std::size_t size, std::uint64_t nowMs) {
        if (size < 3 || report[0] != 0x03) return -1;
        const int usage = report[1] | (report[2] << 8);
        if (usage == 0x07) {
            next_ = Next::High;
            startedMs_ = nowMs;
        } else if (nowMs - startedMs_ > 500) {
            next_ = Next::Start;
        } else if (next_ == Next::High && usage >= 0x08 && usage <= 0x0e) {
            percent_ = (usage - 0x08) << 4;
            next_ = Next::Low;
        } else if (next_ == Next::Low && usage >= 0x10 && usage <= 0x1f) {
            percent_ |= usage - 0x10;
            next_ = Next::Release;
        } else {
            const int result = next_ == Next::Release && usage == 0 && percent_ <= 100 ? percent_ : -1;
            next_ = Next::Start;
            return result;
        }
        return -1;
    }

private:
    enum class Next { Start, High, Low, Release };
    Next next_ = Next::Start;
    std::uint64_t startedMs_ = 0;
    int percent_ = 0;
};

}  // namespace nuphy
