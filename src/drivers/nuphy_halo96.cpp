// NuPhy Halo96 V2 with custom battery firmware. Protocol and hardware evidence
// are in docs/nuphy-halo96-v2.md. The stock receiver forwards the reports.
#include "drivers.h"
#include "nuphy_protocol.h"

#include <map>
#include <memory>

namespace {

constexpr USHORT kVendor = 0x19f5, kProduct = 0x3247;
constexpr DWORD kReadTimeoutMs = 250;
constexpr ULONGLONG kSampleLifetimeMs = 35000;  // Firmware sends a frame about every 15 seconds.

class Receiver {
public:
    explicit Receiver(const HidColl& collection) : collection_(collection) {
        if (stop_) thread_ = CreateThread(nullptr, 0, Listen, this, 0, nullptr);
    }

    ~Receiver() {
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
        }
        if (stop_) CloseHandle(stop_);
    }

    bool Running() const { return thread_ != nullptr; }

    int Percent() {
        AcquireSRWLockShared(&lock_);
        const int percent = GetTickCount64() - receivedMs_ <= kSampleLifetimeMs ? percent_ : -1;
        ReleaseSRWLockShared(&lock_);
        return percent;
    }

private:
    static DWORD WINAPI Listen(LPVOID arg) {
        static_cast<Receiver*>(arg)->ReadReports();
        return 0;
    }

    void UpdateSample(int percent, ULONGLONG now) {
        AcquireSRWLockExclusive(&lock_);
        percent_ = percent;
        receivedMs_ = now;
        ReleaseSRWLockExclusive(&lock_);
    }

    void ReadReports() {
        HidDev device;
        nuphy::BatteryFrame frame;
        std::vector<BYTE> report;
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            if (!device.ok() && !device.OpenReadOnly(collection_)) {
                if (WaitForSingleObject(stop_, kReadTimeoutMs) != WAIT_TIMEOUT) break;
                continue;
            }

            const ULONGLONG started = GetTickCount64();
            const int count = device.Read(report, kReadTimeoutMs);
            const ULONGLONG now = GetTickCount64();
            if (count < 0) {
                // A rapid failure means the handle may have been invalidated by a USB reconnect.
                // An ordinary read timeout leaves the stream open.
                if (now - started < kReadTimeoutMs / 2) {
                    device.Close();
                    frame = {};
                    UpdateSample(-1, now);
                    if (WaitForSingleObject(stop_, kReadTimeoutMs) != WAIT_TIMEOUT) break;
                }
                continue;
            }
            const int percent = frame.Consume(report.data(), report.size(), now);
            if (percent >= 0) UpdateSample(percent, now);
        }
    }

    HidColl collection_;
    HANDLE stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread_ = nullptr;
    SRWLOCK lock_ = SRWLOCK_INIT;
    int percent_ = -1;
    ULONGLONG receivedMs_ = 0;
};

}  // namespace

void PollNuphyHalo96(PollContext& ctx) {
    // The receiving thread preserves the timing of the burst without making the
    // normal poll wait for the next 15-second broadcast, including on stock firmware.
    static std::map<std::wstring, std::unique_ptr<Receiver>> receivers;
    std::set<std::wstring> present;
    for (const auto* c : ctx.Find(kVendor, kProduct, [](const HidColl& c) {
             return c.iface == 1 && c.page() == 0x000c && c.usage() == 0x0001 && c.inLen() == 3;
         })) {
        auto it = receivers.find(c->path);
        if (it == receivers.end()) {
            // Other NuPhy models share this receiver VID/PID.
            if (HidProduct(*c) != L"NuPhy Halo96 V2 Dongle") continue;
            auto receiver = std::make_unique<Receiver>(*c);
            if (!receiver->Running()) continue;
            it = receivers.emplace(c->path, std::move(receiver)).first;
        }
        present.insert(c->path);

        const int percent = it->second->Percent();
        if (percent < 0) continue;
        DeviceStatus status;
        status.key = L"nuphy:" + c->path;
        status.name = L"NuPhy Halo96 V2";
        status.via = L"2.4 GHz receiver";
        status.kind = DeviceKind::Keyboard;
        status.percent = percent;
        ctx.Add(status, kVendor, kProduct);
    }
    for (auto it = receivers.begin(); it != receivers.end();) {
        if (present.count(it->first)) ++it;
        else it = receivers.erase(it);
    }
}
