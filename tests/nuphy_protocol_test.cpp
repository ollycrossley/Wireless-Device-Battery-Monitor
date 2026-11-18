#include "../src/drivers/nuphy_protocol.h"

#include <cassert>

namespace {

int Consume(nuphy::BatteryFrame& frame, int usage, std::uint64_t nowMs, std::uint8_t reportId = 3) {
    const std::uint8_t bytes[] = {reportId, std::uint8_t(usage), std::uint8_t(usage >> 8)};
    return frame.Consume(bytes, sizeof(bytes), nowMs);
}

}  // namespace

int main() {
    nuphy::BatteryFrame frame;
    for (int percent = 0; percent <= 100; ++percent) {
        assert(Consume(frame, 0x07, 0) == -1);
        assert(Consume(frame, 0x08 + (percent >> 4), 50) == -1);
        assert(Consume(frame, 0x10 + (percent & 0xf), 100) == -1);
        assert(Consume(frame, 0, 150) == percent);
    }

    // An out-of-range level and a missing start marker cannot publish a sample.
    assert(Consume(frame, 0x07, 0) == -1);
    assert(Consume(frame, 0x0e, 50) == -1);
    assert(Consume(frame, 0x15, 100) == -1);
    assert(Consume(frame, 0, 150) == -1);
    assert(Consume(frame, 0x0c, 200) == -1);
    assert(Consume(frame, 0x19, 250) == -1);
    assert(Consume(frame, 0, 300) == -1);

    // A media key aborts a frame. A later start marker resynchronizes the decoder.
    assert(Consume(frame, 0x07, 1000) == -1);
    assert(Consume(frame, 0x0c, 1050) == -1);
    assert(Consume(frame, 0xe9, 1075) == -1);
    assert(Consume(frame, 0x19, 1100) == -1);
    assert(Consume(frame, 0, 1150) == -1);
    assert(Consume(frame, 0x07, 1200) == -1);
    assert(Consume(frame, 0x07, 1250) == -1);
    assert(Consume(frame, 0x0c, 1300) == -1);
    assert(Consume(frame, 0x19, 1325, 4) == -1);
    assert(Consume(frame, 0x19, 1350) == -1);
    assert(Consume(frame, 0, 1400) == 73);

    // Incomplete and expired sequences cannot carry over into the next burst.
    assert(Consume(frame, 0x07, 2000) == -1);
    assert(Consume(frame, 0x0c, 2050) == -1);
    assert(Consume(frame, 0, 2100) == -1);
    assert(Consume(frame, 0x07, 2500) == -1);
    assert(Consume(frame, 0x0c, 2550) == -1);
    assert(Consume(frame, 0x19, 2600) == -1);
    assert(Consume(frame, 0, 3001) == -1);
}
