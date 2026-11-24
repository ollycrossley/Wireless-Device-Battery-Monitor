// BatteryHub - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>
#include <bthdef.h>
#include <bluetoothapis.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "device_aliases.h"
#include "drivers/hidpp_events.h"
#include "hid_listener.h"
#include "alert_rules.h"
#include "devices.h"
#include "battery_history.h"
#include "battery_intelligence.h"
#include "dashboard.h"
#include "charge_estimate.h"
#include "i18n.h"
#include "json_output.h"
#include "labels.h"
#include "reading_freshness.h"
#include "status_json.h"
#include "refresh_policy.h"
#include "memory_trim.h"
#include "report.h"
#include "rename_dialog.h"
#include "offset_dialog.h"
#include "widget.h"
#include "update_win.h"
#include "update_apply.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_DEVICE_EVENT = WM_APP + 4;  // lParam: ms to wait before polling (PR #535)
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT_PTR TIMER_ALERT_ICON = 3;
constexpr UINT_PTR TIMER_RESUME = 4;
constexpr GUID kConsoleDisplayStateGuid = {
    0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 }
};
// "Check battery every" choices in seconds; 60 is the default. A poll resets an awake device's sleep timer,
// so a device only sleeps if its timer is shorter than the interval.
constexpr DWORD POLL_SECONDS[] = { 30, 60, 120, 300, 600, 900, 1800, 3600 };
constexpr int LOW_BATTERY = 15;
constexpr UINT PLACEHOLDER_ID = 1;
constexpr UINT ALERT_ICON_ID = 2;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT, ID_REPORT,
    ID_BRAND_COLORS, ID_STACKED, ID_SHOW_DISCONNECTED, ID_RECORD_HISTORY, ID_DIM_SLEEPING,
    ID_TRAY_WHEN_EMPTY, ID_MAKE_ROOM, ID_FAR_LEFT, ID_PERCENT_SIGN, ID_RING_STYLE, ID_BEFORE_APPS, ID_CONNECTION_ICONS, ID_KEEP_MOUSE_LAST_KNOWN, ID_SHORT_RUNTIME_ALERT, ID_HOVER_CARD, ID_HOVER_CHART, ID_TRAY_DEVICE_ICONS, ID_ANIMATE_CHARGING, ID_TRACK_SINCE, ID_TRACK_AWAKE, ID_TRACK_EMPTY, ID_RUNTIME_WIDGET, ID_MODE_MYFINDER, ID_CHARGED_ALERT, ID_TIERED_ALERTS, ID_DASHBOARD,
    ID_PLACE_AUTO, ID_PLACE_TOP, ID_PLACE_TRAY, ID_PLACE_BOTTOM,  // "Position on a vertical taskbar"
    ID_POSITION_FIRST = 700,  // "Position on the taskbar": ID_POSITION_FIRST + 0..5, then the custom value
    ID_POLL_FIRST = 500,  // "Check battery every": ID_POLL_FIRST + 0..7
    ID_LIMIT_OFF = 600, ID_LIMIT_FIRST,  // "Notify when charged to": off, then alerts::kChargeLimits
    ID_UPDATE = 200, ID_AUTO_UPDATE,
    ID_LANG_AUTO = 400, ID_LANG_ADD, ID_LANG_FIRST,  // "Language": ID_LANG_FIRST + 0..63
    ID_MONITOR_ALL = 300, ID_MONITOR_MAIN, ID_MONITOR_FIRST,  // "Show the widget on": ID_MONITOR_FIRST + 0..31
    ID_DEVICE_FIRST = 1000
};
enum DeviceCmd : UINT {
    DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_RENAME, DEV_RESET_NAME, DEV_HISTORY,
    DEV_ALERT_FIRST = 6, DEV_ALERT_DEFAULT = DEV_ALERT_FIRST + 8, DEV_ALERT_OFF,
    DEV_DASHBOARD = 16, DEV_STRIDE = 32
};

const wchar_t* kAppName = L"BatteryHub";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
// Push-event emitters (PR #535): a Logitech mouse switched on behind a receiver that stays plugged in
// produces no Windows event, so without them it would only show up at the next poll.
std::vector<std::unique_ptr<IEventSource>> g_sources;
struct WindowRefreshSink final : IRefreshSink {
    // Called on a listener thread: hand it to the window, which debounces it like a USB change.
    void RequestRefresh(std::uint32_t delayMs) override { PostMessageW(g_hwnd, WM_DEVICE_EVENT, 0, (LPARAM)delayMs); }
} g_refreshSink;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_myFinder = false;  // widget mode on MyDockFinder's bar instead of the taskbar (PR #251)
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
bool g_trayWhenEmpty = false;  // taskbar mode: swap the strip for the tray placeholder while no device is shown
bool g_emptyToTray = false;    // that swap is in effect right now
bool g_showDisconnectedDevices = false;  // opt-in, explicit driver-reported offline devices only
bool g_dimSleeping = true;  // dim a sleeping device's last known value (menu option, on by default)
bool g_recordHistory = true;  // save readings for "Battery history..." (menu option, on by default)
int g_chargeLimit = 0;        // "Notify when charged to" 80/85/90%, 0 = off (the default)
bool g_chargedAlert = true;   // "fully charged" notification (PR #3946, menu option)
bool g_tieredAlerts = true;   // extra low-battery warnings at 10% and 5% (PR #3946, menu option)
bool g_shortRuntimeAlert = true;  // "charge today" notification when the time-to-empty is under 12 h (menu option)
bool g_hoverCard = true;       // card when the mouse rests on a device (off: plain tooltip)
bool g_hoverChart = true;      // 24-hour chart in that card
bool g_animateCharging = false;  // charging icons fill in a loop on the taskbar (menu option, PR #2463)
bool g_trayDeviceIcons = false;  // tray mode: device symbol + level bar instead of the number (PR #1632)
bool g_runtimeWidget = false;  // optionally show estimated remaining hours instead of % in taskbar widget
std::wstring g_widgetMonitor = L"all";  // which taskbars show the widget: "all", "main" or one monitor id
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)
bool g_stacked = false;      // icon above the percentage (menu option)
bool g_showPercentSign = true;  // include '%' in taskbar battery labels (menu option)
bool g_ringStyle = false;  // device icon inside a battery ring (menu option, PR #110)
bool g_connectionIcons = false;  // wireless mark for 2.4 GHz devices (menu option, PR #2277)
bool g_makeRoom = true;      // step past other apps' taskbar widgets (menu option)
// "Position on the taskbar": distance from the notification area in DPI-independent pixels. Stored as
// the TaskbarOffset value the widget has always read, so a value set by hand keeps working.
const int POSITION_OFFSETS[] = { 0, 40, 80, 120, 160, 200 };
widget::VerticalPlace g_verticalPlace = widget::VerticalPlace::Auto;  // column position on a left / right taskbar
bool g_farLeft = false;      // widget at the left end of the taskbar instead of next to the clock (menu option)
bool g_beforeApps = false;   // widget just left of the taskbar apps (menu option, PR #760)
bool g_keepMouseLastKnown = false;  // with "Hide last known", still show mice (menu option, PR #1422)
bool g_brandColors = false;  // off by default; known brands get their color (Razer green, Logitech blue, ...)
std::atomic<DWORD> g_pollIntervalMs{ 60 * 1000 };  // set from the menu, read by the worker thread
bool g_trackSince = false, g_trackAwake = false, g_trackEmpty = false;  // "Battery tracking" menu, off by default
bool g_demo = false;         // --demo (see DemoDevices)
bool g_trace = false;        // --trace: one line per poll in %TEMP%\wirelessbattery-trace.log (PR #303)
std::atomic<long> g_lastCount{ 0 };  // devices in the previous poll, to spot one that went missing

// Battery tracking: when each device was last charged and roughly how long it was awake since.
// One REG_BINARY value per device key under HKCU\Software\WirelessBattery\ChargeTracker.
const wchar_t* kTrackerKey = L"Software\\WirelessBattery\\ChargeTracker";
struct ChargeRecord {
    ULONGLONG since = 0;     // when the last charge ended, in seconds since 1601 (0 = not yet)
    ULONGLONG awakeSec = 0;  // seconds awake since then
    LONGLONG lastPct = -1;   // last live level (a jump up means it was charged)
};

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    ULONGLONG lastReadingTick = 0;  // last confirmed battery sample (PR #2143), for "Last reading: 5 min ago"
    bool added = false;
    bool lowWarned = false;
    bool chargedWarned = false;  // "fully charged" already shown during this charge (PR #3946)
    int missStreak = 0;      // consecutive polls the device was missing from (PR #303)
    bool limitWarned = false;  // "unplug at 80%" already shown during this charge
    bool shortWarned = false;  // "charge today" already shown during this discharge
    charge::Tracker charge;    // readings of the current charge, for "full in ~40 min"
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
    ChargeRecord chargeRecord;  // battery tracking (PR #3648)
    bool chargeLoaded = false;
};
std::map<std::wstring, TrayEntry> g_entries;
std::vector<std::wstring> g_unsupported;  // seen wireless mice / keyboards without a battery driver, menu only (PR #2463)
std::map<std::wstring, alerts::LowLatch> g_alertLatches;  // survive transient removal of a device
bool g_alertIconAdded = false;  // short-lived tray icon for widget-mode notifications
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;
ULONGLONG g_lastPollTick = 0;  // when the previous poll's results arrived

ULONGLONG NowSec() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000;
}

// A new charge cycle starts while the device charges or is full on power, when it is first seen, or when its level
// rose by 10 points or more (no charging flag, or charged while the app was closed). Each answered poll adds up to
// one interval awake.
void TrackCharge(TrayEntry& e, const DeviceStatus& s, ULONGLONG sampleMs) {
    ChargeRecord& r = e.chargeRecord;
    if (!e.chargeLoaded) {
        DWORD sz = sizeof(r);
        LSTATUS st = RegGetValueW(HKEY_CURRENT_USER, kTrackerKey, s.key.c_str(), RRF_RT_REG_BINARY, nullptr, &r, &sz);
        if (st != ERROR_SUCCESS || sz != sizeof(r))
            r = ChargeRecord{};
        e.chargeLoaded = true;
    }
    ChargeRecord before = r;
    bool awake = s.connected && !s.asleep && s.percent >= 0;
    if (s.charging || s.full || !r.since || (awake && r.lastPct >= 0 && s.percent >= r.lastPct + 10)) {
        r.since = NowSec();
        r.awakeSec = 0;
    } else if (awake) {
        r.awakeSec += sampleMs / 1000;
    }
    if (awake) r.lastPct = s.percent;
    if (memcmp(&before, &r, sizeof(r)) != 0)
        RegSetKeyValueW(HKEY_CURRENT_USER, kTrackerKey, s.key.c_str(), REG_BINARY, &r, sizeof(r));
}

std::wstring Duration(ULONGLONG sec) {
    std::wstring d = std::to_wstring(sec / 86400), h = std::to_wstring(sec / 3600 % 24);
    std::wstring m = std::to_wstring(sec / 60 % 60);
    if (sec >= 86400) return TF(L"{days} d {hours} h", { { L"days", d }, { L"hours", h } });
    if (sec >= 3600) return TF(L"{hours} h {minutes} min", { { L"hours", h }, { L"minutes", m } });
    return TF(L"{minutes} min", { { L"minutes", m } });
}

// User-supplied names are presentation-only, keyed by stable DeviceStatus.key.
// Driver identity, HID polling, brand detection and device diagnostics are unchanged.
aliases::AliasMap g_aliases;

std::wstring DisplayName(const TrayEntry& e) {
    return aliases::DisplayName(g_aliases, e.st.key, e.st.name);
}

void LoadAliases() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", RRF_RT_REG_MULTI_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS) return;
    if (size < 2 * sizeof(wchar_t) || size > 128 * 1024 || size % sizeof(wchar_t)) return;
    std::vector<wchar_t> data(size / sizeof(wchar_t), 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", RRF_RT_REG_MULTI_SZ,
                     nullptr, data.data(), &size) == ERROR_SUCCESS)
        g_aliases = aliases::Decode(data.data(), size / sizeof(wchar_t));
}

void SaveAliases() {
    auto data = aliases::Encode(g_aliases);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceAliases", REG_MULTI_SZ,
                    data.data(), (DWORD)(data.size() * sizeof(wchar_t)));
}

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

// Small Windows 11 icons (Segoe Fluent Icons, Segoe MDL2 Assets on Windows 10) in the menu's icon column,
// drawn in the menu's text colour for the light or dark theme. The menu stays the native Windows menu;
// check marks and radio marks keep their own column. The bitmaps live until the object goes away.
class MenuIcons {
  public:
    explicit MenuIcons(bool light) : light_(light) {
        UINT dpi = GetDpiForWindow(g_hwnd);
        size_ = MulDiv(16, dpi ? (int)dpi : 96, 96);
        for (const wchar_t* face : { L"Segoe Fluent Icons", L"Segoe MDL2 Assets" }) {
            HFONT f = CreateFontW(-size_, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
            HDC dc = CreateCompatibleDC(nullptr);
            HGDIOBJ old = SelectObject(dc, f);
            wchar_t got[LF_FACESIZE] = {};
            GetTextFaceW(dc, LF_FACESIZE, got);
            SelectObject(dc, old);
            DeleteDC(dc);
            if (!_wcsicmp(got, face)) { font_ = f; break; }
            DeleteObject(f);
        }
    }
    ~MenuIcons() {
        for (HBITMAP b : bitmaps_) DeleteObject(b);
        if (font_) DeleteObject(font_);
    }
    MenuIcons(const MenuIcons&) = delete;
    MenuIcons& operator=(const MenuIcons&) = delete;

    // `item` is a command id, or a position when `byPosition` (submenus have no command id).
    void Set(HMENU menu, UINT item, bool byPosition, wchar_t glyph) {
        HBITMAP b = Make(glyph);
        if (!b) return;
        MENUITEMINFOW mii{ sizeof(mii) };
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = b;
        SetMenuItemInfoW(menu, item, byPosition ? TRUE : FALSE, &mii);
    }
    void SetLast(HMENU menu, wchar_t glyph) { Set(menu, (UINT)GetMenuItemCount(menu) - 1, true, glyph); }

  private:
    HBITMAP Make(wchar_t glyph) {
        if (!font_) return nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), size_, -size_, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { DeleteDC(dc); return nullptr; }
        HGDIOBJ ob = SelectObject(dc, bmp);
        HGDIOBJ of = SelectObject(dc, font_);
        memset(bits, 0, (size_t)size_ * size_ * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{ 0, 0, size_, size_ };
        wchar_t s[2] = { glyph, 0 };
        DrawTextW(dc, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        // white-on-black coverage -> premultiplied alpha in the menu text colour
        const int c = light_ ? 0x1B : 0xF2;
        auto* p = static_cast<DWORD*>(bits);
        for (int i = 0; i < size_ * size_; i++) {
            int a = ((p[i] & 0xFF) + ((p[i] >> 8) & 0xFF) + ((p[i] >> 16) & 0xFF)) / 3;
            int v = c * a / 255;
            p[i] = ((DWORD)a << 24) | ((DWORD)v << 16) | ((DWORD)v << 8) | (DWORD)v;
        }
        SelectObject(dc, of);
        SelectObject(dc, ob);
        DeleteDC(dc);
        bitmaps_.push_back(bmp);
        return bmp;
    }
    bool light_;
    int size_ = 16;
    HFONT font_ = nullptr;
    std::vector<HBITMAP> bitmaps_;
};

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsEntryHidden(const TrayEntry& e) {
    if (IsHidden(e.st.key)) return true;
    // An explicitly offline device is not just a stale battery reading.
    if (g_showDisconnectedDevices && !e.st.connected) return false;
    // A mouse sleeps between uses; its last level stays useful (PR #1422, opt-in).
    if (g_hideLastKnown && g_keepMouseLastKnown && e.st.kind == DeviceKind::Mouse && e.lastPct >= 0) return false;
    return g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0));
}

bool ShouldDim(const DeviceStatus& s) {
    // An explicitly offline device keeps its dimmed "x", whatever the sleeping-device choice is.
    if (!s.connected) return true;
    // A sleeping device has no fresh reading: keep the last one, optionally at full brightness.
    if (s.asleep && !g_dimSleeping) return false;
    return s.percent < 0;
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

// A REG_MULTI_SZ with one "device-key<TAB>threshold" entry per override.
// Threshold -1 disables alerts; a missing key uses the default 15%.
std::map<std::wstring, int> g_alertOverrides;

void LoadAlertThresholds() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < 2 * sizeof(wchar_t) || size > 128 * 1024) return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    size_t limit = size / sizeof(wchar_t);
    for (size_t i = 0; i < limit && buf[i];) {
        size_t end = i;
        while (end < limit && buf[end]) ++end;
        if (end == limit) break;
        std::wstring item(buf.data() + i, end - i);
        size_t sep = item.rfind(L'\t');
        if (sep != std::wstring::npos && sep != 0) {
            std::wstring value = item.substr(sep + 1);
            wchar_t* tail = nullptr;
            long n = std::wcstol(value.c_str(), &tail, 10);
            if (tail != value.c_str() && *tail == 0 && n >= -1 && n <= 40 && alerts::ValidOverride((int)n))
                g_alertOverrides[item.substr(0, sep)] = (int)n;
        }
        i = end + 1;
    }
}

void SaveAlertThresholds() {
    std::wstring multi;
    for (const auto& [key, threshold] : g_alertOverrides) {
        multi += key + L"\t" + std::to_wstring(threshold);
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    if (multi.size() == 1) multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"AlertThresholds", REG_MULTI_SZ,
                    multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadTrayWhenEmpty() { return LoadBoolSetting(L"MoveToTrayWhenEmpty", false); }
void SaveTrayWhenEmpty(bool on) { SaveBoolSetting(L"MoveToTrayWhenEmpty", on); }
bool LoadShowDisconnectedDevices() { return LoadBoolSetting(L"ShowDisconnectedDevices", false); }
void SaveShowDisconnectedDevices(bool on) { SaveBoolSetting(L"ShowDisconnectedDevices", on); }
bool LoadDimSleeping() { return LoadBoolSetting(L"DimSleepingDevices", true); }
void SaveDimSleeping(bool on) { SaveBoolSetting(L"DimSleepingDevices", on); }
bool LoadRecordHistory() { return LoadBoolSetting(L"RecordBatteryHistory", true); }
bool LoadMakeRoom() { return LoadBoolSetting(L"MakeRoomForOtherWidgets", true); }
void SaveMakeRoom(bool on) { SaveBoolSetting(L"MakeRoomForOtherWidgets", on); }
// Signed: stored as a DWORD, read back as int, so negative values move the widget towards the tray.
int LoadTaskbarOffset() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return (int)v;
}
void SaveTaskbarOffset(int px) {
    DWORD v = (DWORD)px;
    if (px) RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TaskbarOffset", REG_DWORD, &v, sizeof(v));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"TaskbarOffset");
}
void SaveRecordHistory(bool on) { SaveBoolSetting(L"RecordBatteryHistory", on); }
bool LoadRuntimeWidget() { return LoadBoolSetting(L"EstimatedRuntimeInWidget", false); }
void SaveRuntimeWidget(bool on) { SaveBoolSetting(L"EstimatedRuntimeInWidget", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }
std::wstring LoadWidgetMonitor() {
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && buf[0])
        return buf;
    return LoadBoolSetting(L"OtherTaskbars", true) ? L"all" : L"main";  // the earlier on/off toggle
}
void SaveWidgetMonitor(const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"WidgetMonitor", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void SetWidgetMonitor(const std::wstring& v) {
    g_widgetMonitor = v;
    SaveWidgetMonitor(v);
    widget::SetMonitorChoice(v);
}
bool LoadStacked() { return LoadBoolSetting(L"StackedLayout", false); }
void SaveStacked(bool on) { SaveBoolSetting(L"StackedLayout", on); }
bool LoadShowPercentSign() { return LoadBoolSetting(L"ShowPercentSign", true); }
void SaveShowPercentSign(bool on) { SaveBoolSetting(L"ShowPercentSign", on); }
bool LoadRingStyle() { return LoadBoolSetting(L"RingStyle", false); }
void SaveRingStyle(bool on) { SaveBoolSetting(L"RingStyle", on); }
bool LoadConnectionIcons() { return LoadBoolSetting(L"ConnectionIcons", false); }
void SaveConnectionIcons(bool on) { SaveBoolSetting(L"ConnectionIcons", on); }
widget::VerticalPlace LoadVerticalPlace() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"VerticalPlace", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 1 ? widget::VerticalPlace::Top : v == 2 ? widget::VerticalPlace::AboveTray
         : v == 3 ? widget::VerticalPlace::Bottom : widget::VerticalPlace::Auto;
}
void SaveVerticalPlace(widget::VerticalPlace p) {
    DWORD v = p == widget::VerticalPlace::Top ? 1 : p == widget::VerticalPlace::AboveTray ? 2
              : p == widget::VerticalPlace::Bottom ? 3 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"VerticalPlace", REG_DWORD, &v, sizeof(v));
}
bool LoadBeforeApps() { return LoadBoolSetting(L"WidgetBeforeApps", false); }
bool LoadFarLeft() {
    const bool previousLeftSide = !LoadBeforeApps() && LoadBoolSetting(L"TaskbarPosition", false);
    return LoadBoolSetting(L"WidgetFarLeft", previousLeftSide);
}
void SaveFarLeft(bool on) { SaveBoolSetting(L"WidgetFarLeft", on); }
void SaveBeforeApps(bool on) { SaveBoolSetting(L"WidgetBeforeApps", on); }
bool LoadBrandColors() { return LoadBoolSetting(L"BrandColors", false); }
void SaveBrandColors(bool on) { SaveBoolSetting(L"BrandColors", on); }
bool LoadTrackSince() { return LoadBoolSetting(L"TrackSinceCharge", false); }
void SaveTrackSince(bool on) { SaveBoolSetting(L"TrackSinceCharge", on); }
bool LoadTrackAwake() { return LoadBoolSetting(L"TrackAwakeTime", false); }
void SaveTrackAwake(bool on) { SaveBoolSetting(L"TrackAwakeTime", on); }
bool LoadTrackEmpty() { return LoadBoolSetting(L"TrackTimeToEmpty", false); }
void SaveTrackEmpty(bool on) { SaveBoolSetting(L"TrackTimeToEmpty", on); }
DWORD LoadPollSeconds() {
    DWORD v = 60, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"PollIntervalSeconds", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return std::clamp<DWORD>(v, 30, 24 * 60 * 60);
}
void SavePollSeconds(DWORD seconds) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"PollIntervalSeconds", REG_DWORD, &seconds, sizeof(seconds));
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

// deviceGlyph: `text` is a Segoe Fluent Icons / MDL2 glyph (tray device icons, PR #1632), drawn regular weight.
HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim, bool deviceGlyph = false) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        static const wchar_t* iconFace = [] {  // Segoe Fluent Icons on Windows 11, MDL2 Assets on Windows 10
            HDC probeDc = CreateCompatibleDC(nullptr);
            HFONT probe = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe Fluent Icons");
            HGDIOBJ old = SelectObject(probeDc, probe);
            wchar_t actual[LF_FACESIZE] = {};
            GetTextFaceW(probeDc, LF_FACESIZE, actual);
            SelectObject(probeDc, old);
            DeleteObject(probe);
            DeleteDC(probeDc);
            return _wcsicmp(actual, L"Segoe Fluent Icons") == 0 ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        }();
        HFONT f = CreateFontW(-fh, 0, 0, 0, deviceGlyph ? FW_NORMAL : FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, deviceGlyph ? iconFace : L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

Accent AccentFor(const DeviceStatus& s) { return g_brandColors ? BrandOf(s) : Accent::None; }

// glyph != 0: the device's symbol with the level as the bar, instead of the number (option, PR #1632).
HICON IconFor(int pct, bool charging, bool dim, Accent accent, wchar_t glyph = 0) {
    Rgb fg = Foreground();
    BrandColor bc = AccentColor(accent, g_lightTaskbar);
    Rgb bar = charging        ? Rgb{ 0, 170, 255 }
            : pct <= LOW_BATTERY ? Rgb{ 232, 32, 48 }
            : pct <= 30       ? Rgb{ 255, 150, 0 }
            : accent != Accent::None ? Rgb{ bc.r, bc.g, bc.b }
                              : Rgb{ 30, 190, 90 };
    Rgb text = (!charging && pct >= 0 && pct <= LOW_BATTERY) ? Rgb{ 240, 50, 60 } : fg;
    if (glyph) return RenderIcon(std::wstring(1, glyph), pct, fg, bar, dim, true);
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

// "~40 min", "~1 h 20 min": rounded to 5 minutes, then to 10 above an hour (it is an estimate).
std::wstring ApproxMinutes(int mins) {
    if (mins < 60) return TF(L"~{minutes} min", { { L"minutes", std::to_wstring(std::max(5, (mins + 2) / 5 * 5)) } });
    int h = mins / 60, m = (mins % 60 + 5) / 10 * 10;
    if (m == 60) { h++; m = 0; }
    return m ? TF(L"~{hours} h {minutes} min", { { L"hours", std::to_wstring(h) }, { L"minutes", std::to_wstring(m) } })
             : TF(L"~{hours} h", { { L"hours", std::to_wstring(h) } });
}

// How old the last confirmed reading is (PR #2143), never counting failed polls.
std::wstring FreshnessText(const TrayEntry& e) {
    if (!e.lastReadingTick) return L"";
    ULONGLONG now = GetTickCount64(), age = reading_freshness::AgeMinutes(e.lastReadingTick, now);
    std::wstring ago = age == 0 ? std::wstring(T(L"just now"))
                     : age < 60 ? TF(L"{count} min ago", { { L"count", std::to_wstring(age) } })
                     : age < 1440 ? TF(L"{count} h ago", { { L"count", std::to_wstring(age / 60) } })
                                  : TF(L"{count} d ago", { { L"count", std::to_wstring(age / 1440) } });
    switch (reading_freshness::Classify(e.lastReadingTick, now, g_pollIntervalMs)) {
        case reading_freshness::State::Fresh: return TF(L"Last reading: {age}", { { L"age", ago } });
        case reading_freshness::State::Delayed: return TF(L"Delayed reading: {age}", { { L"age", ago } });
        case reading_freshness::State::Stale: return TF(L"Stale reading: {age}", { { L"age", ago } });
        default: return L"";
    }
}

bool IsStale(const TrayEntry& e) {
    return reading_freshness::Classify(e.lastReadingTick, GetTickCount64(), g_pollIntervalMs) ==
           reading_freshness::State::Stale;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) {
        std::wstring off = T(L"off / not connected");
        if (e.lastReadingTick) off += L", " + FreshnessText(e);
        return off;
    }
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = T(s.levelText);
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = TF(L"{percent}% (last known)", { { L"percent", std::to_wstring(e.lastPct) } });
    else t = T(L"unknown");
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.full || (s.charging && s.percent >= 100)) t += std::wstring(L", ") + T(L"fully charged");
    else if (int mins = s.charging ? charge::MinutesToFull(e.charge, (int64_t)time(nullptr)) : -1; mins > 0)
        t += std::wstring(L", ") + T(L"charging") + L", " + TF(L"full in {time}", { { L"time", ApproxMinutes(mins) } });
    else if (s.charging) t += std::wstring(L", ") + T(L"charging");
    if (s.asleep) t += std::wstring(L", ") + T(L"asleep");
    if (!s.note.empty() && s.percent < 0) t += std::wstring(L", ") + T(s.note);
    const ChargeRecord& c = e.chargeRecord;
    if (c.since && !s.charging && !s.full) {
        ULONGLONG now = NowSec(), age = now > c.since ? now - c.since : 0;
        if (g_trackSince) t += L", " + TF(L"{time} since charge", { { L"time", Duration(age) } });
        if (g_trackAwake) t += L", " + TF(L"~{time} awake", { { L"time", Duration(c.awakeSec) } });
    }
    if (g_trackEmpty && g_recordHistory && !s.charging && !s.full) {
        std::wstring left = battery_history::TimeToEmpty(s.key);  // the "Battery history" estimate
        if (!left.empty()) t += L", " + TF(L"{time} to empty", { { L"time", left } });
    }
    if (e.lastReadingTick) t += L", " + FreshnessText(e);
    return t;
}

void TraySetVersion(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

bool TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    bool ok = Shell_NotifyIconW(msg, &nid) != FALSE;
    if (ok && msg == NIM_ADD) TraySetVersion(uid);
    return ok;
}

// Adds or updates a tray icon; returns whether the shell has it. While Explorer is busy (e.g. at sign-in)
// NIM_ADD can time out and report failure although the icon was added, and NIM_MODIFY fails for an icon
// the shell no longer has, so each one falls back to the other.
bool TrayShow(bool added, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    if (TrayCall(added ? NIM_MODIFY : NIM_ADD, uid, icon, tip, infoTitle, info)) return true;
    if (!TrayCall(added ? NIM_ADD : NIM_MODIFY, uid, icon, tip, infoTitle, info)) return false;
    if (!added) TraySetVersion(uid);
    return true;
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowNotification(TrayEntry& e, const std::wstring& title, const std::wstring& message) {
    if (!g_useWidget && e.added) {
        TrayCall(NIM_MODIFY, e.uid, e.icon, DisplayName(e) + L"\n" + StatusText(e), title.c_str(), message);
        return;
    }
    // Windows notification balloons need a tray icon. Keep a temporary one in widget mode
    // long enough for the shell to display the notification, then remove it.
    if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
    g_alertIconAdded =
        TrayShow(g_alertIconAdded, ALERT_ICON_ID, g_placeholderIcon, L"BatteryHub", title.c_str(), message);
    if (g_alertIconAdded) SetTimer(g_hwnd, TIMER_ALERT_ICON, 60 * 1000, nullptr);
}

void ShowBatteryAlert(TrayEntry& e, alerts::LowAlert level) {
    const wchar_t* title = level == alerts::LowAlert::Emergency ? L"Battery almost empty"
                         : level == alerts::LowAlert::Critical  ? L"Battery critically low"
                                                                : L"Low battery";
    const wchar_t* text = level == alerts::LowAlert::Emergency ? L"{name} is at {percent}%. Charge it now."
                                                               : L"{name} is at {percent}%";
    ShowNotification(e, T(title), TF(text, { { L"name", DisplayName(e) }, { L"percent", std::to_wstring(e.st.percent) } }));
}

void ShowChargedAlert(TrayEntry& e) {
    ShowNotification(e, T(L"Fully charged"), TF(L"{name} is fully charged.", { { L"name", DisplayName(e) } }));
}

void ShowChargeLimitAlert(TrayEntry& e) {
    ShowNotification(e, T(L"You can unplug it"),
                     TF(L"{name} is charged to {percent}%. Stopping around here helps the battery last longer.",
                        { { L"name", DisplayName(e) }, { L"percent", std::to_wstring(e.st.percent) } }));
}

// The time-to-empty estimate says the device will not last the next 12 hours: once per discharge.
void ShowShortRuntimeAlert(TrayEntry& e, const std::wstring& left) {
    ShowNotification(e, T(L"Charge today"), TF(L"{name} may run out in {time}", { { L"name", DisplayName(e) }, { L"time", left } }));
}

void UpdatePlaceholder() {
    if (g_useWidget && !g_emptyToTray) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        g_placeholder = TrayShow(false, PLACEHOLDER_ID, g_placeholderIcon,
                                 std::wstring(L"BatteryHub\n") + T(L"No devices with battery info found"));
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = !g_useWidget && !IsEntryHidden(e) && (s.connected || g_showDisconnectedDevices);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    // Never display an old percentage as a live reading for an explicitly offline device.
    int shown = !s.connected ? -1 : (s.percent >= 0 ? s.percent : e.lastPct);
    bool dim = ShouldDim(s) || IsStale(e);  // a stale reading is not shown as live (PR #2143)
    // Fully charged on power: the normal green bar instead of the blue charging one.
    bool charging = s.connected && !s.asleep && s.charging && !s.full && shown < 100;
    Accent accent = AccentFor(s);
    std::wstring sig = std::to_wstring(shown) + (charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k") +
                       L"a" + std::to_wstring((int)accent) + (s.connected ? L"|on" : L"|off") +
                       (g_trayDeviceIcons ? L"|g" + std::to_wstring((int)s.kind) + (s.via == L"Bluetooth" ? L"b" : L"") : L"");
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = s.connected ? IconFor(shown, charging, dim, accent,
                                       g_trayDeviceIcons ? widget::DeviceGlyph(s.kind, s.via == L"Bluetooth") : 0)
                             : RenderIcon(L"\u00D7", -1, Foreground(), Foreground(), true);
        e.iconSig = sig;
    }
    std::wstring tip = DisplayName(e) + L"\n" + StatusText(e);

    e.added = TrayShow(e.added, e.uid, e.icon, tip);
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return (e.st.connected || g_showDisconnectedDevices) && !IsEntryHidden(e); }

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        if (ka != kb) return ka < kb;
        return DisplayName(*a) != DisplayName(*b) ? DisplayName(*a) < DisplayName(*b) : a->st.key < b->st.key;  // same name: keep a fixed order
    });
    return list;
}

void UpdateStatusSnapshot();

void UpdateWidget() {
    if (!g_useWidget || !widget::Alive()) return;  // not alive while the strip is swapped for the tray
    std::vector<TrayEntry*> list = OrderedEntries(true);
    // Two devices of one kind share an icon: give each a tiny caption so they can be told apart.
    std::vector<LabelInput> labelIn;
    for (auto* e : list) labelIn.push_back({ e->st.kind, DisplayName(*e) });
    std::vector<std::wstring> labels = ShortLabels(labelIn, 3);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        bool offline = !e->st.connected;
        it.percent = offline ? -1 : (e->st.percent >= 0 ? e->st.percent : e->lastPct);
        if (offline) it.text = L"\u00D7";  // clear status, not a stale percentage
        else if (e->st.percent >= 0) {
            // Never present a prediction as a live measurement for asleep, charging or full devices.
            if (g_runtimeWidget && g_recordHistory && !e->st.asleep &&
                !e->st.charging && !e->st.full)
                it.text = battery_history::EstimatedRuntimeText(e->st.key);
            // Insufficient, stale or otherwise unavailable estimates keep the normal battery readout.
            if (it.text.empty()) it.text = T(e->st.levelText);
        }
        it.charging = !offline && !e->st.asleep && e->st.charging;
        it.full = !offline && (e->st.full || (e->st.charging && it.percent >= 100));
        it.dim = ShouldDim(e->st) || IsStale(*e);
        it.low = !offline && !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        it.wirelessLink = !offline && e->st.via.rfind(L"2.4 GHz", 0) == 0;
        it.label = labels[items.size()];
        it.accent = AccentFor(e->st);
        it.name = DisplayName(*e);
        {   // the card shows the level in large type already: here only the state, e.g. "Charging", "Asleep"
            std::vector<std::wstring> parts;
            if (offline) parts.push_back(T(L"off / not connected"));
            else {
                if (!e->st.detail.empty() && e->st.percent >= 0) parts.push_back(e->st.detail);
                if (it.full) parts.push_back(T(L"fully charged"));
                else if (it.charging) parts.push_back(T(L"charging"));
                if (e->st.asleep) parts.push_back(T(L"asleep"));
            }
            for (const auto& p : parts) it.status += (it.status.empty() ? L"" : L", ") + p;
            if (!it.status.empty()) it.status[0] = towupper(it.status[0]);
        }
        if (!offline && !e->st.charging) {
            it.hoursLeft = battery_history::HoursRemaining(e->st.key);
            it.hoursLeftText = battery_history::RemainingText(e->st.key);
            if (it.hoursLeft < 0) {  // not enough for the estimate yet: a rough one, marked as approximate
                it.hoursLeft = battery_history::RoughHoursRemaining(e->st.key);
                if (it.hoursLeft >= 0)
                    it.hoursLeftText = TF(L"{time} (approx.)", { { L"time", battery_history::FormatHours(it.hoursLeft) } });
            }
            if (!it.hoursLeftText.empty()) it.info.push_back(std::wstring(T(L"Estimated time to empty")) + L": " + it.hoursLeftText);
        }
        if (!e->st.via.empty() && e->st.via != L"demo") it.info.push_back(T(e->st.via));
        if (g_hoverCard && g_hoverChart && g_recordHistory)
            for (const auto& pt : battery_history::Recent(e->st.key, 24)) it.trend.push_back({ pt.secondsAgo, pt.percent });
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += DisplayName(*e) + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = std::wstring(L"BatteryHub: ") + T(L"No devices with battery info found");
    widget::SetItems(items, tip);
    UpdateStatusSnapshot();
}

// %LOCALAPPDATA%\WirelessBattery\status.json for external tools (PR #378: YASB, Rainmeter, scripts): the
// devices the widget shows, in its order and with its hiding rules, rewritten whenever they change and
// deleted on exit. Its document format is its own (see docs/yasb.md); `--json` keeps its array format.
StatusDevice ToStatus(const DeviceStatus& s, int lastPct) {
    StatusDevice d;
    d.key = s.key;
    d.name = s.name;
    d.via = s.via;
    d.detail = s.detail;
    d.levelText = s.levelText;
    d.kind = s.kind;
    d.percent = s.percent;
    d.lastPct = lastPct;
    d.millivolts = s.millivolts;
    d.charging = s.charging;
    d.asleep = s.asleep;
    d.connected = s.connected;
    return d;
}

void UpdateStatusSnapshot() {
    if (g_demo) return;
    std::vector<StatusDevice> devices;
    for (auto* e : OrderedEntries(true)) devices.push_back(ToStatus(e->st, e->lastPct));
    WriteStatusSnapshot(devices, "snapshot", LOW_BATTERY);
}

void Reconcile();

// Last percentage read while NOT charging, per device name (the name, not the key: a device moved
// from its receiver to a cable gets a new key). Some devices restart their charge estimate when a
// cable goes in -- a Logitech G502 X reports ~50% at first and only reaches its real 70-something
// after a while -- but a battery does not drop while charging, so until the reading catches up the
// last value from before charging is shown.
struct IdleReading { int pct; ULONGLONG at; };
std::map<std::wstring, IdleReading> g_idlePct;
constexpr ULONGLONG kIdlePctMaxAgeMs = 12ull * 60 * 60 * 1000;

void SmoothCharging(DeviceStatus& s) {
    if (s.percent < 0 || s.name.empty() || s.name.find(L"wireless device") != std::wstring::npos) return;
    ULONGLONG now = GetTickCount64();
    if (!s.charging) {
        g_idlePct[s.name] = { s.percent, now };
        return;
    }
    auto it = g_idlePct.find(s.name);
    if (it == g_idlePct.end() || now - it->second.at > kIdlePctMaxAgeMs) return;
    if (s.percent < it->second.pct) {
        s.percent = it->second.pct;
        s.levelText.clear();
    } else {
        g_idlePct.erase(it);  // caught up: the device's own reading is trusted again
    }
}

void OnResults(std::vector<DeviceStatus>& results) {
    if (dashboard::IsOpen()) dashboard::UpdateDevices(results, battery_history::GetStore());
    g_unsupported.clear();
    results.erase(std::remove_if(results.begin(), results.end(), [](const DeviceStatus& s) {
                      if (s.unsupported) g_unsupported.push_back(s.name);
                      return s.unsupported;
                  }), results.end());
    for (auto& s : results) SmoothCharging(s);
    std::map<std::wstring, bool> seen;
    ULONGLONG tick = GetTickCount64();
    ULONGLONG sampleMs = g_lastPollTick ? std::min<ULONGLONG>(tick - g_lastPollTick, g_pollIntervalMs) : 0;
    g_lastPollTick = tick;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        const int previousPercent = e.st.connected && !e.st.asleep ? e.st.percent : -1;  // the last fresh reading
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        if (reading_freshness::Confirmed(s.percent, s.connected, s.asleep)) e.lastReadingTick = GetTickCount64();
        e.lastSeen = GetTickCount64();
        e.missStreak = 0;
        seen[s.key] = true;
        // Evaluate even in widget mode: ApplyEntry only manages tray icons.
        // Alert latches are kept by stable device key even if the driver briefly stops reporting it.
        alerts::LowLatch& latch = g_alertLatches[s.key];
        alerts::LowAlert low = IsHidden(s.key) ? alerts::LowAlert::None
                             : alerts::EvaluateLow(s, alerts::EffectiveThreshold(g_alertOverrides, s.key), latch, g_tieredAlerts);
        e.lowWarned = latch.low;
        bool notify = low != alerts::LowAlert::None;
        if (g_alertLatches.size() > 256) g_alertLatches.erase(g_alertLatches.begin());
        bool charged = g_chargedAlert && !IsHidden(s.key) && alerts::ShouldNotifyCharged(s, previousPercent, e.chargedWarned);
        if (g_recordHistory && s.connected && !s.asleep && s.percent >= 0)
            battery_history::Observe(s.key, s.percent, s.charging);
        if (g_recordHistory && !g_demo) battery_intelligence::ObserveDevice(s, (int64_t)time(nullptr));  // charging sessions (PR #3946)
        bool limit = !IsHidden(s.key) && alerts::ShouldNotifyChargeLimit(s, g_chargeLimit, previousPercent, e.limitWarned);
        if (s.connected && !s.asleep) charge::Observe(e.charge, s.charging, s.percent, (int64_t)time(nullptr));
        bool shortRuntime = false;
        std::wstring left;
        if (g_shortRuntimeAlert && !notify && !IsHidden(s.key) && s.connected && !s.asleep && s.percent >= 0) {
            int threshold = alerts::EffectiveThreshold(g_alertOverrides, s.key);
            double hours = s.charging ? -1 : battery_history::HoursRemaining(s.key);
            if (threshold != alerts::kDisabled &&
                charge::ShouldWarnShortRuntime(hours, s.charging, s.percent, threshold, e.shortWarned)) {
                shortRuntime = true;
                left = battery_history::RemainingText(s.key);
            }
        } else if (s.charging) {
            e.shortWarned = false;
        }
        if (!g_demo && (g_trackSince || g_trackAwake)) TrackCharge(e, s, sampleMs);
        ApplyEntry(e);
        if (notify) ShowBatteryAlert(e, low);
        else if (charged) ShowChargedAlert(e);
        else if (limit) ShowChargeLimitAlert(e);
        else if (shortRuntime && !left.empty()) ShowShortRuntimeAlert(e, left);
    }
    // Devices answering right now, by name: a device plugged in by cable (or on Bluetooth) also
    // has an entry from its receiver, which stops answering then. Generic names are not matched.
    std::set<std::wstring> liveNames;
    for (auto& s : results)
        if (s.connected && !s.asleep && s.percent >= 0 && s.name.find(L"wireless device") == std::wstring::npos)
            liveNames.insert(s.name);
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while
        // (at least two poll intervals), unless the same device is answering another way (then that one
        // is the current value).
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen <
                           std::max<ULONGLONG>((ULONGLONG)e.st.graceSec * 1000, 2ULL * g_pollIntervalMs) &&
                       !liveNames.count(e.st.name);
        if (inGrace) {
            // One missed answer is usually a receiver busy with input reports, not a sleeping device:
            // keep the value solid and only dim it from the second missed poll on (PR #303).
            if (++e.missStreak >= 2) {
                e.st.percent = -1;
                e.st.asleep = true;
            }
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    Reconcile();  // react to the new device list now, not at the next timer tick
    UpdatePlaceholder();
    UpdateWidget();
    memory_trim::Soon();  // the poll (HID, Bluetooth, drivers) is done: give its pages back until the next one
}

bool NothingShown() {
    for (const auto& [k, e] : g_entries)
        if (IsShown(e)) return false;
    return true;
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
// With "Move to tray when no devices" the strip gives way to the tray placeholder while nothing is shown.
void Reconcile() {
    // Taskbar mode, option on, nothing to show: the strip is replaced by the tray placeholder.
    bool emptyToTray = g_wantWidget && g_trayWhenEmpty && NothingShown();
    bool wasAlive = widget::Alive();
    // Keep, create or remove the strip.
    bool alive = g_wantWidget && !emptyToTray && wasAlive;
    if (g_wantWidget && !emptyToTray && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget || emptyToTray) widget::Destroy();
    // Stays in widget mode while swapped, so device icons are not added to the tray in between.
    bool useWidget = alive || emptyToTray;
    bool changed = useWidget != g_useWidget || emptyToTray != g_emptyToTray;
    g_useWidget = useWidget;
    g_emptyToTray = emptyToTray;
    // Refresh after a change, or when the strip was just re-created.
    if (changed || (alive && !wasAlive)) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
    } else if (g_useWidget) {
        widget::Reposition();
    } else {
        // Retry tray icons whose NIM_ADD failed while the shell notification area was starting.
        bool anyAdded = false;
        for (auto& [k, e] : g_entries) {
            if (!e.added && IsShown(e)) ApplyEntry(e);
            anyAdded |= e.added;
        }
        if (anyAdded == g_placeholder) UpdatePlaceholder();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void SetKeepMouseLastKnown(bool on) {
    g_keepMouseLastKnown = on;
    SaveBoolSetting(L"KeepMouseLastKnown", on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetTrayWhenEmpty(bool on) {
    g_trayWhenEmpty = on;
    SaveTrayWhenEmpty(on);
    Reconcile();
}

void SetShowDisconnectedDevices(bool on) {
    g_showDisconnectedDevices = on;
    SaveShowDisconnectedDevices(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
}

void SetDimSleeping(bool on) {
    g_dimSleeping = on;
    SaveDimSleeping(on);
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdateWidget();
}

void ReAddAll() {
    g_placeholder = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
}

// Switches the UI language ("" = follow Windows) and redraws everything that shows text.
void SetLanguage(const std::wstring& code);

void RedrawAll() {
    if (g_alertIconAdded) {  // alert icon shares the placeholder's HICON
        KillTimer(g_hwnd, TIMER_ALERT_ICON);
        TrayDelete(ALERT_ICON_ID);
        g_alertIconAdded = false;
    }
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
}

void SetLanguage(const std::wstring& code) {
    i18n::SetChoice(code);
    RedrawAll();  // tray tooltips and the placeholder text
    UpdateWidget();
}

void SetConnectionIcons(bool on) {
    g_connectionIcons = on;
    SaveConnectionIcons(on);
    widget::SetConnectionIcons(on);
}

void SetRingStyle(bool on) {
    g_ringStyle = on;
    SaveRingStyle(on);
    widget::SetRingStyle(on);
}

void SetShowPercentSign(bool on) {
    g_showPercentSign = on;
    SaveShowPercentSign(on);
    widget::SetShowPercentSign(on);
}

void SetStacked(bool on) {
    g_stacked = on;
    SaveStacked(on);
    widget::SetStacked(on);
}

void SetMakeRoom(bool on) {
    g_makeRoom = on;
    SaveMakeRoom(on);
    widget::SetMakeRoom(on);
}

void SetTaskbarOffset(int px) {
    SaveTaskbarOffset(px);
    if (g_useWidget) widget::Reposition();
}

void SetWidgetPlace(widget::VerticalPlace p) {
    g_verticalPlace = p;
    SaveVerticalPlace(p);
    widget::SetVerticalPlace(p);
}

void SetBeforeApps(bool on) {
    g_beforeApps = on;
    SaveBeforeApps(on);
    widget::SetBeforeApps(on);
}

void SetFarLeft(bool on) {
    g_farLeft = on;
    SaveFarLeft(on);
    widget::SetFarLeft(on);
}

void SetBrandColors(bool on) {
    g_brandColors = on;
    SaveBrandColors(on);
    RedrawAll();
    UpdateWidget();
}

void SetChargeLimit(int limit) {
    g_chargeLimit = limit;
    DWORD v = (DWORD)limit;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"ChargeLimitAlert", REG_DWORD, &v, sizeof(v));
    for (auto& [k, e] : g_entries) e.limitWarned = false;
}

int LoadChargeLimit() {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"ChargeLimitAlert", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    for (int l : alerts::kChargeLimits)
        if ((int)v == l) return l;
    return 0;
}

void SetTrackSince(bool on) {
    g_trackSince = on;
    SaveTrackSince(on);
    RedrawAll();
    UpdateWidget();
}

void SetTrackAwake(bool on) {
    g_trackAwake = on;
    SaveTrackAwake(on);
    RedrawAll();
    UpdateWidget();
}

void SetTrackEmpty(bool on) {
    g_trackEmpty = on;
    SaveTrackEmpty(on);
    RedrawAll();
    UpdateWidget();
}

void SetPollSeconds(DWORD seconds) {
    g_pollIntervalMs = seconds * 1000;
    SavePollSeconds(seconds);
    SetEvent(g_refreshEvt);  // poll now, so the new interval starts right away
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdatePlaceholder();
    UpdateWidget();
}

void RenameDevice(const std::wstring& key) {
    auto it = g_entries.find(key);
    if (it == g_entries.end()) return;
    const std::wstring originalName = it->second.st.name;
    std::wstring chosen;
    if (!PromptDeviceName(g_hwnd, DisplayName(it->second), chosen)) return;
    if (chosen == originalName) g_aliases.erase(key);
    else if (g_aliases.count(key) || g_aliases.size() < aliases::kMaxAliases) g_aliases[key] = chosen;
    else return;  // bounded registry value; don't silently drop other users' names
    SaveAliases();
    // The modal editor pumps the message queue, so polling may remove the entry.
    // Resolve the iterator again rather than dereferencing a potentially stale one.
    it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdateWidget();
}

void ResetDeviceName(const std::wstring& key) {
    if (!g_aliases.erase(key)) return;
    SaveAliases();
    auto it = g_entries.find(key);
    if (it != g_entries.end()) ApplyEntry(it->second);
    UpdateWidget();
}

void SetAlertOverride(const std::wstring& key, int threshold) {
    if (threshold == alerts::kUseDefault) g_alertOverrides.erase(key);
    else if (alerts::ValidOverride(threshold)) g_alertOverrides[key] = threshold;
    else return;
    SaveAlertThresholds();
    g_alertLatches.erase(key);
    auto it = g_entries.find(key);
    if (it != g_entries.end()) it->second.lowWarned = false;
    SetEvent(g_refreshEvt);  // reconsider with a fresh reading; never alert from stale data
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    MenuIcons icons(g_lightTaskbar);  // freed after the menu closes
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"No devices with battery info found"));
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, T(L"Point at a device to show, hide or move it"));
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        if (!e->st.via.empty()) {  // the connection, moved off the device line (PR #3292)
            const auto connection = TF(L"Connection: {connection}", { { L"connection", T(e->st.via) } });
            AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, connection.c_str());
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? T(L"Show on the taskbar") : T(L"Show in the tray"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? T(L"Move left") : T(L"Move up"));
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? T(L"Move right") : T(L"Move down"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, base + DEV_RENAME, T(L"Rename device..."));
        icons.SetLast(sub, 0xE8AC);  // Rename
        AppendMenuW(sub, MF_STRING | (g_aliases.count(k) ? 0 : MF_GRAYED),
                    base + DEV_RESET_NAME, T(L"Reset to original name"));
        HMENU alertMenu = CreatePopupMenu();
        int selectedThreshold = alerts::OverrideFor(g_alertOverrides, k);
        AppendMenuW(alertMenu, MF_STRING | (selectedThreshold == alerts::kUseDefault ? MF_CHECKED : 0),
                    base + DEV_ALERT_DEFAULT, T(L"Use default (15%)"));
        AppendMenuW(alertMenu, MF_STRING | (selectedThreshold == alerts::kDisabled ? MF_CHECKED : 0),
                    base + DEV_ALERT_OFF, T(L"Disable notifications"));
        AppendMenuW(alertMenu, MF_SEPARATOR, 0, nullptr);
        for (int i = 0; i < alerts::kThresholdCount; ++i) {
            int level = alerts::kThresholds[i];
            std::wstring label = std::to_wstring(level) + L"%";
            AppendMenuW(alertMenu, MF_STRING | (selectedThreshold == level ? MF_CHECKED : 0),
                        base + DEV_ALERT_FIRST + i, label.c_str());
        }
        AppendMenuW(sub, MF_POPUP, (UINT_PTR)alertMenu, T(L"Battery alerts"));
        icons.SetLast(sub, 0xEA8F);  // Ringer
        AppendMenuW(sub, MF_STRING, base + DEV_HISTORY, T(L"Battery history..."));
        icons.SetLast(sub, 0xE81C);  // History
        AppendMenuW(sub, MF_STRING, base + DEV_DASHBOARD, T(L"View in dashboard..."));
        std::wstring line = DisplayName(*e) + L"  ·  " + StatusText(*e);
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        icons.SetLast(m, widget::DeviceGlyph(e->st.kind, e->st.via == L"Bluetooth"));
        keys.push_back(k);
    }
    if (!g_unsupported.empty()) {  // detected, but no driver reads their battery (PR #2463)
        HMENU un = CreatePopupMenu();
        for (auto& n : g_unsupported) AppendMenuW(un, MF_STRING | MF_GRAYED, 0, n.c_str());
        AppendMenuW(un, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(un, MF_STRING, ID_REPORT, T(L"Request support for a device..."));
        std::wstring title = TF(L"No battery info ({count})", { { L"count", std::to_wstring(g_unsupported.size()) } });
        AppendMenuW(m, MF_POPUP, (UINT_PTR)un, title.c_str());
    }
    if (!all.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_recordHistory ? MF_CHECKED : 0), ID_RECORD_HISTORY, T(L"Record battery history"));
    {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, ID_LIMIT_OFF, T(L"Off"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        UINT checked = ID_LIMIT_OFF;
        for (UINT i = 0; i < (UINT)alerts::kChargeLimitCount; i++) {
            AppendMenuW(sub, MF_STRING, ID_LIMIT_FIRST + i, (std::to_wstring(alerts::kChargeLimits[i]) + L"%").c_str());
            if (g_chargeLimit == alerts::kChargeLimits[i]) checked = ID_LIMIT_FIRST + i;
        }
        CheckMenuRadioItem(sub, ID_LIMIT_OFF, ID_LIMIT_FIRST + alerts::kChargeLimitCount - 1, checked, MF_BYCOMMAND);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Notify when charged to (saves the battery)"));
    }
    AppendMenuW(m, MF_STRING | (g_chargedAlert ? MF_CHECKED : 0), ID_CHARGED_ALERT, T(L"Notify when fully charged"));
    AppendMenuW(m, MF_STRING | (g_tieredAlerts ? MF_CHECKED : 0), ID_TIERED_ALERTS, T(L"Warn again at 10% and 5%"));
    AppendMenuW(m, MF_STRING | (g_shortRuntimeAlert ? MF_CHECKED : 0) | (g_recordHistory ? 0 : MF_GRAYED), ID_SHORT_RUNTIME_ALERT,
                T(L"Warn when a battery won't last the day"));
    {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (g_trackSince ? MF_CHECKED : 0), ID_TRACK_SINCE, T(L"Time since charge"));
        AppendMenuW(sub, MF_STRING | (g_trackAwake ? MF_CHECKED : 0), ID_TRACK_AWAKE, T(L"Awake time (rough)"));
        UINT emptyFlags = MF_STRING | (g_trackEmpty ? MF_CHECKED : 0) | (g_recordHistory ? 0 : MF_GRAYED);
        AppendMenuW(sub, emptyFlags, ID_TRACK_EMPTY, T(L"Estimated time to empty"));  // needs the recorded history
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Battery tracking"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Display settings (PR #3292): how and where the devices are shown, in one submenu.
    HMENU display = CreatePopupMenu();
    AppendMenuW(display, MF_STRING | (g_wantWidget && !g_myFinder ? MF_CHECKED : 0), ID_MODE_WIDGET,
                T(L"Show on the taskbar (icon + percentage)"));
    AppendMenuW(display, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, T(L"Show as tray icons"));
    // Offered once MyDockFinder is running, or while it is the chosen mode.
    if (g_myFinder || FindWindowW(L"MyFinderApp", nullptr))
        AppendMenuW(display, MF_STRING | (g_wantWidget && g_myFinder ? MF_CHECKED : 0), ID_MODE_MYFINDER,
                    T(L"Show on MyDockFinder (experimental)"));
    AppendMenuW(display, MF_STRING | (g_trayDeviceIcons ? MF_CHECKED : 0) | (g_wantWidget ? MF_GRAYED : 0), ID_TRAY_DEVICE_ICONS,
                T(L"Tray icons show the device type"));
    AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
    UINT bgFlags = MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(display, bgFlags, ID_WIDGET_BACKGROUND, T(L"Background behind taskbar text (for transparent taskbars)"));
    AppendMenuW(display, MF_STRING | (g_stacked ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_STACKED, T(L"Icons above percentages (narrower)"));
    AppendMenuW(display, MF_STRING | (g_showPercentSign ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_PERCENT_SIGN,
                T(L"Show percent sign (%)"));
    AppendMenuW(display, MF_STRING | (g_ringStyle ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_RING_STYLE,
                T(L"Ring style (icon inside a battery ring)"));
    AppendMenuW(display, MF_STRING | (g_animateCharging ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_ANIMATE_CHARGING,
                T(L"Animate charging devices"));
    AppendMenuW(display, MF_STRING | (g_connectionIcons ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_CONNECTION_ICONS,
                T(L"Mark devices on a 2.4 GHz receiver"));
    AppendMenuW(display, MF_STRING | (g_hoverCard ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_HOVER_CARD,
                T(L"Show a card when the mouse rests on a device"));
    AppendMenuW(display, MF_STRING | (g_hoverChart ? MF_CHECKED : 0) | (g_wantWidget && g_hoverCard && g_recordHistory ? 0 : MF_GRAYED),
                ID_HOVER_CHART, T(L"Show the last 24 hours in the card"));
    {
        HMENU sub = CreatePopupMenu();
        UINT f = MF_STRING | (g_wantWidget ? 0 : MF_GRAYED);
        const UINT count = ARRAYSIZE(POSITION_OFFSETS);
        const int current = LoadTaskbarOffset();
        UINT checked = ID_POSITION_FIRST + count;
        for (UINT i = 0; i < count; i++) {
            int px = POSITION_OFFSETS[i];
            std::wstring label = px ? TF(L"{px} px from the notification area", { { L"px", std::to_wstring(px) } })
                                    : T(L"Next to the notification area");
            AppendMenuW(sub, f, ID_POSITION_FIRST + i, label.c_str());
            if (px == current) checked = ID_POSITION_FIRST + i;
        }
        // Any other value (typed in, or set by hand in the registry) shows as the custom entry.
        std::wstring custom = checked == ID_POSITION_FIRST + count
                                  ? TF(L"Custom ({px} px)...", { { L"px", std::to_wstring(current) } })
                                  : std::wstring(T(L"Custom..."));
        AppendMenuW(sub, f, ID_POSITION_FIRST + count, custom.c_str());
        CheckMenuRadioItem(sub, ID_POSITION_FIRST, ID_POSITION_FIRST + count, checked, MF_BYCOMMAND);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, f | (g_makeRoom ? MF_CHECKED : 0), ID_MAKE_ROOM, T(L"Make room for other taskbar widgets"));
        AppendMenuW(display, MF_POPUP | (g_wantWidget ? 0 : MF_GRAYED), (UINT_PTR)sub, T(L"Position on the taskbar"));
    }
    // Left / right taskbar: choose where the column sits.
    if (widget::Vertical()) {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, ID_PLACE_AUTO, T(L"Automatic (keeps clear of app icons)"));
        AppendMenuW(sub, MF_STRING, ID_PLACE_TOP, T(L"Top of the taskbar"));
        AppendMenuW(sub, MF_STRING, ID_PLACE_TRAY, T(L"Above the notification area"));
        AppendMenuW(sub, MF_STRING, ID_PLACE_BOTTOM,
                    T(L"Bottom of the taskbar (moves the taskbar icons up, Windows 11)"));
        CheckMenuRadioItem(sub, ID_PLACE_AUTO, ID_PLACE_BOTTOM,
                           g_verticalPlace == widget::VerticalPlace::Top ? ID_PLACE_TOP
                           : g_verticalPlace == widget::VerticalPlace::AboveTray ? ID_PLACE_TRAY
                           : g_verticalPlace == widget::VerticalPlace::Bottom ? ID_PLACE_BOTTOM : ID_PLACE_AUTO,
                           MF_BYCOMMAND);
        AppendMenuW(display, MF_POPUP, (UINT_PTR)sub, T(L"Position on a vertical taskbar"));
    }
    AppendMenuW(display, MF_STRING | (g_farLeft ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_FAR_LEFT, T(L"At the left end of the taskbar"));
    AppendMenuW(display, MF_STRING | (g_beforeApps ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_BEFORE_APPS,
                T(L"Just before the taskbar apps"));
    AppendMenuW(display, MF_STRING | (g_runtimeWidget ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED), ID_RUNTIME_WIDGET,
                T(L"Show estimated time instead of % (when available)"));
    // With more than one monitor: choose which taskbars show the widget (all, main only, or one monitor).
    auto monitors = widget::OtherMonitors();
    if (monitors.size() > 31) monitors.resize(31);
    if (!monitors.empty()) {
        HMENU sub = CreatePopupMenu();
        UINT f = MF_STRING | (g_wantWidget ? 0 : MF_GRAYED);
        AppendMenuW(sub, f, ID_MONITOR_ALL, T(L"All monitors"));
        AppendMenuW(sub, f, ID_MONITOR_MAIN, T(L"Main monitor only"));
        UINT checked = g_widgetMonitor == L"main" ? ID_MONITOR_MAIN : ID_MONITOR_ALL;
        bool found = g_widgetMonitor == L"all" || g_widgetMonitor == L"main";
        std::vector<std::wstring> ids;
        for (const auto& mo : monitors) ids.push_back(mo.id);
        int chosen = found ? -1 : widget::FindMonitor(ids, g_widgetMonitor);
        for (UINT i = 0; i < monitors.size(); i++) {
            AppendMenuW(sub, f, ID_MONITOR_FIRST + i, TF(L"{monitor} only", { { L"monitor", monitors[i].label } }).c_str());
            if ((int)i == chosen) { checked = ID_MONITOR_FIRST + i; found = true; }
        }
        if (!found) {  // the chosen monitor is unplugged: the main taskbar shows the widget meanwhile
            AppendMenuW(sub, MF_STRING | MF_GRAYED, ID_MONITOR_FIRST + 31, T(L"Chosen monitor (not connected, shown on main)"));
            checked = ID_MONITOR_FIRST + 31;
        }
        CheckMenuRadioItem(sub, ID_MONITOR_ALL, ID_MONITOR_FIRST + 31, checked, MF_BYCOMMAND);
        AppendMenuW(display, MF_POPUP | (g_wantWidget ? 0 : MF_GRAYED), (UINT_PTR)sub, T(L"Show the widget on"));
        icons.SetLast(display, 0xE7F4);  // TVMonitor
    }
    AppendMenuW(display, MF_STRING | (g_brandColors ? MF_CHECKED : 0), ID_BRAND_COLORS, T(L"Brand colors (Razer green, Logitech blue, ...)"));
    AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(display, MF_STRING | (g_hideLastKnown ? MF_CHECKED : 0), ID_HIDE_LAST_KNOWN, T(L"Hide last known / asleep devices"));
    AppendMenuW(display, MF_STRING | (g_keepMouseLastKnown ? MF_CHECKED : 0) | (g_hideLastKnown ? 0 : MF_GRAYED),
                ID_KEEP_MOUSE_LAST_KNOWN, T(L"Keep last known battery for mice"));
    UINT emptyFlags = MF_STRING | (g_trayWhenEmpty ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(display, emptyFlags, ID_TRAY_WHEN_EMPTY, T(L"Move to tray when no devices"));
    AppendMenuW(display, MF_STRING | (g_showDisconnectedDevices ? MF_CHECKED : 0), ID_SHOW_DISCONNECTED, T(L"Show disconnected devices"));
    AppendMenuW(display, MF_STRING | (g_dimSleeping ? MF_CHECKED : 0), ID_DIM_SLEEPING, T(L"Dim sleeping devices"));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)display, T(L"Display settings"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Language: automatic, every built-in or user-added language (in its own name), and a way to add one.
    i18n::Init();  // picks up language files added since the last time
    auto languages = i18n::Available();
    if (languages.size() > 64) languages.resize(64);
    HBITMAP russianFlag = LoadBitmapW(g_inst, L"FLAG_RU");  // PR #1754
    {
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, ID_LANG_AUTO, T(L"Automatic (Windows language)"));
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        UINT checked = ID_LANG_AUTO;
        for (UINT i = 0; i < languages.size(); i++) {
            AppendMenuW(sub, MF_STRING, ID_LANG_FIRST + i, languages[i].name.c_str());
            if (russianFlag && _wcsicmp(languages[i].code.c_str(), L"ru") == 0) {
                MENUITEMINFOW item{ sizeof(item) };
                item.fMask = MIIM_BITMAP;
                item.hbmpItem = russianFlag;
                SetMenuItemInfoW(sub, ID_LANG_FIRST + i, FALSE, &item);
            }
            if (!i18n::Choice().empty() && _wcsicmp(languages[i].code.c_str(), i18n::Choice().c_str()) == 0) checked = ID_LANG_FIRST + i;
        }
        CheckMenuRadioItem(sub, ID_LANG_AUTO, ID_LANG_FIRST + 63, checked, MF_BYCOMMAND);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, ID_LANG_ADD, T(L"Add your own language..."));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Language"));
        icons.SetLast(m, 0xE774);  // Globe
    }
    {
        HMENU sub = CreatePopupMenu();
        const UINT count = ARRAYSIZE(POLL_SECONDS);
        UINT checked = 0;
        for (UINT i = 0; i < count; i++) {
            DWORD s = POLL_SECONDS[i];
            std::wstring label = s < 60 ? TF(L"{seconds} s", { { L"seconds", std::to_wstring(s) } })
                                        : TF(L"{minutes} min", { { L"minutes", std::to_wstring(s / 60) } });
            AppendMenuW(sub, MF_STRING, ID_POLL_FIRST + i, label.c_str());
            if (s * 1000 == g_pollIntervalMs) checked = ID_POLL_FIRST + i;
        }
        if (checked) CheckMenuRadioItem(sub, ID_POLL_FIRST, ID_POLL_FIRST + count - 1, checked, MF_BYCOMMAND);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, T(L"Check battery every"));
        icons.SetLast(m, 0xE916);  // Stopwatch
    }
    AppendMenuW(m, MF_STRING, ID_DASHBOARD, T(L"Device dashboard..."));
    AppendMenuW(m, MF_STRING, ID_REFRESH, T(L"Refresh now"));
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, T(L"Start with Windows"));
    AppendMenuW(m, MF_STRING, ID_REPORT, T(L"Request support for a device..."));
    AppendMenuW(m, MF_STRING, ID_SUPPORT, T(L"Support the project..."));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (updates::Busy() ? MF_GRAYED : 0), ID_UPDATE, updates::MenuLabel().c_str());
    UINT updateFlags = MF_STRING | (updates::AutomaticEnabled() ? MF_CHECKED : 0) |
                       (updates::Supported() ? 0 : MF_GRAYED);
    AppendMenuW(m, updateFlags, ID_AUTO_UPDATE, T(L"Automatically check for updates"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, T(L"Exit"));
    icons.Set(m, ID_REFRESH, false, 0xE72C);         // Refresh
    icons.Set(m, ID_REPORT, false, 0xE897);          // Help
    icons.Set(m, ID_SUPPORT, false, 0xEB51);         // Heart
    icons.Set(m, ID_UPDATE, false, 0xE777);          // UpdateRestore
    icons.Set(m, ID_EXIT, false, 0xE711);            // Cancel
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    // Opened from the taskbar, the menu must not overlap it: Windows only keeps a menu inside the monitor,
    // so its last items (Exit) ended up under the taskbar. Excluding the taskbar's rectangle makes Windows
    // open the menu above it (below / beside it for a top or side taskbar).
    TPMPARAMS tpm{ sizeof(tpm) };
    HWND under = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    wchar_t cls[32] = {};
    if (under) GetClassNameW(under, cls, ARRAYSIZE(cls));
    bool fromTaskbar = !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
    if (fromTaskbar) GetWindowRect(under, &tpm.rcExclude);
    bool sideBar = fromTaskbar && tpm.rcExclude.bottom - tpm.rcExclude.top > tpm.rcExclude.right - tpm.rcExclude.left;
    UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | (sideBar ? TPM_HORIZONTAL : TPM_VERTICAL) |
                                       (i18n::Rtl() ? TPM_LAYOUTRTL : 0),
                                pt.x, pt.y, g_hwnd, fromTaskbar ? &tpm : nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (russianFlag) DeleteObject(russianFlag);
    memory_trim::Soon();
    switch (cmd) {
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_KEEP_MOUSE_LAST_KNOWN: SetKeepMouseLastKnown(!g_keepMouseLastKnown); break;
        case ID_TRAY_WHEN_EMPTY: SetTrayWhenEmpty(!g_trayWhenEmpty); break;
        case ID_SHOW_DISCONNECTED: SetShowDisconnectedDevices(!g_showDisconnectedDevices); break;
        case ID_DIM_SLEEPING: SetDimSleeping(!g_dimSleeping); break;
        case ID_RECORD_HISTORY: g_recordHistory = !g_recordHistory; SaveRecordHistory(g_recordHistory); UpdateWidget(); break;
        case ID_LIMIT_OFF: SetChargeLimit(0); break;
        case ID_DASHBOARD:
            dashboard::Show(g_hwnd);
            {
                std::vector<DeviceStatus> live;
                for (auto& [k, e] : g_entries) live.push_back(e.st);
                dashboard::UpdateDevices(live, battery_history::GetStore());
            }
            break;
        case ID_CHARGED_ALERT: g_chargedAlert = !g_chargedAlert; SaveBoolSetting(L"ChargedAlert", g_chargedAlert); break;
        case ID_TIERED_ALERTS: g_tieredAlerts = !g_tieredAlerts; SaveBoolSetting(L"TieredAlerts", g_tieredAlerts); break;
        case ID_SHORT_RUNTIME_ALERT: g_shortRuntimeAlert = !g_shortRuntimeAlert; SaveBoolSetting(L"ShortRuntimeAlert", g_shortRuntimeAlert); break;
        case ID_HOVER_CARD:
        case ID_HOVER_CHART:
            if (cmd == ID_HOVER_CARD) { g_hoverCard = !g_hoverCard; SaveBoolSetting(L"HoverCard", g_hoverCard); }
            else { g_hoverChart = !g_hoverChart; SaveBoolSetting(L"HoverCardChart", g_hoverChart); }
            widget::SetCardOptions(g_hoverCard, g_hoverChart);
            UpdateWidget();
            break;
        case ID_TRACK_SINCE: SetTrackSince(!g_trackSince); break;
        case ID_TRACK_AWAKE: SetTrackAwake(!g_trackAwake); break;
        case ID_TRACK_EMPTY: SetTrackEmpty(!g_trackEmpty); break;
        case ID_ANIMATE_CHARGING:
            g_animateCharging = !g_animateCharging;
            SaveBoolSetting(L"AnimateCharging", g_animateCharging);
            widget::SetAnimateCharging(g_animateCharging);
            break;
        case ID_TRAY_DEVICE_ICONS:
            g_trayDeviceIcons = !g_trayDeviceIcons;
            SaveBoolSetting(L"TrayDeviceIcons", g_trayDeviceIcons);
            for (auto& [k, e] : g_entries) ApplyEntry(e);
            break;
        case ID_RUNTIME_WIDGET: g_runtimeWidget = !g_runtimeWidget; SaveRuntimeWidget(g_runtimeWidget); UpdateWidget(); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MONITOR_ALL: SetWidgetMonitor(L"all"); break;
        case ID_LANG_AUTO: SetLanguage(L""); break;
        case ID_LANG_ADD: i18n::OpenUserFolder(); break;
        case ID_MONITOR_MAIN: SetWidgetMonitor(L"main"); break;
        case ID_BRAND_COLORS: SetBrandColors(!g_brandColors); break;
        case ID_STACKED: SetStacked(!g_stacked); break;
        case ID_PERCENT_SIGN: SetShowPercentSign(!g_showPercentSign); break;
        case ID_RING_STYLE: SetRingStyle(!g_ringStyle); break;
        case ID_CONNECTION_ICONS: SetConnectionIcons(!g_connectionIcons); break;
        case ID_MAKE_ROOM: SetMakeRoom(!g_makeRoom); break;
        case ID_PLACE_AUTO: SetWidgetPlace(widget::VerticalPlace::Auto); break;
        case ID_PLACE_TOP: SetWidgetPlace(widget::VerticalPlace::Top); break;
        case ID_PLACE_TRAY: SetWidgetPlace(widget::VerticalPlace::AboveTray); break;
        case ID_PLACE_BOTTOM: SetWidgetPlace(widget::VerticalPlace::Bottom); break;
        case ID_FAR_LEFT:  // the two left spots exclude each other
            if (!g_farLeft && g_beforeApps) SetBeforeApps(false);
            SetFarLeft(!g_farLeft);
            break;
        case ID_BEFORE_APPS:
            if (!g_beforeApps && g_farLeft) SetFarLeft(false);
            SetBeforeApps(!g_beforeApps);
            break;
        case ID_MODE_WIDGET:
        case ID_MODE_MYFINDER:
            if (g_myFinder != (cmd == ID_MODE_MYFINDER)) {
                g_myFinder = cmd == ID_MODE_MYFINDER;
                SaveBoolSetting(L"MyFinderWidget", g_myFinder);
                widget::SetMyFinder(g_myFinder);
                g_useWidget = false;  // the host changed: Reconcile re-creates the strip and refreshes it
            }
            SetMode(true);
            break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_REPORT: report::Show(g_inst, g_hwnd); break;
        case ID_UPDATE: updates::Activate(g_hwnd); break;
        case ID_AUTO_UPDATE: updates::ToggleAutomatic(); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        default:
            if (cmd >= ID_POSITION_FIRST && cmd < ID_POSITION_FIRST + ARRAYSIZE(POSITION_OFFSETS)) {
                SetTaskbarOffset(POSITION_OFFSETS[cmd - ID_POSITION_FIRST]);
                break;
            }
            if (cmd == ID_POSITION_FIRST + ARRAYSIZE(POSITION_OFFSETS)) {
                int px = 0;
                if (PromptTaskbarOffset(g_hwnd, LoadTaskbarOffset(), px)) SetTaskbarOffset(px);
                break;
            }
            if (cmd >= ID_LIMIT_FIRST && cmd < ID_LIMIT_FIRST + (UINT)alerts::kChargeLimitCount) {
                SetChargeLimit(alerts::kChargeLimits[cmd - ID_LIMIT_FIRST]);
            } else if (cmd >= ID_POLL_FIRST && cmd < ID_POLL_FIRST + ARRAYSIZE(POLL_SECONDS)) {
                SetPollSeconds(POLL_SECONDS[cmd - ID_POLL_FIRST]);
            } else if (cmd >= ID_LANG_FIRST && cmd < ID_LANG_FIRST + languages.size()) {
                SetLanguage(languages[cmd - ID_LANG_FIRST].code);
            } else if (cmd >= ID_MONITOR_FIRST && cmd < ID_MONITOR_FIRST + monitors.size()) {
                SetWidgetMonitor(monitors[cmd - ID_MONITOR_FIRST].id);
            } else if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                switch ((cmd - ID_DEVICE_FIRST) % DEV_STRIDE) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                    case DEV_RENAME: RenameDevice(key); break;
                    case DEV_RESET_NAME: ResetDeviceName(key); break;
                    case DEV_ALERT_DEFAULT: SetAlertOverride(key, alerts::kUseDefault); break;
                    case DEV_ALERT_OFF: SetAlertOverride(key, alerts::kDisabled); break;
                    case DEV_DASHBOARD:
                        dashboard::Show(g_hwnd);
                        dashboard::SelectDevice(key);
                        break;
                    case DEV_HISTORY: {
                        auto it = g_entries.find(key);
                        if (it != g_entries.end())
                            battery_history::Show(g_hwnd, key, DisplayName(it->second));
                        break;
                    }
                    default: {
                        UINT slot = (cmd - ID_DEVICE_FIRST) % DEV_STRIDE;
                        if (slot >= DEV_ALERT_FIRST && slot < DEV_ALERT_FIRST + alerts::kThresholdCount)
                            SetAlertOverride(key, alerts::kThresholds[slot - DEV_ALERT_FIRST]);
                        break;
                    }
                }
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

// `--demo`: one sample device of every brand instead of real ones, for screenshots and for checking the
// brand colors without owning the hardware. Keys, vendor ids and names are mixed on purpose, so every way
// of recognising a brand gets used.
std::vector<DeviceStatus> DemoDevices() {
    auto make = [](const wchar_t* key, const wchar_t* name, DeviceKind kind, int pct, unsigned short vid, bool charging) {
        DeviceStatus s;
        s.key = key;
        s.name = name;
        s.kind = kind;
        s.percent = pct;
        s.vid = vid;
        s.charging = charging;
        s.via = L"demo";
        return s;
    };
    std::vector<DeviceStatus> devices = {
        make(L"razer:demo", L"Razer DeathAdder V3 Pro", DeviceKind::Mouse, 78, 0x1532, false),
        make(L"demo:logi", L"Logitech G PRO X 2 LIGHTSPEED", DeviceKind::Headset, 64, 0x046D, false),
        make(L"steelseries:demo", L"SteelSeries Arctis Nova 7", DeviceKind::Headset, 55, 0, false),
        make(L"hsc:1b1c:0a6b", L"Corsair HS80 RGB Wireless", DeviceKind::Headset, 81, 0x1B1C, false),
        make(L"hyperx:demo", L"HyperX Cloud III Wireless", DeviceKind::Headset, 47, 0, false),
        make(L"rog:demo", L"ROG Harpe Ace", DeviceKind::Mouse, 90, 0, false),
        make(L"zowie:demo", L"ZOWIE EC2-CW", DeviceKind::Mouse, 66, 0x04A5, false),
        make(L"bt:demo-ps", L"Wireless Controller", DeviceKind::Gamepad, 38, 0x054C, false),
        make(L"xbox:demo", L"Xbox Wireless Controller", DeviceKind::Gamepad, 72, 0x045E, false),
        make(L"bt:demo-buds", L"Galaxy Buds3 Pro", DeviceKind::Headset, 59, 0, false),
        make(L"airpods:demo", L"AirPods Pro", DeviceKind::Headset, 85, 0, false),
        make(L"jabra:demo", L"Jabra Evolve2 65", DeviceKind::Headset, 23, 0x0B0E, false),
        make(L"jbl:demo", L"JBL Quantum 910", DeviceKind::Headset, 70, 0x0ECB, false),
        make(L"coolermaster:demo", L"Cooler Master MM712", DeviceKind::Mouse, 12, 0x2516, false),
        make(L"steam:demo", L"Steam Controller", DeviceKind::Gamepad, 50, 0x28DE, true),
        make(L"bt:demo-plain", L"Bluetooth Headphones", DeviceKind::Headset, 100, 0, true),  // charging at 100%: fully charged
    };
    DeviceStatus offline = make(L"demo:offline-receiver", L"Offline wireless headset", DeviceKind::Headset, -1, 0, false);
    offline.via = L"2.4 GHz receiver";
    offline.connected = false;
    devices.push_back(offline);  // hidden by default, shown when the opt-in setting is enabled
    return devices;
}

// A closer check of the next polls (see refresh_policy.h), handed to the worker for its next poll.
SRWLOCK g_monitorLock = SRWLOCK_INIT;
refresh_policy::Mode g_monitorRequest = refresh_policy::Mode::Idle;
std::set<std::wstring> g_monitorKeys;
std::set<std::wstring> g_awakeBeforeSleep;
DWORD g_displayState = 1;  // GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed

// Bluetooth connections (GUID_BLUETOOTH_HCI_EVENT) are reported only to windows that registered a
// handle to the radio.
struct BluetoothRadioWatch {
    HANDLE radio = nullptr;
    HDEVNOTIFY notify = nullptr;
};
std::vector<BluetoothRadioWatch> g_btRadioWatches;

void ClearBluetoothRadioWatches() {
    for (auto& w : g_btRadioWatches) {
        if (w.notify) UnregisterDeviceNotification(w.notify);
        if (w.radio) CloseHandle(w.radio);
    }
    g_btRadioWatches.clear();
}

void DropBluetoothRadioWatch(HANDLE radio) {
    for (auto it = g_btRadioWatches.begin(); it != g_btRadioWatches.end(); ++it) {
        if (it->radio != radio) continue;
        if (it->notify) UnregisterDeviceNotification(it->notify);
        if (it->radio) CloseHandle(it->radio);
        g_btRadioWatches.erase(it);
        return;
    }
}

void RefreshBluetoothRadioWatches() {
    ClearBluetoothRadioWatches();
    if (!g_hwnd) return;
    BLUETOOTH_FIND_RADIO_PARAMS params{ sizeof(params) };
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) return;
    do {
        DEV_BROADCAST_HANDLE filter{ sizeof(filter) };
        filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
        filter.dbch_handle = radio;
        HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (notify) {
            g_btRadioWatches.push_back({ radio, notify });
        } else {
            CloseHandle(radio);
        }
        radio = nullptr;
    } while (BluetoothFindNextRadio(find, &radio));
    BluetoothFindRadioClose(find);
}

std::set<std::wstring> AwakeDevices() {
    std::set<std::wstring> keys;
    for (const auto& [k, e] : g_entries)
        if (e.st.connected && !e.st.asleep) keys.insert(k);
    return keys;
}

std::set<std::wstring> DevicesWithLevel() {
    std::set<std::wstring> keys;
    for (const auto& [k, e] : g_entries)
        if (e.st.connected && e.st.percent >= 0) keys.insert(k);
    return keys;
}

void RequestMonitor(refresh_policy::Mode mode, std::set<std::wstring> keys) {
    AcquireSRWLockExclusive(&g_monitorLock);
    if (mode != refresh_policy::Mode::Arrival || g_monitorRequest != refresh_policy::Mode::Resume) {
        g_monitorRequest = mode;
        g_monitorKeys = std::move(keys);
    }
    ReleaseSRWLockExclusive(&g_monitorLock);
}

// A receiver busy with input reports can miss a single status query. One quick re-read is much cheaper
// than dimming the readout for a whole interval, so a poll with a device missing or without a level is
// repeated once and both results are merged, the fresher reading winning (PR #303).
std::vector<DeviceStatus> PollWithRetry() {
    std::vector<DeviceStatus> first = PollAllDevices();
    bool incomplete = first.empty();
    for (const auto& s : first)
        if (s.connected && !s.asleep && s.percent < 0 && s.levelText.empty()) incomplete = true;
    long known = g_lastCount.load();
    if (known > 0 && (long)first.size() < known) incomplete = true;
    if (!incomplete || (first.empty() && known == 0)) {
        g_lastCount = (long)first.size();
        return first;
    }
    if (WaitForSingleObject(g_stopEvt, 250) == WAIT_OBJECT_0) return first;
    std::vector<DeviceStatus> second = PollAllDevices();
    std::set<std::wstring> keys;
    for (const auto& s : second) keys.insert(s.key);
    for (const auto& s : first) {
        if (keys.count(s.key)) continue;
        second.push_back(s);
        keys.insert(s.key);
    }
    g_lastCount = (long)second.size();
    return second;
}

// --trace: one line per poll, so "the readout blinks" reports can be checked against what the
// receivers actually answered.
void TracePoll(const std::vector<DeviceStatus>& v) {
    if (!g_trace) return;
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    wcscat_s(path, L"wirelessbattery-trace.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%02d:%02d:%02d.%03d  poll: %zu device(s)", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, v.size());
    for (const auto& s : v) fwprintf(f, L"  [%ls %d%%%ls]", s.key.c_str(), s.percent, s.asleep ? L" asleep" : L"");
    fwprintf(f, L"\n");
    fclose(f);
}

DWORD WINAPI Worker(LPVOID) {
    refresh_policy::Monitor monitor;
    monitor.BeginStartup();
    HANDLE events[2] = { g_stopEvt, g_refreshEvt };
    for (;;) {
        AcquireSRWLockExclusive(&g_monitorLock);
        refresh_policy::Mode request = std::exchange(g_monitorRequest, refresh_policy::Mode::Idle);
        std::set<std::wstring> keys = std::move(g_monitorKeys);
        g_monitorKeys.clear();
        ReleaseSRWLockExclusive(&g_monitorLock);
        if (request == refresh_policy::Mode::Resume) monitor.BeginResume(std::move(keys));
        else if (request == refresh_policy::Mode::Arrival) monitor.BeginArrival(std::move(keys));

        auto* v = new std::vector<DeviceStatus>(g_demo ? DemoDevices() : PollWithRetry());
        TracePoll(*v);
        DWORD delay = monitor.DelayAfterPoll(*v, g_pollIntervalMs);
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, delay);
        if (wait == WAIT_OBJECT_0) return 0;
        monitor.OnWaitFinished(wait == WAIT_TIMEOUT);
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }
void RefreshAfterWake(UINT ms) { SetTimer(g_hwnd, TIMER_RESUME, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        g_alertIconAdded = false;
        KillTimer(g_hwnd, TIMER_ALERT_ICON);
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE: {
            auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
            bool handle = hdr && hdr->dbch_devicetype == DBT_DEVTYP_HANDLE;
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                    auto* iface = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lp);
                    if (iface->dbcc_classguid == GUID_BTHPORT_DEVICE_INTERFACE) RefreshBluetoothRadioWatches();
                    else if (wp == DBT_DEVICEARRIVAL) RequestMonitor(refresh_policy::Mode::Arrival, DevicesWithLevel());
                }
                if (handle && wp == DBT_DEVICEREMOVECOMPLETE)
                    DropBluetoothRadioWatch(reinterpret_cast<const DEV_BROADCAST_HANDLE*>(lp)->dbch_handle);
                RefreshSoon(2500);
                for (auto& src : g_sources) src->DevicesChanged();
            } else if (handle && (wp == DBT_DEVICEQUERYREMOVE || wp == DBT_DEVICEREMOVEPENDING)) {
                // Let the radio go: an open handle would block its removal.
                DropBluetoothRadioWatch(reinterpret_cast<const DEV_BROADCAST_HANDLE*>(lp)->dbch_handle);
            } else if (handle && wp == DBT_DEVICEQUERYREMOVEFAILED) {
                RefreshBluetoothRadioWatches();
            } else if (handle && wp == DBT_CUSTOMEVENT) {
                auto* bh = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(lp);
                // HCI_CONNECTION_TYPE_*: 1 classic (ACL), 2 the voice link of a call (SCO), 3 Bluetooth LE.
                constexpr UCHAR kHciAcl = 1, kHciSco = 2;
                BTH_HCI_EVENT_INFO info{};
                if (bh->dbch_eventguid == GUID_BLUETOOTH_HCI_EVENT &&
                    bh->dbch_size >= offsetof(DEV_BROADCAST_HANDLE, dbch_data) + sizeof(info)) {
                    memcpy(&info, bh->dbch_data, sizeof(info));
                    if (info.connectionType != kHciSco) {
                        // A Bluetooth headset reports its level a few seconds after it connects.
                        if (info.connected && info.connectionType == kHciAcl)
                            RequestMonitor(refresh_policy::Mode::Arrival, DevicesWithLevel());
                        RefreshSoon(2500);
                    }
                }
            }
            return TRUE;
        }
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) {
                g_awakeBeforeSleep = AwakeDevices();
            } else if (wp == PBT_APMRESUMEAUTOMATIC) {
                RefreshAfterWake(5000);
                for (auto& src : g_sources) src->DevicesChanged();  // handles go stale over sleep
            } else if (wp == PBT_POWERSETTINGCHANGE && lp) {
                // Modern Standby may not report a suspend / resume: the display turning off and on stands in.
                auto* ps = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                if (ps->PowerSetting == kConsoleDisplayStateGuid && ps->DataLength >= sizeof(DWORD)) {
                    DWORD state = *reinterpret_cast<const DWORD*>(ps->Data);
                    if (g_displayState != 0 && state == 0) g_awakeBeforeSleep = AwakeDevices();
                    else if (g_displayState == 0 && state != 0) RefreshAfterWake(3000);
                    g_displayState = state;
                }
            }
            return TRUE;
        case WM_DEVICE_EVENT:
            RefreshSoon((UINT)lp);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_RESUME) {
                KillTimer(h, TIMER_RESUME);
                RequestMonitor(refresh_policy::Mode::Resume, std::move(g_awakeBeforeSleep));
                g_awakeBeforeSleep.clear();
                SetEvent(g_refreshEvt);
            }
            if (wp == TIMER_WIDGET) { Reconcile(); updates::Tick(h); }
            if (wp == TIMER_ALERT_ICON) {
                KillTimer(h, TIMER_ALERT_ICON);
                if (g_alertIconAdded) { TrayDelete(ALERT_ICON_ID); g_alertIconAdded = false; }
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); RedrawAll(); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            updates::Shutdown();
            KillTimer(h, TIMER_DEBOUNCE);
            KillTimer(h, TIMER_RESUME);
            KillTimer(h, TIMER_WIDGET);
            KillTimer(h, TIMER_ALERT_ICON);
            if (g_alertIconAdded) { TrayDelete(ALERT_ICON_ID); g_alertIconAdded = false; }
            widget::Destroy();
            dashboard::Close();
            battery_intelligence::Shutdown();
            for (auto& src : g_sources) src->Stop();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            ShutdownDevices();
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            DeleteFileW(StatusSnapshotPath().c_str());  // so external tools see the app is gone right away
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `BatteryHub.exe --list` prints what it finds, `--dump` every HID collection.
// What `--text` prints (PR #369): the taskbar widget's content as one line, for status bars such as YASB.
// Same order, hidden devices and low-battery rule as the widget (settings are read from the registry).
// A one-shot run has no memory of earlier values, so a device that is asleep right now shows "?".
std::wstring ListAsText(const std::vector<DeviceStatus>& list) {
    g_hideLastKnown = LoadHideLastKnown();
    LoadHidden();
    LoadOrder();
    g_entries.clear();
    for (const auto& s : list) g_entries[s.key].st = s;
    std::vector<WidgetItem> items;
    for (const TrayEntry* e : OrderedEntries(true)) {
        WidgetItem it;
        it.kind = e->st.kind;
        it.percent = e->st.connected ? e->st.percent : -1;
        if (e->st.percent >= 0) it.text = e->st.levelText;
        it.charging = e->st.connected && !e->st.asleep && e->st.charging;
        it.dim = ShouldDim(e->st);
        it.low = !it.charging && it.percent >= 0 && it.percent <= LOW_BATTERY;
        it.bluetooth = e->st.via == L"Bluetooth";
        items.push_back(it);
    }
    return widget::ItemsToText(items);
}

int ListToConsole(bool dump, bool text = false) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (dump) {
        // Also saved to a file: PowerShell does not wait for this windowed exe, so console output can end up
        // after the prompt or get lost, and a double-click has no console at all.
        std::wstring text = DumpHidCollections();
        wchar_t path[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, path);
        wcscat_s(path, L"BatteryHub-dump.txt");
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::wstring crlf;
            for (wchar_t ch : text) crlf += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch);
            int n = WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), nullptr, 0, nullptr, nullptr);
            std::string u = "\xEF\xBB\xBF" + std::string(n, 0);  // UTF-8 byte order mark, so Notepad shows it right
            WideCharToMultiByte(CP_UTF8, 0, crlf.c_str(), (int)crlf.size(), u.data() + 3, n, nullptr, nullptr);
            DWORD w;
            WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
            CloseHandle(f);
        }
        if (out && out != INVALID_HANDLE_VALUE) {
            write(text);
            write(std::wstring(L"\nAlso saved to ") + path + L"\n");
        } else {
            ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);  // no console: show the file
        }
        return 0;
    }
    auto list = PollAllDevices();
    list.erase(std::remove_if(list.begin(), list.end(), [](const DeviceStatus& s) { return s.unsupported; }), list.end());  // menu-only entries
    if (text) {
        std::wstring line = ListAsText(list);
        if (!line.empty()) write(line + L"\n");
        return 0;
    }
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (auto& s : list) {
        Accent accent = BrandOf(s);
        std::wstring line = s.name + L"  [" + s.via + L"]  " +
                            (accent != Accent::None ? L"{" + std::wstring(StyleOf(accent).name) + L"}  " : L"");
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.full || (s.charging && s.percent >= 100)) line += L", fully charged";
        else if (s.charging) line += L", charging";
        if (!s.note.empty() && s.percent < 0) line += L", " + s.note;
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        write(line + L"   (" + s.key + L")\n");
    }
    ShutdownDevices();
    return 0;
}

// Machine-readable device list, shared with the Linux entry point (src/json_output.h).
int ListJson() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto devices = PollAllDevices();
    devices.erase(std::remove_if(devices.begin(), devices.end(), [](const DeviceStatus& s) { return s.unsupported; }),
                  devices.end());  // menu-only entries
    std::string text = DevicesToJson(devices);  // UTF-8
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(out, text.data(), (DWORD)text.size(), &written, nullptr);
    }
    ShutdownDevices();
    return 0;
}

}  // namespace

// `--events [seconds]` prints the push events the listeners see (default 60 s), so a contributor can
// check that a device sends them before relying on them (PR #535).
int EventsToConsole(const wchar_t* args) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [out](const std::wstring& s) {
        std::wstring line = s + L"\n";
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };

    // Logitech receivers send connection events only while their "wireless" notification flag is set.
    // Read it (a register read, like the battery queries; never written) so the output says why nothing arrives.
    for (const HidColl& c : EnumHid()) {
        if (c.vid != 0x046D || c.page() != 0xFF00 || c.usage() != 0x0001 || c.outLen() != 7) continue;
        HidDev d;
        if (!d.Open(c)) continue;
        BYTE req[7];
        hidpp::NotificationFlagsRequest(req);
        d.Flush();
        std::wstring state = L"no answer (not a receiver?)";
        if (d.Write(std::vector<BYTE>(req, req + 7))) {
            std::vector<BYTE> r;
            for (ULONGLONG end = GetTickCount64() + 500; GetTickCount64() < end;) {
                std::uint32_t flags = 0;
                if (d.Read(r, 50) > 0 && hidpp::ParseNotificationFlags(r.data(), r.size(), flags)) {
                    state = (flags & hidpp::kWirelessNotifications)
                                ? L"on"
                                : L"OFF (connection events need G HUB, Options+ or Solaar to switch them on)";
                    break;
                }
            }
        }
        write(L"receiver " + Hex4(c.vid) + L":" + Hex4(c.pid) + L" wireless notifications: " + state);
    }

    // The same listener the app runs, with a sink that prints instead of polling.
    struct ConsoleSink final : IRefreshSink {
        std::function<void(const std::wstring&)> write;
        void RequestRefresh(std::uint32_t delayMs) override {
            write(L"  => refresh requested (in " + std::to_wstring(delayMs) + L" ms)");
        }
    } sink;
    sink.write = write;
    HidEventListener listener(HidEventDecoders(), write);
    if (!listener.Start(sink)) return 1;
    int seconds = (int)wcstol(wcsstr(args, L"--events") + 8, nullptr, 10);
    if (seconds <= 0) seconds = 60;
    write(L"listening for " + std::to_wstring(seconds) + L" s: switch a wireless device off and on...");
    Sleep((DWORD)seconds * 1000);
    listener.Stop();
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--events")) return EventsToConsole(cmdLine);
    if (cmdLine && wcsstr(cmdLine, L"--json")) return ListJson();
    if (cmdLine && wcsstr(cmdLine, L"--help")) {
        MessageBoxW(nullptr,
                    L"BatteryHub options:\n\n"
                    L"--list   Print devices and battery levels\n"
                    L"--json   Print a JSON array of devices and battery levels\n"
                    L"--dump   Print every HID collection and device\n"
                    L"--events Print push events from wireless receivers (60 s)\n"
                    L"--report Open the \"Request support for a device\" window\n"
                    L"--demo   Show sample devices\n"
                    L"--trace  Log every poll to %TEMP%\\wirelessbattery-trace.log\n"
                    L"--quit   Quit a running instance",
                    L"BatteryHub", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int applyResult = argv ? updates::RunApplyMode(argc, argv) : -1;
    if (argv) LocalFree(argv);
    if (applyResult >= 0) return applyResult;
    if (cmdLine && wcsstr(cmdLine, L"--list")) return ListToConsole(false);
    if (cmdLine && wcsstr(cmdLine, L"--text")) return ListToConsole(false, true);
    if (cmdLine && wcsstr(cmdLine, L"--dump")) return ListToConsole(true);
    if (cmdLine && wcsstr(cmdLine, L"--report")) {  // the "Request support for a device" window on its own
        i18n::Init();
        report::Show(inst, nullptr);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (report::PreTranslate(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            HANDLE running = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
            if (!running) return 1;
            PostMessageW(w, WM_CLOSE, 0, 0);
            DWORD exited = WaitForSingleObject(running, 30000);
            CloseHandle(running);
            if (exited != WAIT_OBJECT_0) return 1;
        }
        return 0;
    }
    g_inst = inst;
    g_demo = cmdLine && wcsstr(cmdLine, L"--demo");
    g_trace = cmdLine && wcsstr(cmdLine, L"--trace");

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    updates::Initialize();

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    // Bluetooth radios coming and going, and the connections on each one.
    filter.dbcc_classguid = GUID_BTHPORT_DEVICE_INTERFACE;
    HDEVNOTIFY radioNotify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    RefreshBluetoothRadioWatches();
    HPOWERNOTIFY displayNotify =
        RegisterPowerSettingNotification(g_hwnd, &kConsoleDisplayStateGuid, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_myFinder = LoadBoolSetting(L"MyFinderWidget", false);
    widget::SetMyFinder(g_myFinder);
    g_hideLastKnown = LoadHideLastKnown();
    g_keepMouseLastKnown = LoadBoolSetting(L"KeepMouseLastKnown", false);
    g_trayWhenEmpty = LoadTrayWhenEmpty();
    g_showDisconnectedDevices = LoadShowDisconnectedDevices();
    g_dimSleeping = LoadDimSleeping();
    g_recordHistory = LoadRecordHistory();
    g_chargeLimit = LoadChargeLimit();
    g_shortRuntimeAlert = LoadBoolSetting(L"ShortRuntimeAlert", true);
    g_chargedAlert = LoadBoolSetting(L"ChargedAlert", true);
    g_tieredAlerts = LoadBoolSetting(L"TieredAlerts", true);
    g_hoverCard = LoadBoolSetting(L"HoverCard", true);
    g_hoverChart = LoadBoolSetting(L"HoverCardChart", true);
    widget::SetCardOptions(g_hoverCard, g_hoverChart);
    g_trackSince = LoadTrackSince();
    g_trackAwake = LoadTrackAwake();
    g_trackEmpty = LoadTrackEmpty();
    g_runtimeWidget = LoadRuntimeWidget();
    g_trayDeviceIcons = LoadBoolSetting(L"TrayDeviceIcons", false);
    g_animateCharging = LoadBoolSetting(L"AnimateCharging", false);
    widget::SetAnimateCharging(g_animateCharging);
    g_widgetBackground = LoadWidgetBackground();
    g_brandColors = LoadBrandColors();
    g_stacked = LoadStacked();
    g_makeRoom = LoadMakeRoom();
    widget::SetMakeRoom(g_makeRoom);
    g_farLeft = LoadFarLeft();
    g_pollIntervalMs = LoadPollSeconds() * 1000;
    widget::SetStacked(g_stacked);
    g_showPercentSign = LoadShowPercentSign();
    widget::SetShowPercentSign(g_showPercentSign);
    g_ringStyle = LoadRingStyle();
    widget::SetRingStyle(g_ringStyle);
    g_connectionIcons = LoadConnectionIcons();
    widget::SetConnectionIcons(g_connectionIcons);
    g_verticalPlace = LoadVerticalPlace();
    widget::SetVerticalPlace(g_verticalPlace);
    widget::SetFarLeft(g_farLeft);
    g_beforeApps = LoadBeforeApps() && !g_farLeft;
    widget::SetBeforeApps(g_beforeApps);
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    i18n::Init();
    g_widgetMonitor = LoadWidgetMonitor();
    widget::SetMonitorChoice(g_widgetMonitor);
    LoadHidden();
    LoadOrder();
    LoadAliases();
    LoadAlertThresholds();
    battery_history::Initialize(g_demo);
    battery_intelligence::Initialize(g_demo);
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!g_demo) {
        g_sources.push_back(std::make_unique<HidEventListener>(HidEventDecoders()));
        for (auto& src : g_sources) src->Start(g_refreshSink);
    }

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (report::PreTranslate(&m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (radioNotify) UnregisterDeviceNotification(radioNotify);
    ClearBluetoothRadioWatches();
    if (displayNotify) UnregisterPowerSettingNotification(displayNotify);
    if (g_worker) CloseHandle(g_worker);
    ReleaseMutex(mutex);
    return 0;
}
