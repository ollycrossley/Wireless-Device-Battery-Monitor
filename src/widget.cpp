#include "widget.h"
#include "myfinder_layout.h"
#include "../native/composition_surface.h"
#include "../native/finder_order_guard.h"

#include "i18n.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>
#include <climits>
#include <cmath>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
bool g_cardOn = true, g_chartOn = true;  // hover card and its 24-hour chart (menu options); card off = plain tooltip
UINT g_clickMsg;
bool g_light, g_hover;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
bool g_stacked;     // icon above the percentage (menu option, PR #525)
bool g_showPercentSign = true;  // '%' after numeric levels (menu option, PR #1386)
bool g_ringStyle;   // device icon inside a ring that fills with the charge (menu option, PR #110)
bool g_animateCharging;  // charging icons fill green in a loop (menu option, PR #2463)
bool g_connectionIcons;  // wireless mark after 2.4 GHz devices' levels (menu option, PR #2277)
bool g_vertical;    // the primary taskbar is docked left or right: one block per device, stacked (PR #321)
bool g_makeRoom = true;  // step past other apps' widgets in the taskbar instead of covering them
widget::VerticalPlace g_place = widget::VerticalPlace::Auto;  // where the column sits on such a taskbar (menu option)
bool g_farLeft;     // at the left end of the taskbar instead of next to the clock (menu option)
bool g_beforeApps;  // just left of Start / the first task button, centred taskbar (menu option, PR #760)
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
// Drag to move (PR #124): hold the left button on the strip and slide it along the taskbar. The drop
// is saved as TaskbarOffset (DPI-independent px left of the tray), the value the menu also sets.
int g_anchorLeft = -1;   // where the strip goes with no offset (taskbar client x), from the last Reposition
bool g_pressed = false, g_dragging = false;
POINT g_pressAt{};
int g_grabDx = 0;
SIZE g_size{};
SIZE g_paintedBarClient{};
bool g_painted = false;
HINSTANCE g_inst;
// MyDockFinder host (experimental, PR #251): the strip is a separate composited popup that follows
// the first MyFinder bar, left of its system indicators.
bool g_myFinder = false;
CompositionSurface g_composition;
FinderOrderGuard g_orderGuard;
bool g_compositionReady = false;
ULONGLONG g_lastValidFinderPlacement = 0;

// Copies of the strip on secondary-monitor taskbars (Shell_SecondaryTrayWnd), so the levels stay visible
// while a game runs fullscreen on the main screen. Same drawing, same click and tooltip; each one sits
// left of its taskbar's clock. The primary strip above is unchanged, and with one monitor none exist.
struct Mirror {
    HWND bar = nullptr, wnd = nullptr;
    bool hover = false;
    bool painted = false;
    SIZE size{};
    int dpi = 0;
    int lastLeft = INT_MIN;  // client x it was last placed at; INT_MIN = not placed (hidden)
    std::vector<RECT> itemRects;  // each device's area in this strip (for the hover card)
};
std::vector<Mirror> g_mirrors;
// Which taskbars show the strip: "all", "main", or one monitor's id (MonitorId), e.g. only monitor 3.
// With one monitor chosen, the primary strip is hidden; if that monitor is not connected, the primary
// strip is shown instead so the levels never disappear.
std::wstring g_choice = L"all";
bool g_primaryHidden = false;
HWND g_dpiBar;  // while a mirror is painted or placed, Px() uses its taskbar's DPI

const wchar_t* kClass = L"WirelessBatteryWidget";
const wchar_t* kBridgeClass = L"Windows.UI.Composition.DesktopWindowContentBridge";

// Ask MyFinder to render its own bar, excluding our separate popup. No desktop pixels, files or
// network are involved. Keep the potentially blocking PrintWindow call off our UI thread.
SRWLOCK g_finderLock = SRWLOCK_INIT;
HWND g_finderHost = nullptr, g_finderResultHost = nullptr;
RECT g_finderRect{};
int g_finderEdge = -1;
myfinder::Foreground g_finderForeground = myfinder::Foreground::Unknown;
ULONGLONG g_finderUpdated = 0;
HANDLE g_finderWake = nullptr;
bool FinderAboveStrip() {
    for (HWND window = GetWindow(g_wnd, GW_HWNDPREV); window; window = GetWindow(window, GW_HWNDPREV))
        if (window == g_taskbar) return true;
    return false;
}

HWINEVENTHOOK g_finderOrderHook = nullptr, g_finderForegroundHook = nullptr;
bool g_finderOrderQueued = false;

void CALLBACK FinderOrderEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (!g_myFinder || !g_wnd || !g_taskbar || g_finderOrderQueued || !FinderAboveStrip()) return;
    g_finderOrderQueued = true;
    HWND above = GetWindow(g_taskbar, GW_HWNDPREV);
    const bool top = (GetWindowLongPtrW(g_taskbar, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (!above || ((GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != top)
        above = top ? HWND_TOPMOST : HWND_TOP;
    SetWindowPos(g_wnd, above, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    g_finderOrderQueued = false;
}

DWORD WINAPI FinderLayoutThread(LPVOID) {
    DWORD interval = 350;
    ULONGLONG activeUntil = 0;
    for (;;) {
        WaitForSingleObject(g_finderWake, interval);
        interval = 350;
        AcquireSRWLockShared(&g_finderLock);
        HWND host = g_finderHost;
        ReleaseSRWLockShared(&g_finderLock);
        if (!host || !IsWindow(host) || !IsWindowVisible(host)) continue;
        RECT rect{};
        if (!GetWindowRect(host, &rect)) continue;
        POINT cursor{};
        const auto now = GetTickCount64();
        if (GetCursorPos(&cursor) && PtInRect(&rect, cursor)) activeUntil = now + 1000;
        if (now < activeUntil) interval = 100;
        int w = rect.right - rect.left, h = rect.bottom - rect.top;
        if (w < 8 || w > 16384 || h < 4 || h > 256) continue;
        int edge = -1;
        auto foreground = myfinder::Foreground::Unknown;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bitmap = mem ? CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (bitmap && bits) {
            HGDIOBJ old = SelectObject(mem, bitmap);
            memset(bits, 0, (size_t)w * h * 4);
            if (PrintWindow(host, mem, 2 /* PW_RENDERFULLCONTENT */)) {
                GdiFlush();
                edge = myfinder::GroupLeft(static_cast<const std::uint32_t*>(bits), w, h, std::max(24, h * 2));
                foreground = myfinder::GlyphForeground(static_cast<const std::uint32_t*>(bits), w, h, edge);
            }
            SelectObject(mem, old);
        }
        if (bitmap) DeleteObject(bitmap);
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        AcquireSRWLockExclusive(&g_finderLock);
        // A menu/transition can temporarily produce an empty or failed rendering.
        // Keep the last good layout until its existing 3.5-second freshness limit;
        // a failed sample must neither erase it immediately nor refresh its age.
        if (g_finderHost == host && edge >= 0) {
            g_finderResultHost = host;
            g_finderRect = rect;
            g_finderEdge = edge;
            if (foreground != myfinder::Foreground::Unknown) g_finderForeground = foreground;
            g_finderUpdated = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&g_finderLock);
    }
}

void WatchFinder(HWND host) {
    AcquireSRWLockExclusive(&g_finderLock);
    bool changed = host != g_finderHost;
    g_finderHost = host;
    if (changed) {
        g_finderResultHost = nullptr;
        g_finderEdge = -1;
        g_finderForeground = myfinder::Foreground::Unknown;
        g_finderUpdated = 0;
    }
    ReleaseSRWLockExclusive(&g_finderLock);
    if (host && !g_finderWake) {
        g_finderWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE thread = g_finderWake ? CreateThread(nullptr, 0, FinderLayoutThread, nullptr, 0, nullptr) : nullptr;
        if (thread) CloseHandle(thread);
    }
    if (changed && g_finderWake) SetEvent(g_finderWake);
}

int FinderEdge(HWND host, const RECT& rect) {
    AcquireSRWLockShared(&g_finderLock);
    int edge = g_finderResultHost == host && EqualRect(&rect, &g_finderRect) &&
               (GetTickCount64() - g_finderUpdated < 3500 || IsWindowVisible(g_wnd)) ? g_finderEdge : -1;
    ReleaseSRWLockShared(&g_finderLock);
    return edge;
}
myfinder::Foreground FinderForeground(HWND host) {
    AcquireSRWLockShared(&g_finderLock);
    auto value = g_finderResultHost == host ? g_finderForeground : myfinder::Foreground::Unknown;
    ReleaseSRWLockShared(&g_finderLock);
    return value;
}

int Dpi() {
    RECT finderRect{};  // MyFinder: scale with the bar height (24 px at 100%)
    if (g_myFinder && g_taskbar && GetClientRect(g_taskbar, &finderRect) && finderRect.bottom > 0)
        return MulDiv(std::clamp((int)finderRect.bottom, 8, 128), 96, 24);
    HWND bar = g_dpiBar ? g_dpiBar : g_taskbar;
    UINT d = bar ? GetDpiForWindow(bar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }

// Some taskbar customizations report tray geometry that places the strip off-screen (#2993).
// A plain clamp to x=0 is insufficient: the Start/task buttons may cover a child widget there.
// Preserve an earlier valid position; otherwise estimate the normal slot left of the tray.
constexpr int RecoverHorizontalWidgetX(int desired, int widgetWidth, int barWidth,
                                       int trayWidth, int lastGoodX, int gap) {
    const int rightmost = std::max(0, barWidth - widgetWidth);
    if (desired >= 0 && desired <= rightmost) return desired;  // don't touch healthy positioning
    if (lastGoodX >= 0 && lastGoodX <= rightmost) return lastGoodX;
    if (trayWidth > 0 && trayWidth < barWidth) {
        // With invalid tray coordinates, assume the tray is anchored to the taskbar's outer edge.
        return std::clamp(barWidth - trayWidth - widgetWidth - gap, 0, rightmost);
    }
    return std::clamp(desired, 0, rightmost);  // final fallback when tray metrics are unusable
}
static_assert(RecoverHorizontalWidgetX(-61, 59, 1707, 220, 1400, 2) == 1400); // keep valid position
static_assert(RecoverHorizontalWidgetX(-61, 59, 1707, 220, -1, 2) == 1426);   // fresh startup
static_assert(RecoverHorizontalWidgetX(1240, 80, 1707, 220, 900, 2) == 1240);// normal unchanged
static_assert(RecoverHorizontalWidgetX(1720, 80, 1707, 220, -1, 2) == 1405); // right overflow
static_assert(RecoverHorizontalWidgetX(-50, 120, 100, 0, -1, 2) == 0);        // too wide

// Windows 11, left / right taskbar, "Bottom" place: the taskbar's XAML content (task buttons and tray) lives
// in one child window that fills the bar. Making that window `reserve` pixels shorter moves all of its
// content up by as much and leaves the bottom of the bar free for the column, so nothing is covered.
// Explorer gives the window its full height back on its own layout passes, so Reposition() re-applies
// this; 0 restores it. Only a window we shortened ourselves is ever restored.
bool g_reserved;
// The freed strip shows the bare taskbar window, which is not the colour the taskbar's content is drawn
// in (acrylic, or whatever a taskbar theme sets), so the column would sit on a visible band. While the
// bottom place is used, the column is filled with the colour found just above it instead.
bool g_fill;
COLORREF g_fillColor;

// Reads the taskbar's colour from three points on the row just above `screenTop`. Fails when they differ
// (text or an icon is there, e.g. before the content has moved up), so a glyph's colour is never taken.
bool SampleBarColor(const RECT& bar, int screenTop, COLORREF& out) {
    HDC dc = GetDC(nullptr);
    int w = bar.right - bar.left, y = screenTop - Px(4);
    COLORREF c[3];
    for (int i = 0; i < 3; i++) c[i] = GetPixel(dc, bar.left + w * (i + 1) / 4, y);
    ReleaseDC(nullptr, dc);
    auto differs = [](COLORREF a, COLORREF b, int tol) {
        return abs(GetRValue(a) - GetRValue(b)) > tol || abs(GetGValue(a) - GetGValue(b)) > tol ||
               abs(GetBValue(a) - GetBValue(b)) > tol;
    };
    for (COLORREF v : c)
        if (v == CLR_INVALID || differs(v, c[1], 10)) return false;
    out = c[1];
    return true;
}

void ReserveBottom(int reserve) {
    HWND bridge = g_taskbar && IsWindow(g_taskbar) ? FindWindowExW(g_taskbar, nullptr, kBridgeClass, nullptr) : nullptr;
    if (!bridge || (!reserve && !g_reserved)) { g_reserved = false; return; }
    RECT client{}, br{};
    GetClientRect(g_taskbar, &client);
    GetWindowRect(bridge, &br);
    int want = client.bottom - reserve;
    if (want > 0 && br.bottom - br.top != want)
        SetWindowPos(bridge, nullptr, 0, 0, br.right - br.left, want, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_reserved = reserve > 0;
}

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on BatteryHub.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

// Where a Windows 11 secondary taskbar's clock, widgets button and task buttons are. Its XAML has no child
// windows for them, so the same lookup thread reads them with UI Automation (the clock has AutomationId
// "SystemTrayIcon", widgets button "WidgetsButton", task buttons class "Taskbar.TaskListButtonAutomationPeer";
// found by PR #107).
// On a vertical (left / right) taskbar "left/right" below read as "top/bottom": positions run along the bar.
struct Span {
    HWND bar = nullptr;
    bool measured = false;   // UI Automation answered for this taskbar
    bool hasClock = false;   // the clock can be turned off on secondary taskbars
    int clockLeft = 0;       // screen x (y on a vertical bar)
    bool hasWidgets = false; // Widgets (weather) button on secondary taskbar
    RECT widgets{};          // bounding rectangle of WidgetsButton (screen coords)
    bool hasTasks = false;
    int tasksRight = 0;      // right (bottom) edge of the last task button before the clock (screen x / y)
    bool hasFirst = false;
    int firstLeft = 0;       // left edge of the leftmost Start or task button (screen x), for the far-left spot
    // Right-to-left taskbars (Arabic, Hebrew; from PR #2853): the clock sits at the left end, so the strip
    // goes right of it. clockRight is the clock area's right edge, rtlTasksLeft the left edge of the
    // nearest task button to its right (horizontal bars only).
    bool rtl = false;
    int clockRight = 0;
    bool hasRtlTasks = false;
    int rtlTasksLeft = 0;
    RECT bounds{};
    UINT dpi = 0;
    ULONGLONG sampledAt = 0;
    bool leftMeasured = false;
    bool hasStart = false;
    LONG trayLeft = 0;
    std::vector<RECT> occupied;
};

bool IsVertical(const RECT& r) { return r.bottom - r.top > r.right - r.left; }
// Start and end of a rectangle along a taskbar's length: x for a horizontal bar, y for a vertical one.
int Lo(const RECT& r, bool vertical) { return vertical ? r.top : r.left; }
int Hi(const RECT& r, bool vertical) { return vertical ? r.bottom : r.right; }
std::vector<Span> g_spans;   // guarded by g_wbLock
std::vector<HWND> g_wbMirrors;  // guarded by g_wbLock: selected secondary taskbars
bool g_wbLeftSpot;            // guarded by g_wbLock
bool g_wbPrimaryVisible = true;  // guarded by g_wbLock
unsigned g_wbGeneration;      // guarded by g_wbLock

IUIAutomationCondition* StringCondition(IUIAutomation* uia, PROPERTYID prop, const wchar_t* value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    IUIAutomationCondition* c = nullptr;
    if (v.bstrVal) uia->CreatePropertyCondition(prop, v, &c);
    VariantClear(&v);
    return c;
}

bool IsXamlTaskbar(HWND bar) {
    return FindWindowExW(bar, nullptr, kBridgeClass, nullptr) != nullptr;
}

IUIAutomationCondition* SpanFilter(IUIAutomation* uia) {
    constexpr CONTROLTYPEID types[]{
        UIA_ButtonControlTypeId, UIA_SplitButtonControlTypeId, UIA_MenuItemControlTypeId, UIA_CheckBoxControlTypeId,
        UIA_RadioButtonControlTypeId, UIA_ListItemControlTypeId, UIA_TabItemControlTypeId, UIA_EditControlTypeId,
        UIA_ComboBoxControlTypeId, UIA_HyperlinkControlTypeId,
    };
    constexpr size_t namedCount = 4;
    IUIAutomationCondition* conditions[namedCount + ARRAYSIZE(types)]{
        StringCondition(uia, UIA_AutomationIdPropertyId, L"StartButton"),
        StringCondition(uia, UIA_AutomationIdPropertyId, L"WidgetsButton"),
        StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon"),
        StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer"),
    };
    VARIANT type{};
    type.vt = VT_I4;
    for (size_t i = 0; i < ARRAYSIZE(types); ++i) {
        type.lVal = types[i];
        uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &conditions[namedCount + i]);
    }
    IUIAutomationCondition* filter = nullptr;
    if (std::all_of(std::begin(conditions), std::end(conditions), [](auto* condition) { return condition != nullptr; }))
        uia->CreateOrConditionFromNativeArray(conditions, ARRAYSIZE(conditions), &filter);
    for (auto* condition : conditions)
        if (condition) condition->Release();
    return filter;
}

IUIAutomationCacheRequest* SpanCache(IUIAutomation* uia) {
    IUIAutomationCacheRequest* cache = nullptr;
    if (FAILED(uia->CreateCacheRequest(&cache)) || !cache) return nullptr;
    bool complete = SUCCEEDED(cache->put_AutomationElementMode(AutomationElementMode_None));
    for (PROPERTYID property : { UIA_AutomationIdPropertyId, UIA_ClassNamePropertyId,
                                 UIA_IsOffscreenPropertyId, UIA_BoundingRectanglePropertyId })
        complete = complete && SUCCEEDED(cache->AddProperty(property));
    if (complete) return cache;
    cache->Release();
    return nullptr;
}

bool AddNativeControls(Span& span, bool xaml) {
    bool sawNativeControls = false;
    for (HWND child = FindWindowExW(span.bar, nullptr, nullptr, nullptr); child;
         child = FindWindowExW(span.bar, child, nullptr, nullptr)) {
        if (!IsWindowVisible(child)) continue;
        wchar_t cls[128]{};
        if (!GetClassNameW(child, cls, ARRAYSIZE(cls))) return false;
        if (wcscmp(cls, kClass) == 0 ||
            (xaml && (wcscmp(cls, kBridgeClass) == 0 ||
                      wcscmp(cls, L"Windows.UI.Input.InputSite.WindowClass") == 0 ||
                      wcscmp(cls, L"ReBarWindow32") == 0)))
            continue;
        RECT rect{}, clipped{};
        if (!GetWindowRect(child, &rect) || !IntersectRect(&clipped, &span.bounds, &rect)) continue;
        if (wcscmp(cls, L"TrayNotifyWnd") == 0 || wcscmp(cls, L"ClockButton") == 0)
            span.trayLeft = std::min(span.trayLeft, clipped.left);
        sawNativeControls |= wcscmp(cls, L"Start") == 0 || wcscmp(cls, L"ReBarWindow32") == 0 ||
                             wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"MSTaskListWClass") == 0;
        span.occupied.push_back(clipped);
    }
    std::sort(span.occupied.begin(), span.occupied.end(), [](const RECT& a, const RECT& b) { return a.left < b.left; });
    return xaml || sawNativeControls;
}

Span MeasureSpan(IUIAutomation* uia, IUIAutomationCondition* filter, IUIAutomationCacheRequest* cache,
                 HWND bar, bool leftSpot) {
    Span s;
    s.bar = bar;
    s.sampledAt = GetTickCount64();
    if (!GetWindowRect(bar, &s.bounds) || IsRectEmpty(&s.bounds)) return s;
    s.dpi = GetDpiForWindow(bar);
    s.trayLeft = s.bounds.right;
    s.rtl = (GetWindowLongW(bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    const bool v = IsVertical(s.bounds);
    const bool measureLeft = leftSpot && !v && !s.rtl;
    IUIAutomationElement* root = nullptr;
    IUIAutomationElementArray* elements = nullptr;
    int count = 0;
    // Cache the controls and their positions in one call to Explorer.
    bool complete = SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(root->FindAllBuildCache(TreeScope_Descendants, filter, cache, &elements)) &&
                    elements && SUCCEEDED(elements->get_Length(&count));
    if (root) root->Release();
    std::vector<RECT> tasks;
    for (int i = 0; complete && i < count; ++i) {
        IUIAutomationElement* element = nullptr;
        BOOL offscreen = TRUE;
        complete = SUCCEEDED(elements->GetElement(i, &element)) && element &&
                   SUCCEEDED(element->get_CachedIsOffscreen(&offscreen));
        if (complete && !offscreen) {
            BSTR id = nullptr, cls = nullptr;
            RECT rect{}, clipped{};
            complete = SUCCEEDED(element->get_CachedAutomationId(&id)) &&
                       SUCCEEDED(element->get_CachedClassName(&cls)) &&
                       SUCCEEDED(element->get_CachedBoundingRectangle(&rect));
            if (complete && IntersectRect(&clipped, &s.bounds, &rect)) {
                if (measureLeft) s.occupied.push_back(clipped);
                const bool start = id && wcscmp(id, L"StartButton") == 0;
                const bool task = cls && wcscmp(cls, L"Taskbar.TaskListButtonAutomationPeer") == 0;
                s.hasStart |= start;
                if (start || task) {
                    s.firstLeft = s.hasFirst ? std::min(s.firstLeft, (int)clipped.left) : clipped.left;
                    s.hasFirst = true;
                }
                if (task) tasks.push_back(clipped);
                if (id && wcscmp(id, L"SystemTrayIcon") == 0) {
                    s.clockLeft = s.hasClock ? std::min(s.clockLeft, Lo(clipped, v)) : Lo(clipped, v);
                    s.clockRight = s.hasClock ? std::max(s.clockRight, (int)clipped.right) : clipped.right;
                    s.hasClock = true;
                }
                if (id && wcscmp(id, L"WidgetsButton") == 0 && !s.hasWidgets) {
                    s.widgets = clipped;
                    s.hasWidgets = true;
                }
            }
            SysFreeString(id);
            SysFreeString(cls);
        }
        if (element) element->Release();
    }
    if (elements) elements->Release();
    if (!complete) return s;
    s.measured = true;
    for (const RECT& task : tasks) {
        if ((!s.hasClock || Hi(task, v) <= s.clockLeft) && (!s.hasWidgets || Hi(task, v) <= Lo(s.widgets, v)) &&
            (!s.hasTasks || Hi(task, v) > s.tasksRight)) {
            s.tasksRight = Hi(task, v);
            s.hasTasks = true;
        }
        if (s.rtl && !v && (!s.hasClock || task.left >= s.clockRight) &&
            (!s.hasWidgets || task.left >= s.widgets.right) && (!s.hasRtlTasks || task.left < s.rtlTasksLeft)) {
            s.rtlTasksLeft = task.left;
            s.hasRtlTasks = true;
        }
    }
    if (measureLeft && s.hasFirst) {
        if (s.hasClock) s.trayLeft = s.clockLeft;
        s.leftMeasured = AddNativeControls(s, true);
    }
    return s;
}

// A left / right Windows 11 taskbar: the screen-y range its XAML buttons (Start, search, task buttons)
// take above the notification area. The task list is centred and grows towards the tray, and being XAML
// it cannot be shortened like Windows 10's ReBarWindow32, so the column has to step out of its way.
struct Column {
    HWND bar = nullptr;
    bool measured = false;  // UI Automation answered for this taskbar
    bool any = false;       // at least one button found
    int top = 0, bottom = 0;
};
Column g_column;  // guarded by g_wbLock

bool IsVerticalBar(HWND bar) {
    RECT r{};
    return GetWindowRect(bar, &r) && r.bottom - r.top > r.right - r.left;
}

Column MeasureColumn(IUIAutomation* uia, HWND bar) {
    Column c;
    c.bar = bar;
    RECT br{}, tr{};
    HWND tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    IUIAutomationElement* root = nullptr;
    if (!tray || !GetWindowRect(tray, &tr) || !GetWindowRect(bar, &br)) return c;
    if (FAILED(uia->ElementFromHandle(bar, &root)) || !root) return c;
    // XAML elements have no window of their own; this leaves out our strip and the taskbar's legacy
    // child windows (ReBarWindow32 and friends still exist on Windows 11, with stale rectangles).
    VARIANT none;
    VariantInit(&none);
    none.vt = VT_I4;
    none.lVal = 0;
    IUIAutomationCondition* xaml = nullptr;
    IUIAutomationCacheRequest* req = nullptr;
    IUIAutomationElementArray* all = nullptr;
    int n = 0;
    // One cross-process call for all rectangles.
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_NativeWindowHandlePropertyId, none, &xaml)) && xaml &&
        SUCCEEDED(uia->CreateCacheRequest(&req)) && req &&
        SUCCEEDED(req->AddProperty(UIA_BoundingRectanglePropertyId)) &&
        SUCCEEDED(root->FindAllBuildCache(TreeScope_Descendants, xaml, req, &all)) && all &&
        SUCCEEDED(all->get_Length(&n))) {
        c.measured = true;
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* el = nullptr;
            RECT r{};
            // Elements reaching below the tray's top are the tray icons and the frame around everything.
            if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CachedBoundingRectangle(&r)) &&
                r.bottom > r.top && r.right > r.left && r.left >= br.left && r.right <= br.right && r.top >= br.top && r.bottom <= tr.top) {
                c.top = c.any ? std::min(c.top, (int)r.top) : (int)r.top;
                c.bottom = c.any ? std::max(c.bottom, (int)r.bottom) : (int)r.bottom;
                c.any = true;
            }
            if (el) el->Release();
        }
    }
    if (all) all->Release();
    if (req) req->Release();
    if (xaml) xaml->Release();
    root->Release();
    return c;
}

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationCondition* filter = nullptr;
    IUIAutomationCacheRequest* cache = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    DWORD delay = INFINITE;
    for (;;) {
        WaitForSingleObject(g_wbWake, delay);
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        const auto mirrors = g_wbMirrors;
        const bool primaryVisible = g_wbPrimaryVisible, leftSpot = g_wbLeftSpot;
        const unsigned generation = g_wbGeneration;
        ReleaseSRWLockShared(&g_wbLock);
        const HWND primary = primaryVisible && bar && IsWindow(bar) ? bar : nullptr;
        if (btn && primary != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = primary;
        RECT r{};
        if (bar && !uia)
            CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
        if (primary && uia) {
            if (!btn) {
                IUIAutomationElement* root = nullptr;
                if (SUCCEEDED(uia->ElementFromHandle(primary, &root)) && root) {
                    if (auto* cond = StringCondition(uia, UIA_AutomationIdPropertyId, L"WidgetsButton")) {
                        root->FindFirst(TreeScope_Descendants, cond, &btn);
                        cond->Release();
                    }
                    root->Release();
                }
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();
                btn = nullptr;
                r = {};
            }
        }
        std::vector<Span> spans;
        if (bar && uia && ((leftSpot && primary) || !mirrors.empty())) {
            if (!filter) filter = SpanFilter(uia);
            if (!cache) cache = SpanCache(uia);
            if (filter && cache) {
                if (leftSpot && primary && IsXamlTaskbar(primary))
                    spans.push_back(MeasureSpan(uia, filter, cache, primary, true));
                for (HWND mirror : mirrors)
                    if (IsXamlTaskbar(mirror)) spans.push_back(MeasureSpan(uia, filter, cache, mirror, leftSpot));
            }
        }
        Column col;
        const bool column = primary && uia && IsVerticalBar(primary) && IsXamlTaskbar(primary);
        if (column) col = MeasureColumn(uia, primary);
        delay = bar ? 5000 : INFINITE;
        if (leftSpot && !spans.empty()) delay = spans.size() > 1 ? 1000 : 2000;
        if (column) delay = std::min<DWORD>(delay, 1500);
        AcquireSRWLockExclusive(&g_wbLock);
        if (generation == g_wbGeneration) {
            g_column = col;
            if (bar) {
                g_wbRect = r;
                g_wbRectBar = bar;
            }
            g_spans = std::move(spans);
        }
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ++g_wbGeneration;
    g_spans.clear();
    g_column = {};
    if (!taskbar) {
        g_wbMirrors.clear();
        g_wbPrimaryVisible = true;
    }
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

bool FindLeftSlot(const Span& span, LONG width, LONG pad, int& x) {
    LONG left = span.bounds.left + pad;
    const LONG right = span.trayLeft - pad;
    for (const auto& rect : span.occupied) {
        if (left + width <= rect.left - pad) break;
        left = std::max(left, rect.right + pad);
    }
    if (left + width > right) return false;
    x = left;
    return true;
}

bool FindBeforeAppsSlot(const Span& span, LONG width, LONG pad, int& x) {
    LONG edge = span.firstLeft;
    if (span.hasWidgets && span.widgets.right <= edge && span.widgets.right + pad >= edge - width - pad)
        edge = span.widgets.left;
    const LONG left = edge - width - pad;
    if (left < span.bounds.left + pad || left + width > span.trayLeft - pad ||
        std::any_of(span.occupied.begin(), span.occupied.end(), [&](const RECT& rect) {
            return rect.left - pad < left + width && rect.right + pad > left;
        }))
        return false;
    x = left;
    return true;
}

// Use fresh measurements for horizontal left-side placement and let callers handle the tray position.
bool FarLeftX(HWND bar, int width, int& x) {
    RECT bounds{};
    if (width <= 0 || !GetWindowRect(bar, &bounds) || IsRectEmpty(&bounds) || IsVertical(bounds) ||
        (GetWindowLongW(bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL))
        return false;
    const UINT dpi = GetDpiForWindow(bar);
    if (!dpi) return false;
    const LONG pad = MulDiv(4, dpi, 96);
    if (!IsXamlTaskbar(bar)) {
        if (g_beforeApps) return false;
        Span span;
        span.bar = bar;
        span.bounds = bounds;
        span.trayLeft = bounds.right;
        return AddNativeControls(span, false) && FindLeftSlot(span, width, pad, x);
    }

    bool placed = false;
    const ULONGLONG now = GetTickCount64();
    constexpr ULONGLONG sampleLifetimeMs = 3500;
    AcquireSRWLockShared(&g_wbLock);
    for (const auto& span : g_spans) {
        if (span.bar != bar || !span.leftMeasured || (bar == g_taskbar && !span.hasStart) ||
            span.dpi != dpi || !EqualRect(&span.bounds, &bounds) || now - span.sampledAt > sampleLifetimeMs)
            continue;
        placed = g_beforeApps ? FindBeforeAppsSlot(span, width, pad, x) : FindLeftSlot(span, width, pad, x);
        break;
    }
    ReleaseSRWLockShared(&g_wbLock);
    return placed;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        case DeviceKind::Phone:    return 0xE8EA;  // CellPhone
        case DeviceKind::Microphone: return 0xE720;  // Microphone
        case DeviceKind::Speaker:  return 0xE7F5;  // Speakers
        default:                   return bluetooth ? 0xE702 : 0xE772;  // Bluetooth / Devices
    }
}
constexpr wchar_t kBolt = 0xE945;            // LightningBolt
constexpr wchar_t kCheck = 0xE73E;           // CheckMark, in place of the bolt once fully charged
constexpr wchar_t kWifi = 0xE701;            // Wifi: device on a 2.4 GHz receiver (PR #2277)
constexpr wchar_t kBatteryUnknown = 0xE996;  // shown when there is nothing to display

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback, int fallbackPx = 0) {
    for (const wchar_t* f : { face, fallback }) {
        int h_px = (f == fallback && fallbackPx > 0) ? fallbackPx : px;
        HFONT h = CreateFontW(-h_px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    // rowsFrom / rowsTo: only those rows of the mask (the charging fill animation draws part of an icon).
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha, int rowsTo = -1, int rowsFrom = 0) {
        if (rowsTo < 0 || rowsTo > m.h) rowsTo = m.h;
        for (int j = std::max(0, rowsFrom); j < rowsTo; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
};

// Soft glow around a glyph, like a lit RGB device: the coverage mask padded by r pixels, box-blurred
// twice and brightened so a thin icon outline still gives a visible glow.
Mask Halo(const Mask& m, int r) {
    Mask g;
    g.w = m.w + 2 * r;
    g.h = m.h + 2 * r;
    std::vector<int> a((size_t)g.w * g.h, 0), tmp(a.size(), 0);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) a[(y + r) * g.w + x + r] = m.a[y * m.w + x];
    int n = 2 * r + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (x + k >= 0 && x + k < g.w) s += a[y * g.w + x + k];
                tmp[y * g.w + x] = s / n;
            }
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++) {
                int s = 0;
                for (int k = -r; k <= r; k++) if (y + k >= 0 && y + k < g.h) s += tmp[(y + k) * g.w + x];
                a[y * g.w + x] = s / n;
            }
    }
    g.a.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) g.a[i] = (BYTE)std::min(255, a[i] * 3);
    return g;
}

// One drawn piece of the strip. Normal pieces follow each other from left to right. A piece with an
// `anchor` (index of an earlier piece) is a caption: drawn over that piece, `bx`/`by` from its top-left
// corner, and taking no room of its own. Branded devices add a glow behind the glyph and a level bar.
struct Piece {
    Mask m;
    Rgb color;
    int alpha;
    int gapAfter;
    int anchor = -1;
    int bx = 0, by = 0;
    int barPct = -1;  // >= 0: thin level bar under the text (branded devices)
    Rgb barColor{};
    Mask halo;        // glow drawn behind the glyph (branded devices)
    int haloR = 0, haloAlpha = 0;
    int px = INT_MIN, py = 0;  // fixed top-left position (stacked and vertical layouts); INT_MIN = flow left to right
    bool charging = false;  // animated fill (PR #2463)
    int item = -1;    // index in g_items of the device this piece belongs to (finds the device under the mouse)
};

// Coverage masks for a ring of diameter d (PR #110): the full circle (track) and the arc that starts at
// 12 o'clock and runs clockwise over `fraction` of it. 3x3 supersampled for smooth edges.
void RingMasks(int d, double stroke, double fraction, Mask& track, Mask& arc) {
    const double kTwoPi = 6.283185307179586;
    track.w = track.h = arc.w = arc.h = d;
    track.a.assign((size_t)d * d, 0);
    arc.a.assign((size_t)d * d, 0);
    double R = d / 2.0, inner = R - stroke;
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++) {
            int t = 0, a = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    double px = x + (sx + 0.5) / 3 - R, py = y + (sy + 0.5) / 3 - R;
                    double r = std::sqrt(px * px + py * py);
                    if (r > R || r < inner) continue;
                    t++;
                    double ang = std::atan2(px, -py);
                    if (ang < 0) ang += kTwoPi;
                    if (ang <= fraction * kTwoPi) a++;
                }
            track.a[(size_t)y * d + x] = (BYTE)(t * 255 / 9);
            arc.a[(size_t)y * d + x] = (BYTE)(a * 255 / 9);
        }
}

// Three arrangements of the same pieces:
//  * normal: "[icon] 30%  [icon] 93%" in one row (unchanged default)
//  * stacked (menu option, from PR #525): each device's icon above its percentage, to save width;
//    used only when the taskbar is tall enough, otherwise the normal row
//  * vertical (left / right taskbar, from PR #321): one device per row block, stacked top to bottom
// `width` and `height` return the strip's size; in the vertical layout the width is the bar's.
std::vector<Piece> Layout(int barW, int barH, int& width, int& height) {
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    Rgb full = g_light ? Rgb{ 16, 124, 16 } : Rgb{ 108, 203, 95 };  // Windows "success" green
    HFONT icons = MakeFont(Px(16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT bolt = MakeFont(Px(11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT text = MakeFont(Px(13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI", Px(12));
    HFONT caption = MakeFont(std::max(8, Px(7)), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");  // never under 8 px
    // Ring style (PR #110) is drawn in the normal one-row layout only; stacked and vertical keep the text.
    const bool ring = g_ringStyle && !g_stacked && barH <= barW;
    HFONT ringIcons = ring ? MakeFont(Px(11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets") : nullptr;

    std::vector<Piece> out;
    struct Slot { int icon, label, bolt; };  // indices of one device's pieces
    std::vector<Slot> slots;
    if (g_items.empty()) out.push_back({ RenderMask(std::wstring(1, kBatteryUnknown), icons, g_light), fg, 110, 0 });
    for (size_t i = 0; i < g_items.size(); i++) {
        const auto& it = g_items[i];
        int a = it.dim ? 120 : 255;
        if (ring) {
            // The arc is the charge: red when low, blue while charging; the faint full circle is the track.
            int d = Px(28);
            d += d & 1;  // even, so the ring and the icon share an exact centre
            Mask track, arc;
            RingMasks(d, std::max(2.0, d / 13.0), it.percent >= 0 ? it.percent / 100.0 : 0.0, track, arc);
            int base = (int)out.size();
            out.push_back({ std::move(track), fg, a * (g_light ? 45 : 60) / 255, i + 1 < g_items.size() ? Px(8) : 0 });
            Piece arcPiece{ std::move(arc), it.charging ? charge : it.low ? red : fg, a, 0 };
            arcPiece.anchor = base;
            out.push_back(std::move(arcPiece));
            Piece glyph{ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), ringIcons, g_light), fg, a, 0 };
            glyph.anchor = base;
            glyph.bx = (d - glyph.m.w) / 2;
            glyph.by = (d - glyph.m.h) / 2;
            out.push_back(std::move(glyph));
            for (size_t k = (size_t)base; k < out.size(); k++) out[k].item = (int)i;  // the hover card finds it
            continue;
        }
        Rgb tc = it.low ? red : fg;
        bool mark = it.charging || it.full;  // bolt while charging, green check mark once full
        bool link = !mark && g_connectionIcons && it.wirelessLink;  // wireless mark otherwise (opt-in)
        size_t glyph = out.size();
        slots.push_back({ (int)glyph, (int)glyph + 1, mark || link ? (int)glyph + 2 : -1 });
        bool branded = it.accent != Accent::None;
        BrandColor bc = AccentColor(it.accent, g_light);
        Rgb accent{ bc.r, bc.g, bc.b };
        Piece icon{ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), icons, g_light), branded ? accent : fg, a, Px(5) };
        if (branded && !it.dim) {
            icon.haloR = Px(3);
            icon.halo = Halo(icon.m, icon.haloR);
            icon.haloAlpha = g_light ? 60 : 140;
        }
        icon.charging = g_animateCharging && it.charging && !it.full;
        out.push_back(std::move(icon));
        std::wstring t = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + (g_showPercentSign ? L"%" : L"") : L"?";
        Piece label{ RenderMask(t, text, g_light), tc, a, mark || link ? Px(2) : 0 };
        if (branded && it.percent >= 0) {
            label.barPct = it.percent;
            label.barColor = it.low ? red : accent;
        }
        out.push_back(std::move(label));
        if (mark) out.push_back({ RenderMask(std::wstring(1, it.full ? kCheck : kBolt), bolt, g_light), it.full ? full : charge, a, 0 });
        else if (link) out.push_back({ RenderMask(std::wstring(1, kWifi), bolt, g_light), fg, a * 160 / 255, 0 });
        out.back().gapAfter = i + 1 < g_items.size() ? Px(14) : 0;
        if (!it.label.empty()) {  // tiny caption in the bottom-right corner of the icon
            Piece c{ RenderMask(it.label, caption, g_light), fg, a, 0 };
            c.anchor = (int)glyph;
            c.bx = out[glyph].m.w - c.m.w + Px(3);
            c.by = out[glyph].m.h - c.m.h + Px(3);
            out.push_back(c);
        }
        for (size_t k = glyph; k < out.size(); k++) out[k].item = (int)i;
    }
    DeleteObject(icons);
    DeleteObject(bolt);
    DeleteObject(text);
    DeleteObject(caption);
    if (ringIcons) DeleteObject(ringIcons);

    height = barH;
    const int rowGap = Px(2);
    const int barRoom = Px(3) + std::max(2, Px(2));  // space for a brand level bar under the text
    int iconH = 0, textH = 0;
    bool hasBar = false;
    for (const Slot& s : slots) {
        iconH = std::max(iconH, out[s.icon].m.h);
        textH = std::max(textH, out[s.label].m.h);
        hasBar |= out[s.label].barPct >= 0;
    }
    bool vertical = barH > barW;  // per bar: a secondary taskbar can be docked differently from the main one
    const int stackedBarRoom = hasBar ? barRoom : 0;
    const int stackedPadding = Px(6);  // Minimum space above and below the stack.
    bool stacked = !vertical && g_stacked && !slots.empty() &&
                   barH >= iconH + rowGap + textH + stackedBarRoom + 2 * stackedPadding;
    if (vertical) {
        // One block per device, centred in the bar's width: icon, then the percentage (and bolt) under it.
        // Spaced like the notification area's own icons, so the column reads as part of it.
        width = barW;
        const int pad = Px(10), blockGap = Px(12), iconGap = Px(3);
        int y = pad;
        if (slots.empty()) {
            // No devices yet (startup, or the last one disconnected): only the placeholder icon. Without this
            // branch the row layout below would keep height = barH, and on a left / right taskbar barH is the
            // whole screen height, so the strip covered the entire taskbar.
            Piece& ic = out[0];
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            height = y + ic.m.h + pad;
            return out;
        }
        for (const Slot& s : slots) {
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            ic.px = (barW - ic.m.w) / 2;
            ic.py = y;
            y += ic.m.h + iconGap;
            lb.px = (barW - lb.m.w - boltW) / 2;
            lb.py = y;
            if (s.bolt >= 0) {
                out[s.bolt].px = lb.px + lb.m.w + Px(2);
                out[s.bolt].py = y + (lb.m.h - out[s.bolt].m.h) / 2;
            }
            y += lb.m.h + (lb.barPct >= 0 ? barRoom : 0) + blockGap;
        }
        height = y - blockGap + pad;
        return out;
    }
    if (stacked) {
        // Icon on top (bolt to its right), percentage centred under it; the pair centred in the bar's height.
        int top = (barH - (iconH + rowGap + textH + stackedBarRoom)) / 2;
        int x = Px(8);
        for (const Slot& s : slots) {
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].m.w : 0;
            int slotW = std::max(ic.m.w + boltW, lb.m.w);
            ic.px = x + (slotW - ic.m.w - boltW) / 2;
            ic.py = top + (iconH - ic.m.h) / 2;
            if (s.bolt >= 0) {
                out[s.bolt].px = ic.px + ic.m.w + Px(2);
                out[s.bolt].py = top + (iconH - out[s.bolt].m.h) / 2;
            }
            lb.px = x + (slotW - lb.m.w) / 2;
            lb.py = top + iconH + rowGap;
            x += slotW + Px(10);
        }
        width = x - Px(10) + Px(8);
        return out;
    }
    width = Px(8) * 2;
    for (auto& p : out)
        if (p.anchor < 0) width += p.m.w + p.gapAfter;
    return out;
}

// Each device's area in a strip (client coordinates), from the last paint: the primary strip's, and each
// mirror keeps its own. Neighbouring areas meet halfway between devices, so there is no dead gap.
std::vector<RECT> g_itemRects;

// Draws the strip for one window: the primary one or a mirror on a secondary taskbar.
// Returns true only when the layered surface was updated with a valid non-empty size.
bool PaintStrip(HWND wnd, HWND taskbar, bool hover, SIZE& size, std::vector<RECT>& itemRects) {
    if (!wnd || !taskbar || !IsWindow(wnd) || !IsWindow(taskbar)) return false;
    RECT bar{};
    if (!GetClientRect(taskbar, &bar)) return false;
    int W = 0, H = 0;
    auto pieces = Layout(bar.right - bar.left, bar.bottom - bar.top, W, H);
    if (W <= 0 || H <= 0) return false;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    DWORD clear = 0x01000000;
    if (wnd == g_wnd && g_fill)  // bottom place on a vertical taskbar: continue the taskbar's colour
        clear = 0xFF000000 | ((DWORD)GetRValue(g_fillColor) << 16) | ((DWORD)GetGValue(g_fillColor) << 8) |
                GetBValue(g_fillColor);
    // MyFinder needs alpha 0: even nearly transparent fills cause blur flashes there; the composited
    // window still takes clicks on transparent pixels.
    if (g_myFinder && wnd == g_wnd) clear = 0;
    for (auto& p : cv.px) p = clear;
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        for (int y = inset; y < H - inset; y++)
            for (int x = 0; x < W; x++) {
                int dx = x < r ? r - x : (x >= W - r ? x - (W - r - 1) : 0);
                int dy = y < inset + r ? inset + r - y : (y >= H - inset - r ? y - (H - inset - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    Rgb barColor = g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 };
    if (g_background && !g_myFinder) roundRect(Px(4), Px(6), barColor, 240);
    if (hover && !g_myFinder) roundRect(Px(4), Px(4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);

    int x = Px(8);
    Rgb track = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    std::vector<int> xs(pieces.size()), ys(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++) {
        const Piece& p = pieces[i];
        if (p.anchor < 0) {
            bool fixed = p.px != INT_MIN;  // stacked / vertical layout: Layout() placed it
            xs[i] = fixed ? p.px : x;
            ys[i] = fixed ? p.py : (H - p.m.h) / 2;
            if (p.haloR) cv.Draw(p.halo, xs[i] - p.haloR, ys[i] - p.haloR, p.color, p.haloAlpha * p.alpha / 255);
            if (p.barPct >= 0) {
                // The bar spans the text's width, like Synapse's battery bar; in the row layout text and bar
                // are centred together.
                int bh = std::max(2, Px(2)), gap = Px(3);
                if (!fixed) ys[i] = (H - (p.m.h + gap + bh)) / 2;
                int by = ys[i] + p.m.h + gap;
                int fill = std::max(p.barPct > 0 ? 1 : 0, (p.barPct * p.m.w + 50) / 100);
                for (int yy = by; yy < by + bh; yy++)
                    for (int xx = 0; xx < p.m.w; xx++)
                        cv.Blend(xs[i] + xx, yy, xx < fill ? p.barColor : track, xx < fill ? p.alpha : 45 * p.alpha / 255);
            }
            if (!fixed) x += p.m.w + p.gapAfter;
        } else {  // caption: one-pixel outline in the taskbar's colour so it reads over the icon's lines
            xs[i] = xs[p.anchor] + p.bx;
            ys[i] = ys[p.anchor] + p.by;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                    if (ox || oy) cv.Draw(p.m, xs[i] + ox, ys[i] + oy, barColor, 255);
        }
        if (p.charging) {
            // Green fills the icon bottom to top, then drains from the bottom: a 2.4 s loop that ignores the
            // level (the percentage and bar show that). The empty part stays in the normal colour, a bit dimmer.
            Rgb green = g_light ? Rgb{ 0, 160, 40 } : Rgb{ 30, 230, 80 };
            double t = (GetTickCount64() % 2400) / 2400.0;
            int head = t < 0.5 ? (int)(p.m.h * t * 2) : p.m.h;
            int tail = t < 0.5 ? 0 : (int)(p.m.h * (t - 0.5) * 2);
            cv.Draw(p.m, xs[i], ys[i], p.color, p.alpha * 80 / 100);
            cv.Draw(p.m, xs[i], ys[i], green, p.alpha, p.m.h - tail, p.m.h - head);
        } else {
            cv.Draw(p.m, xs[i], ys[i], p.color, p.alpha);
        }
    }

    // Where each device sits, for the hover card: the span of its pieces, widened to meet its neighbours.
    itemRects.assign(g_items.size(), RECT{ INT_MAX, INT_MAX, INT_MIN, INT_MIN });
    for (size_t i = 0; i < pieces.size(); i++) {
        int k = pieces[i].item;
        if (k < 0 || k >= (int)itemRects.size()) continue;
        RECT& r = itemRects[k];
        r.left = std::min(r.left, (LONG)xs[i]);
        r.top = std::min(r.top, (LONG)ys[i]);
        r.right = std::max(r.right, (LONG)(xs[i] + pieces[i].m.w));
        r.bottom = std::max(r.bottom, (LONG)(ys[i] + pieces[i].m.h));
    }
    for (size_t k = 0; k < itemRects.size(); k++) {
        RECT& r = itemRects[k];
        if (r.left > r.right) { r = RECT{}; continue; }
        if (g_vertical) {  // devices one under the other: full width, split the height
            r.left = 0;
            r.right = W;
            r.top = k == 0 ? 0 : (itemRects[k - 1].bottom + r.top) / 2;
        } else {
            r.top = 0;
            r.bottom = H;
            r.left = k == 0 ? 0 : (itemRects[k - 1].right + r.left) / 2;
        }
    }
    for (size_t k = 0; k + 1 < itemRects.size(); k++) {
        if (g_vertical) itemRects[k].bottom = itemRects[k + 1].top;
        else itemRects[k].right = itemRects[k + 1].left;
    }
    if (!itemRects.empty()) {
        if (g_vertical) itemRects.back().bottom = H;
        else itemRects.back().right = W;
    }

    if (g_myFinder && wnd == g_wnd) {
        if (!g_compositionReady) return false;
        if (!g_composition.Paint(cv.px.data(), W, H, W * 4)) {
            g_compositionReady = g_composition.Initialize(g_wnd) && g_composition.Paint(cv.px.data(), W, H, W * 4);
            if (!g_compositionReady) {
                ShowWindow(g_wnd, SW_HIDE);
                return false;
            }
        }
        size = { W, H };
        g_lastW = -1;
        return true;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    if (!mem) {
        ReleaseDC(nullptr, screen);
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    bool updated = UpdateLayeredWindow(wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA) != FALSE;
    if (updated) size = { W, H };
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return updated;
}

void Paint() {
    if (!g_wnd) return;
    RECT client{};
    g_painted = PaintStrip(g_wnd, g_taskbar, g_hover, g_size, g_itemRects);
    if (g_painted && GetClientRect(g_taskbar, &client)) {
        g_paintedBarClient = { client.right - client.left, client.bottom - client.top };
    } else {
        g_size = {};
        g_paintedBarClient = {};
    }
    g_lastW = -1;  // force a re-position with the new size
}

void PaintMirror(Mirror& m) {
    g_dpiBar = m.bar;
    m.dpi = Dpi();
    m.painted = PaintStrip(m.wnd, m.bar, m.hover, m.size, m.itemRects);
    if (!m.painted) m.size = {};
    g_dpiBar = nullptr;
    m.lastLeft = INT_MIN;  // force a re-position with the new size
}

void HideMirror(Mirror& m) {
    if (m.lastLeft != INT_MIN || IsWindowVisible(m.wnd)) ShowWindow(m.wnd, SW_HIDE);
    m.lastLeft = INT_MIN;
}

// The window's own visible flag (IsWindowVisible() is also false while the taskbar itself is hidden).
bool Shown(HWND w) { return (GetWindowLongW(w, GWL_STYLE) & WS_VISIBLE) != 0; }

// Places a mirror left of its taskbar's clock (and left of the Widgets button if present), or hides it
// when that spot is unknown or taken by task buttons: never guess and cover something.
// On a vertical (left / right) taskbar the same happens along the bar's length: the mirror sits above
// the clock, centred in the bar's width, like the primary strip does.
void PositionMirror(Mirror& m) {
    RECT bar{};
    if (!GetWindowRect(m.bar, &bar)) { HideMirror(m); return; }
    bool v = IsVertical(bar);
    const bool rtl = (GetWindowLongW(m.bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    if (rtl && v) { HideMirror(m); return; }  // a vertical right-to-left bar is not handled
    g_dpiBar = m.bar;
    // The strip's cross-bar size equals the bar's; a mismatch means the bar was resized or re-docked.
    if (!m.painted || Dpi() != m.dpi || m.size.cx <= 0 ||
        (v ? m.size.cx != bar.right - bar.left : m.size.cy != bar.bottom - bar.top)) {
        PaintMirror(m);
        g_dpiBar = m.bar;
    }
    if (!m.painted || m.size.cx <= 0 || m.size.cy <= 0) {
        g_dpiBar = nullptr;
        HideMirror(m);
        return;
    }
    int length = v ? m.size.cy : m.size.cx;  // the strip's size along the bar
    int right = 0, minLeft = Lo(bar, v);      // read "right"/"left" as "bottom"/"top" on a vertical bar
    bool known = false;
    if (rtl) {
        // Right-to-left (PR #2853): the notification area is at the left end; sit just right of it (or of the
        // Widgets button there), and only while the task buttons leave room. Mirrors the left-to-right rules.
        int edge = 0, limit = bar.right;
        HWND rtray = FindWindowExW(m.bar, nullptr, L"TrayNotifyWnd", nullptr);
        RECT rt{};
        if (rtray && IsWindowVisible(rtray) && GetWindowRect(rtray, &rt) && rt.right > rt.left && rt.left >= bar.left &&
            rt.right < bar.right && rt.bottom > bar.top && rt.top < bar.bottom) {
            edge = rt.right;
            known = true;
        } else if (!IsXamlTaskbar(m.bar)) {
            HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
            RECT cr{};
            edge = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? cr.right : bar.left + Px(8);
            for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
                HWND feeds = FindWindowExW(m.bar, nullptr, cls, nullptr);
                RECT fr{};
                if (feeds && IsWindowVisible(feeds) && GetWindowRect(feeds, &fr) && fr.right > edge && fr.right > fr.left)
                    edge = fr.right;
            }
            known = true;
        } else {
            AcquireSRWLockShared(&g_wbLock);
            for (const auto& s : g_spans)
                if (s.bar == m.bar && s.measured && s.rtl) {
                    edge = s.hasClock ? s.clockRight : bar.left + Px(8);
                    if (s.hasWidgets && s.widgets.right > edge && s.widgets.left < edge + length + Px(2))
                        edge = s.widgets.right;
                    if (s.hasRtlTasks) limit = s.rtlTasksLeft - Px(4);
                    known = true;
                }
            ReleaseSRWLockShared(&g_wbLock);
        }
        DWORD off = 0, osz = sizeof(off);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &off,
                         &osz) == ERROR_SUCCESS)
            edge += Px((int)off);
        int left = edge + Px(2);
        g_dpiBar = nullptr;
        if (!known || length <= 0 || left + length > limit) { HideMirror(m); return; }
        // In a WS_EX_LAYOUTRTL parent, client x runs from the right edge: convert the strip's screen RIGHT edge.
        POINT p{ left + length, bar.top };
        ScreenToClient(m.bar, &p);
        int along = p.x;
        bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
        if (!covered && Shown(m.wnd) && along == m.lastLeft) return;
        m.lastLeft = along;
        SetWindowPos(m.wnd, HWND_TOP, p.x, 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return;
    }
    // StartAllBack can put a full notification area on each secondary taskbar.
    // Use its left edge, as on the primary taskbar, to clear all of the tray icons.
    HWND tray = FindWindowExW(m.bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tr{};
    if (tray && IsWindowVisible(tray) && GetWindowRect(tray, &tr) &&
        Lo(tr, v) > Lo(bar, v) && Hi(tr, v) <= Hi(bar, v) && Hi(tr, v) > Lo(tr, v) &&
        Hi(tr, !v) > Lo(bar, !v) && Lo(tr, !v) < Hi(bar, !v)) {
        right = Lo(tr, v);
        known = true;
    } else if (!IsXamlTaskbar(m.bar)) {  // Windows 10: the clock is a real child window (absent when it is turned off)
        HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
        RECT cr{};
        right = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? Lo(cr, v) : Hi(bar, v) - Px(8);
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(m.bar, nullptr, cls, nullptr);
            RECT fr{};
            if (feeds && IsWindowVisible(feeds) && GetWindowRect(feeds, &fr) && Lo(fr, v) < right && Hi(fr, v) > Lo(fr, v))
                right = Lo(fr, v);
        }
        known = true;
    } else {
        AcquireSRWLockShared(&g_wbLock);
        for (const auto& s : g_spans)
            if (s.bar == m.bar && s.measured) {
                right = s.hasClock ? s.clockLeft : Hi(bar, v) - Px(8);
                if (s.hasWidgets && Lo(s.widgets, v) < right && Hi(s.widgets, v) > right - length - Px(2))
                    right = Lo(s.widgets, v);
                if (s.hasTasks) minLeft = s.tasksRight + Px(4);
                known = true;
            }
        ReleaseSRWLockShared(&g_wbLock);
    }
    DWORD offset = 0, sz = sizeof(offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"SecondaryTaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    int left = right - length - Px(2), farX = 0;
    bool atFarLeft = !v && (g_farLeft || g_beforeApps) && FarLeftX(m.bar, m.size.cx, farX);  // horizontal bars only
    if (atFarLeft) left = farX;
    g_dpiBar = nullptr;
    if (!atFarLeft && (!known || length <= 0 || left < minLeft)) { HideMirror(m); return; }
    POINT p = v ? POINT{ bar.left, left } : POINT{ left, bar.top };
    ScreenToClient(m.bar, &p);
    int along = v ? p.y : p.x;
    bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && Shown(m.wnd) && along == m.lastLeft) return;
    m.lastLeft = along;
    RECT client{};
    GetClientRect(m.bar, &client);
    int x = v ? (int)std::max<LONG>(0, (client.right - m.size.cx) / 2) : p.x;
    SetWindowPos(m.wnd, HWND_TOP, x, v ? p.y : 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// The summary text is the strip's window name, so screen readers still read the levels.
// The summary text is the strip's window name (read by screen readers) and the plain tooltip's text, which is
// shown instead of the card when the card is turned off.
void SetSummary(HWND w) {
    const wchar_t* text = g_tipText.empty() ? L"BatteryHub" : g_tipText.c_str();
    SetWindowTextW(w, text);
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)w;
    ti.lpszText = const_cast<wchar_t*>(text);
    TTTOOLINFOW probe{ sizeof(probe) };
    probe.hwnd = g_notify;
    probe.uId = (UINT_PTR)w;
    bool known = SendMessageW(g_tip, TTM_GETTOOLINFOW, 0, (LPARAM)&probe) != 0;
    SendMessageW(g_tip, known ? TTM_UPDATETIPTEXTW : TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void DestroyMirror(Mirror& m) {
    if (g_tip) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)m.wnd;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    if (m.wnd && IsWindow(m.wnd)) DestroyWindow(m.wnd);
    m.wnd = nullptr;
}

void SyncLayoutWatch() {
    AcquireSRWLockExclusive(&g_wbLock);
    const bool primaryVisible = !g_primaryHidden;
    const bool changed = g_wbPrimaryVisible != primaryVisible ||
                         !std::equal(g_wbMirrors.begin(), g_wbMirrors.end(), g_mirrors.begin(), g_mirrors.end(),
                                     [](HWND bar, const Mirror& mirror) { return bar == mirror.bar; });
    if (changed) {
        g_wbPrimaryVisible = primaryVisible;
        g_wbMirrors.clear();
        for (const auto& mirror : g_mirrors) g_wbMirrors.push_back(mirror.bar);
        ++g_wbGeneration;
    }
    ReleaseSRWLockExclusive(&g_wbLock);
    if (changed && g_wbWake) SetEvent(g_wbWake);
}

std::vector<HWND> SecondaryTaskbars() {
    std::vector<HWND> out;
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (IsWindowVisible(t)) out.push_back(t);  // horizontal or vertical: PositionMirror handles both
    }
    return out;
}

// Keeps one mirror per secondary taskbar: adds them for new monitors, removes them for gone ones.
// Called from Reposition() about once a second; with one monitor it is a single FindWindowEx call.
// A monitor's lasting identity: its device interface name, which survives reboots, Explorer restarts and
// Windows renumbering the displays (falls back to "\\.\DISPLAYn").
std::wstring MonitorId(HWND bar) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONULL);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"";
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) return dd.DeviceID;
    return mi.szDevice;
}

// The model part of a monitor id: "AUO7EAD" in \\?\DISPLAY#AUO7EAD#4&300ab7d&0&UID8388688#{...}.
// A laptop panel gets a different instance path when a MUX / Advanced Optimus switch moves it between the
// integrated and the NVIDIA GPU, so its full id changes from one boot to the next; this part does not.
std::wstring MonitorModel(const std::wstring& id) {
    size_t a = id.find(L"DISPLAY#");
    if (a == std::wstring::npos) return L"";
    a += 8;
    size_t b = id.find(L'#', a);
    return b == std::wstring::npos ? L"" : id.substr(a, b - a);
}

// "Display 3 (right)": the number Windows gives the display and where it sits next to the main one.
std::wstring MonitorLabel(HWND bar) {
    MONITORINFOEXW mi{}, pm{};
    mi.cbSize = pm.cbSize = sizeof(MONITORINFOEXW);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return T(L"Other monitor");
    std::wstring dev = mi.szDevice;
    size_t d = dev.find(L"DISPLAY");
    std::wstring label = d != std::wstring::npos ? TF(L"Display {number}", { { L"number", dev.substr(d + 7) } }) : T(L"Other monitor");
    POINT origin{ 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm)) {
        int dx = (mi.rcMonitor.left + mi.rcMonitor.right - pm.rcMonitor.left - pm.rcMonitor.right) / 2;
        int dy = (mi.rcMonitor.top + mi.rcMonitor.bottom - pm.rcMonitor.top - pm.rcMonitor.bottom) / 2;
        label += std::wstring(L" (") + T(abs(dx) >= abs(dy) ? (dx < 0 ? L"left" : L"right") : (dy < 0 ? L"above" : L"below")) + L")";
    }
    return label;
}

// Hides or shows the strip on the main taskbar (it keeps running, just out of sight).
void SetPrimaryHidden(bool hide) {
    if (hide == g_primaryHidden || !g_wnd) return;
    g_primaryHidden = hide;
    if (hide) {
        ShowWindow(g_wnd, SW_HIDE);
        ReserveBottom(0);
        if (g_rebar && IsWindow(g_rebar)) {  // Windows 10: give the task buttons their full width back
            RECT r;
            GetClientRect(g_taskbar, &r);
            PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
        }
    }
    g_lastW = -1;  // re-place (and show) on the next pass
}

void SyncMirrors() {
    if (g_myFinder) return;  // MyFinder: one bar only
    std::vector<HWND> all = g_wnd ? SecondaryTaskbars() : std::vector<HWND>{}, bars;
    bool chosenFound = false;
    if (g_choice == L"all") {
        bars = all;
    } else if (g_choice != L"main") {
        std::vector<std::wstring> ids;
        for (HWND b : all) ids.push_back(MonitorId(b));
        int i = widget::FindMonitor(ids, g_choice);
        if (i >= 0) {
            bars.push_back(all[i]);
            chosenFound = true;
        }
    }
    SetPrimaryHidden(chosenFound);
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (std::find(bars.begin(), bars.end(), it->bar) == bars.end() || !IsWindow(it->wnd)) {
            DestroyMirror(*it);
            it = g_mirrors.erase(it);
        } else {
            ++it;
        }
    }
    for (HWND b : bars) {
        if (std::any_of(g_mirrors.begin(), g_mirrors.end(), [b](const Mirror& m) { return m.bar == b; })) continue;
        Mirror m;
        m.bar = b;
        m.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, b, nullptr,
                                g_inst, nullptr);
        if (!m.wnd) continue;
        SetSummary(m.wnd);
        g_mirrors.push_back(m);
        PaintMirror(g_mirrors.back());
    }
    SyncLayoutWatch();
    for (auto& m : g_mirrors) PositionMirror(m);
}

void PaintAll() {
    Paint();
    for (auto& m : g_mirrors) PaintMirror(m);
}

// ---- hover card ----------------------------------------------------------------------------------
// Resting the mouse on a device opens a small card above it, in the style of the Windows 11 flyouts: the
// device's icon and name, its level in large type with a coloured bar, then a few lines (charging / asleep,
// estimated time to empty, connection). It replaces the plain tooltip, follows the light / dark theme and
// the taskbar's DPI, and lets the mouse through (it never takes focus or clicks).
HWND g_card;
int g_cardItem = -1;   // device shown in the card; -1 = hidden, kCardMessage = the "no devices" text
HWND g_cardStrip;      // strip the card was opened from (primary or a mirror)
int g_hoverItem = -1;  // device under the mouse
constexpr int kCardMessage = -2;
constexpr UINT_PTR kCardTimer = 1;
constexpr UINT_PTR kFinderTimer = 3;  // MyFinder: follow the bar every 125 ms
constexpr UINT_PTR kChargeTimer = 2;  // repaints the charging fill animation every 100 ms
constexpr UINT kCardDelayMs = 400;
const wchar_t* kCardClass = L"WirelessBatteryCard";

const std::vector<RECT>* RectsOf(HWND strip) {
    if (strip == g_wnd) return &g_itemRects;
    for (auto& m : g_mirrors)
        if (m.wnd == strip) return &m.itemRects;
    return nullptr;
}

int ItemAt(HWND strip, POINT p) {
    if (g_items.empty()) return kCardMessage;
    const auto* rects = RectsOf(strip);
    if (!rects) return -1;
    for (size_t i = 0; i < rects->size(); i++)
        if (PtInRect(&(*rects)[i], p)) return (int)i;
    return -1;
}

// A line of text at its full cell height (RenderMask crops to the ink), so lines line up on a baseline.
Mask RenderLine(const std::wstring& s, HFONT font) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    if (sz.cx > 0 && sz.cy > 0) {
        int W = sz.cx + 2, H = sz.cy;
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ ob = SelectObject(dc, bmp);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 0, 0, s.c_str(), (int)s.size());
        GdiFlush();
        auto* p = static_cast<DWORD*>(bits);
        m.w = W;
        m.h = H;
        m.a.resize((size_t)W * H);
        for (int i = 0; i < W * H; i++) m.a[i] = (BYTE)(((p[i] & 0xFF) + ((p[i] >> 8) & 0xFF) + ((p[i] >> 16) & 0xFF)) / 3);
        SelectObject(dc, ob);
        DeleteObject(bmp);
    }
    SelectObject(dc, of);
    DeleteDC(dc);
    return m;
}

// Shortens `s` with an ellipsis until it fits in `maxW` pixels.
std::wstring Fit(std::wstring s, HFONT font, int maxW) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    auto width = [&](const std::wstring& t) { SIZE z{}; GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &z); return (int)z.cx; };
    if (width(s) > maxW) {
        while (s.size() > 1 && width(s + L"…") > maxW) s.pop_back();
        s += L"…";
    }
    SelectObject(dc, of);
    DeleteDC(dc);
    return s;
}

// Rounded rectangle with anti-aliased corners.
void FillRound(Canvas& cv, int x0, int y0, int w, int h, int r, Rgb c, int alpha) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            double dx = x < x0 + r ? x0 + r - (x + 0.5) : (x >= x0 + w - r ? (x + 0.5) - (x0 + w - r) : 0);
            double dy = y < y0 + r ? y0 + r - (y + 0.5) : (y >= y0 + h - r ? (y + 0.5) - (y0 + h - r) : 0);
            double cover = (dx > 0 && dy > 0) ? std::min(1.0, std::max(0.0, r - std::sqrt(dx * dx + dy * dy) + 0.5)) : 1.0;
            if (cover > 0) cv.Blend(x, y, c, (int)(alpha * cover));
        }
}

void HideCard() {
    g_cardItem = -1;
    if (g_card) ShowWindow(g_card, SW_HIDE);
}

void ShowCard(HWND strip, int item) {
    const auto* rects = RectsOf(strip);
    bool message = item == kCardMessage;
    if (!rects || (!message && (item < 0 || item >= (int)g_items.size() || item >= (int)rects->size()))) {
        HideCard();
        return;
    }
    if (!g_card) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{ sizeof(wc) };
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_inst;
            wc.lpszClassName = kCardClass;
            registered = RegisterClassExW(&wc) != 0;
        }
        g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kCardClass, L"",
                                 WS_POPUP, 0, 0, 1, 1, g_notify, nullptr, g_inst, nullptr);
        if (!g_card) return;
    }
    HWND bar = GetParent(strip);
    g_dpiBar = bar;  // Px() at the DPI of the monitor the strip is on

    Rgb bg = g_light ? Rgb{ 249, 249, 249 } : Rgb{ 44, 44, 44 };
    Rgb border = g_light ? Rgb{ 222, 222, 222 } : Rgb{ 66, 66, 66 };
    Rgb fg = g_light ? Rgb{ 27, 27, 27 } : Rgb{ 242, 242, 242 };
    Rgb sub = g_light ? Rgb{ 96, 96, 96 } : Rgb{ 170, 170, 170 };
    Rgb track = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    Rgb green = g_light ? Rgb{ 16, 124, 16 } : Rgb{ 108, 203, 95 };
    HFONT icons = MakeFont(Px(18), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT title = MakeFont(Px(14), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT big = MakeFont(Px(28), FW_SEMIBOLD, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT body = MakeFont(Px(12), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");

    const int pad = Px(16), minW = Px(240), maxW = Px(380), textMax = maxW - 2 * pad;
    struct Line { Mask m; Rgb c; int x, y; };
    std::vector<Line> lines;
    Mask glyph;
    int W = minW, y = pad, barY = -1, pct = -1, barAlpha = 255, chartY = -1, chartNow = -1, axisY = 0;
    double forecastHours = -1;
    std::vector<std::pair<long long, int>> chart;
    Mask axisLeft, axisNow, axisEnd;
    Rgb barColor = fg;
    if (message) {
        size_t start = 0;
        while (start <= g_tipText.size()) {
            size_t end = g_tipText.find(L'\n', start);
            std::wstring t = g_tipText.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            Mask m = RenderLine(Fit(t, body, textMax), body);
            W = std::max(W, m.w + 2 * pad);
            lines.push_back({ std::move(m), fg, pad, y });
            y += lines.back().m.h + Px(2);
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        y += pad - Px(2);
    } else {
        const WidgetItem& it = g_items[item];
        // icon + name
        glyph = RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), icons, false);
        int nameX = pad + glyph.w + Px(10);
        Mask name = RenderLine(Fit(it.name, title, maxW - pad - nameX), title);
        int rowH = std::max(glyph.h, name.h);
        W = std::max(W, nameX + name.w + pad);
        int nameY = y + (rowH - name.h) / 2;
        lines.push_back({ std::move(name), fg, nameX, nameY });
        int glyphY = y + (rowH - glyph.h) / 2;
        y += rowH + Px(8);
        // big level, with the state next to it
        std::wstring level = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"?";
        Mask lv = RenderLine(level, big);
        int lvY = y;
        W = std::max(W, lv.w + 2 * pad);
        Rgb levelColor = it.low ? red : fg;
        lines.push_back({ std::move(lv), levelColor, pad, lvY });
        y += lines.back().m.h + Px(4);
        // level bar
        pct = it.percent;
        barAlpha = it.dim ? 150 : 255;  // asleep / last known value: same colours, a little faded
        barColor = it.low ? red : it.charging ? charge : it.full ? green
                 : it.accent != Accent::None ? Rgb{ AccentColor(it.accent, g_light).r, AccentColor(it.accent, g_light).g, AccentColor(it.accent, g_light).b }
                 : green;
        barY = y;
        y += Px(6) + Px(12);
        // status and extra lines
        std::vector<std::wstring> rest;
        if (!it.status.empty()) rest.push_back(it.status);
        rest.insert(rest.end(), it.info.begin(), it.info.end());
        for (const auto& t : rest) {
            Mask m = RenderLine(Fit(t, body, textMax), body);
            W = std::max(W, m.w + 2 * pad);
            lines.push_back({ std::move(m), sub, pad, y });
            y += lines.back().m.h + Px(2);
        }
        // chart: the recorded levels of the last 24 hours, then the forecast down to empty (dashed)
        forecastHours = it.percent > 0 ? it.hoursLeft : -1;
        if (g_chartOn && it.percent >= 0 && (!it.trend.empty() || forecastHours > 0)) {
            y += Px(8);
            Mask cap = RenderLine(T(forecastHours > 0 ? L"Last 24 hours and forecast" : L"Last 24 hours"), body);
            lines.push_back({ std::move(cap), sub, pad, y });
            y += lines.back().m.h + Px(6);
            chartY = y;
            chart = it.trend;
            chartNow = it.percent;
            y += Px(56) + Px(4);
            axisLeft = RenderLine(T(L"24h ago"), body);
            axisNow = RenderLine(T(L"Now"), body);
            if (forecastHours > 0) axisEnd = RenderLine(it.hoursLeftText, body);
            axisY = y;
            y += axisLeft.h;
        }
        y += pad - Px(2);
        // the icon goes first, drawn in the device's colour like the strip
        lines.insert(lines.begin(), Line{ std::move(glyph), it.accent != Accent::None
                                              ? Rgb{ AccentColor(it.accent, g_light).r, AccentColor(it.accent, g_light).g, AccentColor(it.accent, g_light).b }
                                              : fg, pad, glyphY });
    }
    W = std::min(W, maxW);
    int H = y;
    DeleteObject(icons);
    DeleteObject(title);
    DeleteObject(big);
    DeleteObject(body);

    Canvas cv(W, H);
    int radius = Px(8);
    FillRound(cv, 0, 0, W, H, radius, border, 255);
    FillRound(cv, 1, 1, W - 2, H - 2, std::max(1, radius - 1), bg, 255);
    if (barY >= 0) {
        int bw = W - 2 * pad, bh = Px(6);
        FillRound(cv, pad, barY, bw, bh, bh / 2, track, g_light ? 28 : 40);
        if (pct > 0) FillRound(cv, pad, barY, std::max(bh, bw * std::min(pct, 100) / 100), bh, bh / 2, barColor, barAlpha);
    }
    if (chartY >= 0) {
        // Time runs left to right: the last 24 hours, "now", then (when there is an estimate) the forecast down
        // to 0%. The past takes 45% of the width when a forecast is shown, so "now" never gets squeezed.
        const int cx = pad, cw = W - 2 * pad, ch = Px(56);
        const double past = 24.0 * 3600.0, maxFuture = 90.0 * 24 * 3600.0;  // the forecast side has its own width
        const double future = forecastHours > 0 ? std::min(forecastHours * 3600.0, maxFuture) : 0;
        const double pastW = future > 0 ? (cw - 1) * 0.45 : (cw - 1);
        auto X = [&](double t) {  // t: seconds from now (negative = past)
            return t <= 0 ? cx + (1.0 + std::max(t, -past) / past) * pastW : cx + pastW + std::min(t, future) / future * ((cw - 1) - pastW);
        };
        auto Y = [&](double p) { return chartY + (100.0 - std::max(0.0, std::min(p, 100.0))) / 100.0 * (ch - 1); };
        for (int yy : { chartY, chartY + ch / 2, chartY + ch - 1 })  // faint 100% / 50% / 0% guides
            for (int xx = cx; xx < cx + cw; xx++) cv.Blend(xx, yy, track, g_light ? 18 : 24);
        const int nowX = (int)std::lround(X(0));
        for (int yy = chartY; yy < chartY + ch; yy += 3) cv.Blend(nowX, yy, track, g_light ? 50 : 70);  // "now"
        auto dot = [&](double fx, double fy, int alpha) {  // anti-aliased point, for 2 px lines
            int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
            double ax = fx - x0, ay = fy - y0;
            cv.Blend(x0, y0, barColor, (int)(alpha * (1 - ax) * (1 - ay)));
            cv.Blend(x0 + 1, y0, barColor, (int)(alpha * ax * (1 - ay)));
            cv.Blend(x0, y0 + 1, barColor, (int)(alpha * (1 - ax) * ay));
            cv.Blend(x0 + 1, y0 + 1, barColor, (int)(alpha * ax * ay));
        };
        auto line = [&](double x1, double y1, double x2, double y2, int alpha, bool dashed, bool area) {
            if (area)
                for (int xx = (int)std::ceil(x1); xx <= (int)std::floor(x2); xx++) {
                    double k = x2 > x1 ? (xx - x1) / (x2 - x1) : 0;
                    for (int yy = (int)std::ceil(y1 + (y2 - y1) * k); yy < chartY + ch; yy++) cv.Blend(xx, yy, barColor, g_light ? 30 : 40);
                }
            double len = std::max(1.0, std::hypot(x2 - x1, y2 - y1));
            for (double s = 0; s <= len; s += 0.25) {
                if (dashed && std::fmod(s, (double)Px(7)) > Px(4)) continue;
                double fx = x1 + (x2 - x1) * s / len, fy = y1 + (y2 - y1) * s / len;
                dot(fx, fy - 0.5, alpha);
                dot(fx, fy + 0.5, alpha);
            }
        };
        auto disc = [&](double fx, double fy, int r, int alpha) {
            for (int yy = -r; yy <= r; yy++)
                for (int xx = -r; xx <= r; xx++)
                    if (xx * xx + yy * yy <= r * r + 1) cv.Blend((int)std::lround(fx) + xx, (int)std::lround(fy) + yy, barColor, alpha);
        };
        // the past: recorded readings joined when less than 12 hours apart (a mouse asleep overnight is not a gap), up to now
        std::vector<std::pair<double, double>> pts;
        for (const auto& c : chart) pts.push_back({ (double)c.first, (double)c.second });
        pts.push_back({ 0.0, (double)chartNow });
        for (size_t i = 0; i + 1 < pts.size(); i++) {
            if (pts[i + 1].first - pts[i].first > 12 * 3600) { disc(X(pts[i].first), Y(pts[i].second), 1, 200); continue; }
            line(X(pts[i].first), Y(pts[i].second), X(pts[i + 1].first), Y(pts[i + 1].second), 110, false, false);
        }
        // the forecast: a dashed line from now down to empty
        if (future > 0) {
            double endPct = chartNow * (1.0 - future / (forecastHours * 3600.0));
            line(X(0), Y(chartNow), X(future), Y(endPct), 90, true, false);
            disc(X(future), Y(endPct), Px(2), 140);
        }
        disc(X(0), Y(chartNow), Px(2) + 1, 255);  // the current level
        // axis labels: "24h ago" at the left, "Now" under the marker, the time to empty at the right
        lines.push_back({ std::move(axisLeft), sub, cx, axisY });
        int nowLabelX = std::max(cx, std::min(nowX - axisNow.w / 2, cx + cw - axisNow.w));
        if (future > 0) nowLabelX = std::max(nowLabelX, cx + lines.back().m.w + Px(6));
        int endX = cx + cw - axisEnd.w;
        if (future > 0 && axisEnd.w > 0) nowLabelX = std::min(nowLabelX, endX - axisNow.w - Px(6));
        if (future > 0 || nowX > cx + lines.back().m.w + Px(6)) lines.push_back({ std::move(axisNow), sub, nowLabelX, axisY });
        if (future > 0 && axisEnd.w > 0) lines.push_back({ std::move(axisEnd), fg, endX, axisY });
    }
    for (const auto& l : lines) cv.Draw(l.m, l.x, l.y, l.c, 255);

    // Above the device (below it for a top taskbar, beside it for a left / right one), kept on its monitor.
    RECT ir{};
    if (message) GetClientRect(strip, &ir);
    else ir = (*rects)[item];
    MapWindowPoints(strip, nullptr, reinterpret_cast<POINT*>(&ir), 2);
    RECT br{};
    GetWindowRect(bar, &br);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& mon = mi.rcMonitor;
    int gap = Px(8), x, top;
    bool vertical = br.bottom - br.top > br.right - br.left;
    if (vertical) {
        top = (ir.top + ir.bottom) / 2 - H / 2;
        x = br.left - mon.left < mon.right - br.right ? br.right + gap : br.left - gap - W;
    } else {
        x = (ir.left + ir.right) / 2 - W / 2;
        top = br.top - mon.top > mon.bottom - br.bottom ? br.top - gap - H : br.bottom + gap;
    }
    x = std::max((int)mon.left + gap, std::min(x, (int)mon.right - gap - W));
    top = std::max((int)mon.top + gap, std::min(top, (int)mon.bottom - gap - H));
    g_dpiBar = nullptr;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    SIZE sz{ W, H };
    POINT src{ 0, 0 }, dst{ x, top };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_card, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    SetWindowPos(g_card, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_cardItem = item;
    g_cardStrip = strip;
}

// Hover highlight for the strip under the mouse: the primary one or a mirror.
void SetHover(HWND h, bool on) {
    if (h == g_wnd) {
        if (g_hover == on) return;
        g_hover = on;
        Paint();
        widget::Reposition();
        return;
    }
    for (auto& m : g_mirrors)
        if (m.wnd == h && m.hover != on) {
            m.hover = on;
            PaintMirror(m);
            PositionMirror(m);
        }
}

// Only the main strip, next to the clock on a horizontal left-to-right taskbar: the other placements
// (far left, before apps, vertical, RTL, other monitors) have no offset to save.
bool CanDrag(HWND h) {
    return h == g_wnd && !g_myFinder && !g_vertical && !g_farLeft && !g_beforeApps && g_anchorLeft >= 0 &&
           !(GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL);
}

void BeginDrag(HWND h) {
    g_dragging = true;
    KillTimer(h, kCardTimer);
    g_hoverItem = -1;
    HideCard();
    RECT r;
    GetWindowRect(h, &r);
    POINT c;
    GetCursorPos(&c);
    g_grabDx = c.x - r.left;
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32644)));  // IDC_SIZEWE
}

void DragTo(HWND h, POINT c) {
    RECT bar;
    GetClientRect(g_taskbar, &bar);
    POINT p{ c.x - g_grabDx, 0 };
    ScreenToClient(g_taskbar, &p);
    // Never right of the plain position: that is the tray.
    int x = std::max(0, std::min((int)p.x, std::min(g_anchorLeft, (int)bar.right - (int)g_size.cx)));
    SetWindowPos(h, HWND_TOP, x, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE);
}

void Drop(HWND h) {
    g_dragging = false;
    RECT r;
    GetWindowRect(h, &r);
    POINT p{ r.left, r.top };
    ScreenToClient(g_taskbar, &p);
    int px = std::max(0, MulDiv(g_anchorLeft - (int)p.x, 96, Dpi()));
    DWORD v = (DWORD)px;
    if (px) RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", REG_DWORD, &v, sizeof(v));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset");
    g_lastLeft = -1;  // settle on the saved offset
    widget::Reposition();
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            if (CanDrag(h)) {
                g_pressed = true;
                GetCursorPos(&g_pressAt);
                SetCapture(h);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (g_dragging && (HWND)lp != h) Drop(h);  // capture taken away (Esc, another window): keep where it is
            g_pressed = false;
            return 0;
        case WM_MOUSEMOVE: {
            if (g_pressed) {
                POINT c;
                GetCursorPos(&c);
                if (!g_dragging && (abs(c.x - g_pressAt.x) > GetSystemMetrics(SM_CXDRAG) ||
                                    abs(c.y - g_pressAt.y) > GetSystemMetrics(SM_CYDRAG)))
                    BeginDrag(h);
                if (g_dragging) {
                    DragTo(h, c);
                    return 0;
                }
            }
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            SetHover(h, true);
            if (!g_cardOn) return 0;  // card turned off: the plain tooltip shows the summary instead
            POINT p{ (short)LOWORD(lp), (short)HIWORD(lp) };
            int k = ItemAt(h, p);
            if (k != g_hoverItem || h != g_cardStrip) {
                g_hoverItem = k;
                if (g_cardItem != -1 && k != -1) {
                    ShowCard(h, k);  // already open: follow the mouse to the next device right away
                } else {
                    KillTimer(h, kCardTimer);
                    if (k == -1) HideCard();
                    else SetTimer(h, kCardTimer, kCardDelayMs, nullptr);
                }
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kCardTimer) {
                KillTimer(h, kCardTimer);
                if (g_hoverItem != -1) ShowCard(h, g_hoverItem);
            } else if (wp == kChargeTimer) {
                PaintAll();
                widget::Reposition();
            } else if (wp == kFinderTimer) {
                widget::Reposition();
            }
            return 0;
        case WM_MOUSELEAVE:
            SetHover(h, false);
            KillTimer(h, kCardTimer);
            g_hoverItem = -1;
            HideCard();
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            KillTimer(h, kCardTimer);
            HideCard();
            bool dragged = g_dragging;
            g_pressed = false;
            if (dragged) Drop(h);  // a drag is not a click
            if (msg == WM_LBUTTONUP && GetCapture() == h) ReleaseCapture();
            if (!dragged) PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        // MyFinder's strip is a top-level popup: keep it a plain clickable field.
        case WM_SHOWWINDOW:
            if (g_myFinder && lp != 0) return 0;
            break;
        case WM_NCHITTEST:
            if (g_myFinder) return HTCLIENT;
            break;
        case WM_NCLBUTTONDBLCLK:
            if (g_myFinder) return 0;
            break;
        case WM_SYSCOMMAND:
            if (g_myFinder) {
                auto command = wp & 0xfff0;
                if (command == SC_MAXIMIZE || command == SC_MINIMIZE || command == SC_RESTORE || command == SC_MOVE ||
                    command == SC_SIZE)
                    return 0;
            }
            break;
        case WM_ERASEBKGND:
            if (g_myFinder && g_compositionReady) return 1;
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

void SetMyFinder(bool on) {
    if (g_myFinder == on) return;
    Destroy();
    g_myFinder = on;
}

wchar_t DeviceGlyph(DeviceKind kind, bool bluetooth) { return Glyph(kind, bluetooth); }

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(g_myFinder ? L"MyFinderApp" : L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || (!g_myFinder && !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr))) {
        g_taskbar = nullptr;
        return false;
    }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    g_vertical = tb.bottom - tb.top > tb.right - tb.left;  // left / right taskbar: devices stacked in a column
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = g_myFinder || FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;
    g_inst = inst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    DWORD exStyle = WS_EX_NOACTIVATE | (g_myFinder ? WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP : WS_EX_LAYERED);
    DWORD style = g_myFinder ? WS_POPUP : WS_CHILD | WS_CLIPSIBLINGS;
    g_wnd = CreateWindowExW(exStyle, kClass, L"", style, 0, 0, 1, 1, g_myFinder ? nullptr : g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }
    if (g_myFinder) {
        g_compositionReady = g_composition.Initialize(g_wnd);
        if (!g_compositionReady) {
            Destroy();
            return false;
        }
        g_finderOrderHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, FinderOrderEvent, 0, 0,
                                            WINEVENT_OUTOFCONTEXT);
        g_finderForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FinderOrderEvent,
                                                 0, 0, WINEVENT_OUTOFCONTEXT);
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
        SendMessageW(g_tip, TTM_ACTIVATE, !g_cardOn, 0);  // the card replaces it while the card is on
    }
    SetSummary(g_wnd);
    if (!g_myFinder) WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    if (g_myFinder) {
        g_orderGuard.Start(g_wnd, g_taskbar);
        WatchFinder(g_taskbar);
        SetTimer(g_wnd, kFinderTimer, 125, nullptr);
    }
    Reposition();  // shows the strip once it is drawn and placed; it stays hidden until then
    return true;
}

void Destroy() {
    g_lastValidFinderPlacement = 0;
    g_orderGuard.Stop();
    if (g_finderOrderHook) UnhookWinEvent(g_finderOrderHook);
    if (g_finderForegroundHook) UnhookWinEvent(g_finderForegroundHook);
    g_finderOrderHook = g_finderForegroundHook = nullptr;
    g_finderOrderQueued = false;
    WatchFinder(nullptr);
    g_anchorLeft = -1;
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    ReserveBottom(0);  // give the taskbar its full height back
    g_fill = false;
    for (auto& m : g_mirrors) DestroyMirror(m);
    g_mirrors.clear();
    g_primaryHidden = false;  // the next Create() decides again
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_card) { DestroyWindow(g_card); g_card = nullptr; }
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    g_cardItem = g_hoverItem = -1;
    g_cardStrip = nullptr;
    g_composition.Reset();
    g_compositionReady = false;
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hover = false;
    g_painted = false;
    g_size = {};
    g_paintedBarClient = {};
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar) && (!g_myFinder || g_compositionReady); }

// The 100 ms repaint runs only while the animation is on and something is charging.
void UpdateChargeTimer() {
    if (!g_wnd) return;
    bool any = false;
    for (const auto& it : g_items) any |= it.charging && !it.full;
    if (g_animateCharging && any) SetTimer(g_wnd, kChargeTimer, 100, nullptr);
    else KillTimer(g_wnd, kChargeTimer);
}

void SetAnimateCharging(bool on) {
    g_animateCharging = on;
    UpdateChargeTimer();
    PaintAll();
    if (g_wnd) Reposition();
}

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    if (tooltip != g_tipText) {
        g_tipText = tooltip;
        if (g_wnd) SetSummary(g_wnd);
        for (const auto& m : g_mirrors) SetSummary(m.wnd);
    }
    PaintAll();
    Reposition();
    UpdateChargeTimer();
    if (g_cardItem != -1 && g_cardStrip && IsWindow(g_cardStrip)) {  // keep an open card up to date
        if (g_cardItem == kCardMessage ? g_items.empty() : g_cardItem < (int)g_items.size()) ShowCard(g_cardStrip, g_cardItem);
        else HideCard();
    }
}

void SetCardOptions(bool card, bool chart) {
    g_cardOn = card;
    g_chartOn = chart;
    if (g_tip) SendMessageW(g_tip, TTM_ACTIVATE, !card, 0);
    if (!card) HideCard();
    else if (g_cardItem != -1 && g_cardStrip && IsWindow(g_cardStrip)) ShowCard(g_cardStrip, g_cardItem);
}

void SetLightTheme(bool light) {
    if (g_myFinder) {  // follow the glyphs MyFinder draws, not the Windows theme
        auto foreground = FinderForeground(g_taskbar);
        if (foreground != myfinder::Foreground::Unknown) light = foreground == myfinder::Foreground::Dark;
    }
    g_light = light;
    ApplyTipTheme();
    PaintAll();
    if (g_cardItem != -1 && g_cardStrip && IsWindow(g_cardStrip)) ShowCard(g_cardStrip, g_cardItem);
}

void SetBackground(bool on) {
    g_background = on;
    PaintAll();
}

// One line of text with the strip's content (PR #369, for status bars such as YASB): glyph, level, a bolt
// while charging, "~" for a last known value and "!" when low.
std::wstring ItemsToText(const std::vector<WidgetItem>& items) {
    std::wstring out;
    for (const auto& it : items) {
        if (!out.empty()) out += L"  ";
        out += Glyph(it.kind, it.bluetooth);
        out += L' ';
        if (it.dim && it.percent >= 0) out += L'~';
        out += !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"?";
        if (it.charging) {
            out += L' ';
            out += kBolt;
        }
        if (it.low) out += L'!';
    }
    return out;
}

void SetConnectionIcons(bool on) {
    g_connectionIcons = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetRingStyle(bool on) {
    g_ringStyle = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetShowPercentSign(bool on) {
    g_showPercentSign = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetStacked(bool on) {
    g_stacked = on;
    PaintAll();
    if (g_wnd) Reposition();
}

void SetMakeRoom(bool on) {
    g_makeRoom = on;
    g_lastW = -1;  // force a re-position
    if (g_wnd) Reposition();
}

void SetVerticalPlace(VerticalPlace place) {
    g_place = place;
    if (g_wnd) Reposition();
}

bool Vertical() { return g_wnd && g_vertical; }

void ApplyLeftSpot() {
    bool on = g_farLeft || g_beforeApps;
    AcquireSRWLockExclusive(&g_wbLock);
    if (g_wbLeftSpot != on) {
        g_wbLeftSpot = on;
        ++g_wbGeneration;
    }
    ReleaseSRWLockExclusive(&g_wbLock);
    if (g_wbWake) SetEvent(g_wbWake);
    if (g_wnd) Reposition();
}

void SetFarLeft(bool on) {
    g_farLeft = on;
    ApplyLeftSpot();
}

void SetBeforeApps(bool on) {
    g_beforeApps = on;
    ApplyLeftSpot();
}

void SetMonitorChoice(const std::wstring& choice) {
    g_choice = choice.empty() ? L"all" : choice;
    SyncMirrors();  // adds, removes or hides strips right away
    if (g_wnd) Reposition();
}

int FindMonitor(const std::vector<std::wstring>& ids, const std::wstring& choice) {
    for (size_t i = 0; i < ids.size(); i++)
        if (ids[i] == choice) return (int)i;
    std::wstring model = MonitorModel(choice);
    int found = -1;
    if (!model.empty())
        for (size_t i = 0; i < ids.size(); i++)
            if (MonitorModel(ids[i]) == model) {
                if (found >= 0) return -1;  // two monitors of that model: can't tell which one was chosen
                found = (int)i;
            }
    return found;
}

std::vector<MonitorOption> OtherMonitors() {
    std::vector<MonitorOption> out;
    for (HWND b : SecondaryTaskbars()) out.push_back({ MonitorId(b), MonitorLabel(b) });
    std::sort(out.begin(), out.end(), [](const MonitorOption& a, const MonitorOption& b) { return a.label < b.label; });
    return out;
}

// Other apps can put their own small windows into the taskbar next to the notification area (network
// graphs, countdowns, ...). They are visible child windows of the taskbar owned by neither Explorer nor
// us. Starting at `edge` (where the strip's trailing side would go), steps past every such window packed
// against it, so the strips sit side by side. A window already sitting next to the edge keeps its spot
// and the strip goes beside it. If another app does the same, the first of the two to re-position takes
// the outer spot and the other one then stays put, so they settle within a second.
int StepPastOtherWidgets(int edge, const RECT& bar, bool vertical, bool rtl) {
    if (!g_makeRoom || !g_wnd) return edge;
    DWORD explorer = 0, pid = 0;
    GetWindowThreadProcessId(g_taskbar, &explorer);
    const DWORD self = GetCurrentProcessId();
    const LONG span = vertical ? bar.bottom - bar.top : bar.right - bar.left;
    // Candidates: the taskbar's own child windows, and (from PR #1970) top-level windows of other apps that
    // float over the taskbar, lying entirely inside it (e.g. a monitor gadget docked onto the taskbar).
    std::vector<RECT> others;
    auto consider = [&](HWND c, bool topLevel) {
        if (c == g_wnd || !IsWindowVisible(c)) return;
        GetWindowThreadProcessId(c, &pid);
        RECT r;
        if (pid == explorer || pid == self || !GetWindowRect(c, &r) || r.right <= r.left || r.bottom <= r.top) return;
        // A window docked over the taskbar has to be topmost to show there; anything else is just behind it.
        if (topLevel && (!(GetWindowLongW(c, GWL_EXSTYLE) & WS_EX_TOPMOST) || r.left < bar.left || r.right > bar.right ||
                         r.top < bar.top || r.bottom > bar.bottom))
            return;
        if ((vertical ? r.bottom - r.top : r.right - r.left) > span / 3) return;  // not a small widget
        others.push_back(r);
    };
    for (HWND c = GetWindow(g_taskbar, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) consider(c, false);
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) consider(w, true);
    for (int pass = 0; pass < 8; pass++) {
        bool moved = false;
        for (const RECT& r : others) {
            if (vertical) {
                if (r.right > bar.left && r.left < bar.right && r.bottom >= edge - Px(16) && r.bottom <= edge + Px(4) &&
                    r.top < edge)
                    edge = r.top, moved = true;
            } else if (r.bottom > bar.top && r.top < bar.bottom) {
                if (!rtl && r.right >= edge - Px(16) && r.right <= edge + Px(4) && r.left < edge)
                    edge = r.left, moved = true;
                else if (rtl && r.left <= edge + Px(16) && r.left >= edge - Px(4) && r.right > edge)
                    edge = r.right, moved = true;
            }
        }
        if (!moved) break;
    }
    return edge;
}

bool Reposition() {
    if (!Alive()) return false;
    if (g_dragging) return true;  // the mouse places the strip until the drop
    SyncMirrors();
    if (g_primaryHidden) return true;  // only another monitor's taskbar shows the strip
    if (g_myFinder) {
        if (Dpi() != g_lastDpi) { g_lastDpi = Dpi(); Paint(); }
        auto foreground = FinderForeground(g_taskbar);
        if (foreground != myfinder::Foreground::Unknown && g_light != (foreground == myfinder::Foreground::Dark)) {
            g_light = foreground == myfinder::Foreground::Dark;
            ApplyTipTheme();
            Paint();
        }
        RECT bar{};
        const auto now = GetTickCount64();
        const bool keepPlacement = IsWindowVisible(g_wnd) && g_lastValidFinderPlacement && now - g_lastValidFinderPlacement < 400;
        if (!GetClientRect(g_taskbar, &bar) || !IsWindowVisible(g_taskbar) || IsIconic(g_taskbar) || bar.right <= 0 ||
            bar.bottom <= 0) {
            if (!keepPlacement && IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);
            return true;
        }
        if (!g_painted || bar.bottom != g_size.cy) Paint();
        RECT screenBar;
        GetWindowRect(g_taskbar, &screenBar);
        int edge = FinderEdge(g_taskbar, screenBar);
        int left = edge - g_size.cx - Px(8);
        if (edge < 0 || left < 0) {  // not sampled yet, or no indicators found: stay hidden
            if (!keepPlacement && IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);
            return true;
        }
        g_lastValidFinderPlacement = now;
        POINT origin{ left, 0 };
        ClientToScreen(g_taskbar, &origin);
        const bool hostTopmost = (GetWindowLongPtrW(g_taskbar, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool stripTopmost = (GetWindowLongPtrW(g_wnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if (hostTopmost != stripTopmost)
            SetWindowPos(g_wnd, hostTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        HWND above = GetWindow(g_taskbar, GW_HWNDPREV);
        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (!IsWindowVisible(g_wnd)) flags |= SWP_SHOWWINDOW;
        if (!FinderAboveStrip()) flags |= SWP_NOZORDER;
        else if (!above || ((GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != hostTopmost)
            above = hostTopmost ? HWND_TOPMOST : HWND_TOP;
        RECT current{};
        GetWindowRect(g_wnd, &current);
        if (current.left == origin.x && current.top == origin.y) flags |= SWP_NOMOVE;
        if (current.right - current.left == g_size.cx && current.bottom - current.top == g_size.cy) flags |= SWP_NOSIZE;
        // An unchanged visible strip needs no window-manager transaction.
        if ((flags & (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER)) != (SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER) ||
            (flags & SWP_SHOWWINDOW))
            SetWindowPos(g_wnd, above, origin.x, origin.y, g_size.cx, g_size.cy, flags);
        return true;
    }
    RECT bar{}, client{};
    if (!GetWindowRect(g_taskbar, &bar) || !GetClientRect(g_taskbar, &client)) return true;
    int clientW = client.right - client.left, clientH = client.bottom - client.top;
    if (clientW <= 0 || clientH <= 0) return true;  // taskbar not laid out yet
    // The taskbar can be moved between a horizontal and a vertical edge while the app runs.
    bool vertical = bar.bottom - bar.top > bar.right - bar.left;
    int dpi = Dpi();
    if (!g_painted || g_size.cx <= 0 || g_size.cy <= 0 || dpi != g_lastDpi || vertical != g_vertical ||
        g_paintedBarClient.cx != clientW || g_paintedBarClient.cy != clientH) {
        g_lastDpi = dpi;
        g_vertical = vertical;
        Paint();
    }
    if (!g_painted || g_size.cx <= 0 || g_size.cy <= 0) return true;
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tr{};
    if (!tray || !GetWindowRect(tray, &tr)) return true;
    if (tr.right <= tr.left || tr.bottom <= tr.top) return true;
    if (tr.right <= bar.left || tr.left >= bar.right || tr.bottom <= bar.top || tr.top >= bar.bottom)
        return true;
    if (g_vertical) {
        // Left or right taskbar (PR #321): the column sits just above the notification area, centred in
        // the bar's width.
        POINT anchor{ bar.left, StepPastOtherWidgets(tr.top, bar, true, false) };
        ScreenToClient(g_taskbar, &anchor);
        int top = anchor.y - g_size.cy - Px(2);
        DWORD offset = 0, osz = sizeof(offset);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset,
                         &osz) == ERROR_SUCCESS)
            top -= Px((int)offset);
        top = std::clamp(top, 0, (int)std::max<LONG>(0, client.bottom - g_size.cy));
        // Windows 11: the centred task list grows down to the tray and would end up under the column.
        // When that happens, move to the free end of the bar above the first button, if the column fits.
        // The menu can also pin the column to the top, keep it above the tray, or give it the bottom of the bar.
        AcquireSRWLockShared(&g_wbLock);
        Column col = g_column;
        ReleaseSRWLockShared(&g_wbLock);
        bool bottom = g_place == widget::VerticalPlace::Bottom && !g_rebar && g_size.cy > 0 &&
                      g_size.cy < client.bottom / 2;
        bool wasReserved = g_reserved;  // the content has had a pass to move up
        ReserveBottom(bottom ? g_size.cy : 0);
        COLORREF fill = 0;
        bool sampled = bottom && wasReserved && SampleBarColor(bar, bar.bottom - g_size.cy, fill);
        bool changed = sampled && (!g_fill || abs(GetRValue(fill) - GetRValue(g_fillColor)) > 4 ||
                                   abs(GetGValue(fill) - GetGValue(g_fillColor)) > 4 ||
                                   abs(GetBValue(fill) - GetBValue(g_fillColor)) > 4);
        if (changed || (!bottom && g_fill)) {
            g_fill = bottom;
            g_fillColor = fill;
            Paint();
        }
        if (bottom) {
            top = client.bottom - g_size.cy;
        } else if (g_place == widget::VerticalPlace::Top) {
            top = 0;
        } else if (g_place == widget::VerticalPlace::Auto && !g_rebar && col.bar == g_taskbar && col.measured &&
                   col.any) {
            POINT first{ bar.left, col.top }, last{ bar.left, col.bottom };
            ScreenToClient(g_taskbar, &first);
            ScreenToClient(g_taskbar, &last);
            if (last.y > top && first.y < top + g_size.cy && g_size.cy + Px(2) <= first.y) top = 0;
        }
        int left = (int)std::max<LONG>(0, (client.right - g_size.cx) / 2);
        // Windows 10: shorten the task-button area when it would reach behind the column.
        if (g_rebar && IsWindow(g_rebar)) {
            RECT rr{};
            GetWindowRect(g_rebar, &rr);
            POINT tl{ rr.left, rr.top };
            ScreenToClient(g_taskbar, &tl);
            if (tl.y < top && tl.y + (rr.bottom - rr.top) > top)
                MoveWindow(g_rebar, tl.x, tl.y, rr.right - rr.left, top - tl.y, TRUE);
        }
        bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
        if (!covered && Shown(g_wnd) && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) &&
            g_lastW == g_size.cy && g_lastLeft == top)
            return true;
        g_lastTray = tr;
        g_lastBar = bar;
        g_lastW = g_size.cy;
        g_lastLeft = top;
        SetWindowPos(g_wnd, HWND_TOP, left, top, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
    ReserveBottom(0);  // the taskbar was moved from a vertical edge to a horizontal one
    if (g_fill) { g_fill = false; Paint(); }
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int edge = rtl ? tr.right : tr.left;  // screen x we sit next to
    // Windows 10: News and Interests / Weather sits immediately left of the tray. The window that hosts it
    // depends on the build: ShellFeedsTaskbarViewWindow on some, DynamicContent2 on others (e.g. 19045,
    // where the search highlights button is DynamicContent1). Both are direct children of the taskbar and
    // only a visible one counts. Its rectangle is read on every pass because the weather text changes its
    // width, so the widget follows it.
    if (!rtl)
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    // Step past the Widgets button when it sits where we would go.
    RECT wb = WidgetsButtonRect();
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    edge = StepPastOtherWidgets(edge, bar, false, rtl);
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    int left = p.x - g_size.cx - Px(2);
    g_anchorLeft = left;

    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
        left -= Px((int)customOffset);
    }

    // Clamp only out-of-range positions. At x=0 the widget can be obscured by Start/XAML,
    // so prefer its previous valid position or the space beside a normally anchored tray (#2993).
    RECT taskbarClient{};
    if (GetClientRect(g_taskbar, &taskbarClient)) {
        left = RecoverHorizontalWidgetX(left, g_size.cx, taskbarClient.right - taskbarClient.left,
                                        tr.right - tr.left, g_lastLeft, Px(2));
    } else if (left < 0 && g_lastLeft >= 0) {
        left = g_lastLeft;
    }

    int farX = 0;
    bool atFarLeft = !rtl && (g_farLeft || g_beforeApps) && FarLeftX(g_taskbar, g_size.cx, farX);
    if (atFarLeft) {
        POINT fp{ farX, bar.top };
        ScreenToClient(g_taskbar, &fp);
        left = fp.x;
    }

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (!atFarLeft && g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && Shown(g_wnd) && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) &&
        EqualRect(&wb, &g_lastWidgets) && g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace widget