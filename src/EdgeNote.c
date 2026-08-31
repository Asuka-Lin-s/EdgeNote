// EdgeNote - native Win32 sticky note for Windows 10/11 x64
// True borderless window, manual resize, four-edge auto-hide,
// multiple editable notes in one process, per-note colors.

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdlib.h>

#pragma comment(lib, "Comdlg32.lib")

#define ID_EDIT 1001

#define IDM_NEWNOTE 2000
#define IDM_TOPMOST 2001
#define IDM_AUTOHIDE 2002
#define IDM_FONT_PLUS 2003
#define IDM_FONT_MINUS 2004
#define IDM_UNDOCK 2005
#define IDM_CLEAR 2006
#define IDM_EXIT 2007

#define IDM_COLOR_YELLOW 2100
#define IDM_COLOR_PINK   2101
#define IDM_COLOR_BLUE   2102
#define IDM_COLOR_GREEN  2103
#define IDM_COLOR_PURPLE 2104
#define IDM_COLOR_ORANGE 2105
#define IDM_COLOR_CUSTOM 2110

#define TIMER_EDGE 1

#define EDGE_NONE 0
#define EDGE_LEFT 1
#define EDGE_RIGHT 2
#define EDGE_TOP 3
#define EDGE_BOTTOM 4

#define DRAG_NONE 0
#define DRAG_MOVE 1
#define DRAG_SIZE 2

#define SNAP_DISTANCE 32
#define HIDDEN_SLIVER 4
#define EDGE_TRIGGER 3
#define HIDE_TICKS 7
#define MIN_W 220
#define MIN_H 150
#define TITLE_H 34
#define RESIZE_BORDER 7
#define TITLE_BTN_W 38
#define COLOR_PRESET_COUNT 6

typedef struct NoteColorPreset {
    COLORREF body;
    COLORREF title;
} NoteColorPreset;

typedef struct NoteState {
    HWND hwnd;
    HWND edit;
    HBRUSH bodyBrush;
    HBRUSH titleBrush;
    HFONT editFont;

    RECT visibleRect;
    int dockEdge;
    int hidden;
    int hideCounter;
    int alwaysOnTop;
    int autoHide;
    int fontSize;
    int inMoveSize;
    int menuOpen;

    int dragMode;
    int dragHit;
    POINT dragStart;
    RECT dragRect;

    int colorIndex;
    COLORREF bodyColor;
    COLORREF titleColor;
    COLORREF textColor;
    COLORREF titleTextColor;
    COLORREF customColors[16];
} NoteState;

static const NoteColorPreset g_presets[COLOR_PRESET_COUNT] = {
    { RGB(247,229,142), RGB(228,201, 85) },
    { RGB(250,213,225), RGB(232,169,193) },
    { RGB(211,231,250), RGB(154,194,232) },
    { RGB(216,240,211), RGB(160,208,151) },
    { RGB(231,216,248), RGB(191,159,226) },
    { RGB(252,224,190), RGB(235,181,113) }
};

static HINSTANCE g_instance = NULL;
static int g_noteCount = 0;
static int g_nextColor = 1;
static WNDPROC g_originalEditProc = NULL;

static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int notches = delta / WHEEL_DELTA;
        UINT lines = 3;
        int scrollLines;

        if (notches == 0)
            notches = delta > 0 ? 1 : -1;

        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);

        if (lines == WHEEL_PAGESCROLL) {
            RECT rc;
            HDC dc;
            TEXTMETRICW tm;
            int lineHeight = 18;
            int pageLines;

            GetClientRect(hwnd, &rc);
            dc = GetDC(hwnd);
            if (dc) {
                if (GetTextMetricsW(dc, &tm))
                    lineHeight = tm.tmHeight > 0 ? tm.tmHeight : 18;
                ReleaseDC(hwnd, dc);
            }

            pageLines = (rc.bottom - rc.top) / lineHeight;
            if (pageLines < 1) pageLines = 1;
            scrollLines = -notches * pageLines;
        } else {
            if (lines < 1) lines = 3;
            scrollLines = -notches * (int)lines;
        }

        SendMessageW(hwnd, EM_LINESCROLL, 0, scrollLines);
        return 0;
    }

    return CallWindowProcW(g_originalEditProc, hwnd, msg, wParam, lParam);
}

static NoteState *note_from(HWND hwnd) {
    return (NoteState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static COLORREF contrast_text(COLORREF c) {
    int r = GetRValue(c);
    int g = GetGValue(c);
    int b = GetBValue(c);
    int luminance = (299 * r + 587 * g + 114 * b) / 1000;
    return luminance < 140 ? RGB(250,250,250) : RGB(47,44,30);
}

static COLORREF darken_color(COLORREF c) {
    return RGB(
        (GetRValue(c) * 82) / 100,
        (GetGValue(c) * 82) / 100,
        (GetBValue(c) * 82) / 100
    );
}

static void apply_preset_values(NoteState *n, int index) {
    if (index < 0 || index >= COLOR_PRESET_COUNT) index = 0;
    n->colorIndex = index;
    n->bodyColor = g_presets[index].body;
    n->titleColor = g_presets[index].title;
    n->textColor = contrast_text(n->bodyColor);
    n->titleTextColor = contrast_text(n->titleColor);
}

static void get_monitor_info_for(HMONITOR mon, MONITORINFO *mi) {
    ZeroMemory(mi, sizeof(*mi));
    mi->cbSize = sizeof(*mi);
    GetMonitorInfoW(mon, mi);
}

static HMONITOR monitor_for_note(NoteState *n) {
    POINT p;
    p.x = (n->visibleRect.left + n->visibleRect.right) / 2;
    p.y = (n->visibleRect.top + n->visibleRect.bottom) / 2;
    return MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
}

static HWND z_after(NoteState *n) {
    return n->alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
}

static void rebuild_brushes(NoteState *n) {
    HBRUSH body = CreateSolidBrush(n->bodyColor);
    HBRUSH title = CreateSolidBrush(n->titleColor);
    HBRUSH oldBody;
    HBRUSH oldTitle;

    if (!body || !title) {
        if (body) DeleteObject(body);
        if (title) DeleteObject(title);
        return;
    }

    oldBody = n->bodyBrush;
    oldTitle = n->titleBrush;
    n->bodyBrush = body;
    n->titleBrush = title;

    if (n->hwnd) InvalidateRect(n->hwnd, NULL, TRUE);
    if (n->edit) InvalidateRect(n->edit, NULL, TRUE);

    if (oldBody) DeleteObject(oldBody);
    if (oldTitle) DeleteObject(oldTitle);
}

static void set_preset_color(NoteState *n, int index) {
    apply_preset_values(n, index);
    rebuild_brushes(n);
}

static void set_custom_color(NoteState *n, COLORREF body) {
    n->colorIndex = -1;
    n->bodyColor = body;
    n->titleColor = darken_color(body);
    n->textColor = contrast_text(n->bodyColor);
    n->titleTextColor = contrast_text(n->titleColor);
    rebuild_brushes(n);
}

static void choose_custom_color(NoteState *n) {
    CHOOSECOLORW cc;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = n->hwnd;
    cc.rgbResult = n->bodyColor;
    cc.lpCustColors = n->customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;

    n->menuOpen = 1;
    if (ChooseColorW(&cc)) set_custom_color(n, cc.rgbResult);
    n->menuOpen = 0;
}

static void update_font(NoteState *n) {
    HDC dc;
    int dpi;
    HFONT f;

    dc = GetDC(n->hwnd);
    dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(n->hwnd, dc);

    f = CreateFontW(
        -MulDiv(n->fontSize, dpi, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI"
    );

    if (f) {
        SendMessageW(n->edit, WM_SETFONT, (WPARAM)f, TRUE);
        if (n->editFont) DeleteObject(n->editFont);
        n->editFont = f;
    }
}

static void layout_edit(NoteState *n) {
    RECT cr;
    int pad;
    int ew;
    int eh;

    if (!n->edit) return;
    GetClientRect(n->hwnd, &cr);

    pad = RESIZE_BORDER;
    ew = cr.right - pad * 2;
    eh = cr.bottom - TITLE_H - pad;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;

    MoveWindow(n->edit, pad, TITLE_H, ew, eh, TRUE);
}

static void place_visible_for_dock(NoteState *n) {
    MONITORINFO mi;
    int w;
    int h;
    int x;
    int y;

    get_monitor_info_for(monitor_for_note(n), &mi);
    w = n->visibleRect.right - n->visibleRect.left;
    h = n->visibleRect.bottom - n->visibleRect.top;
    x = n->visibleRect.left;
    y = n->visibleRect.top;

    if (n->dockEdge == EDGE_LEFT) x = mi.rcWork.left;
    else if (n->dockEdge == EDGE_RIGHT) x = mi.rcWork.right - w;
    else if (n->dockEdge == EDGE_TOP) y = mi.rcWork.top;
    else if (n->dockEdge == EDGE_BOTTOM) y = mi.rcWork.bottom - h;

    SetWindowPos(n->hwnd, z_after(n), x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    GetWindowRect(n->hwnd, &n->visibleRect);
}

static void show_from_edge(NoteState *n) {
    if (!n->hidden) return;
    place_visible_for_dock(n);
    n->hidden = 0;
    n->hideCounter = 0;
}

static void hide_to_edge(NoteState *n) {
    MONITORINFO mi;
    int w;
    int h;
    int x;
    int y;

    if (n->dockEdge == EDGE_NONE || !n->autoHide) return;

    get_monitor_info_for(monitor_for_note(n), &mi);
    w = n->visibleRect.right - n->visibleRect.left;
    h = n->visibleRect.bottom - n->visibleRect.top;
    x = n->visibleRect.left;
    y = n->visibleRect.top;

    if (n->dockEdge == EDGE_LEFT)
        x = mi.rcMonitor.left - w + HIDDEN_SLIVER;
    else if (n->dockEdge == EDGE_RIGHT)
        x = mi.rcMonitor.right - HIDDEN_SLIVER;
    else if (n->dockEdge == EDGE_TOP)
        y = mi.rcMonitor.top - h + HIDDEN_SLIVER;
    else if (n->dockEdge == EDGE_BOTTOM)
        y = mi.rcMonitor.bottom - HIDDEN_SLIVER;

    SetWindowPos(n->hwnd, z_after(n), x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    n->hidden = 1;
    n->hideCounter = 0;
}

static int point_in_rect_margin(POINT p, RECT r, int margin) {
    return p.x >= r.left - margin && p.x <= r.right + margin &&
           p.y >= r.top - margin && p.y <= r.bottom + margin;
}

static void edge_timer(NoteState *n) {
    POINT p;
    MONITORINFO mi;

    if (n->dockEdge == EDGE_NONE || !n->autoHide || n->inMoveSize || n->menuOpen)
        return;
    if (!GetCursorPos(&p)) return;

    get_monitor_info_for(monitor_for_note(n), &mi);

    if (n->hidden) {
        int hit = 0;
        if (n->dockEdge == EDGE_LEFT &&
            p.x <= mi.rcMonitor.left + EDGE_TRIGGER &&
            p.y >= n->visibleRect.top - 20 && p.y <= n->visibleRect.bottom + 20)
            hit = 1;
        else if (n->dockEdge == EDGE_RIGHT &&
                 p.x >= mi.rcMonitor.right - 1 - EDGE_TRIGGER &&
                 p.y >= n->visibleRect.top - 20 && p.y <= n->visibleRect.bottom + 20)
            hit = 1;
        else if (n->dockEdge == EDGE_TOP &&
                 p.y <= mi.rcMonitor.top + EDGE_TRIGGER &&
                 p.x >= n->visibleRect.left - 20 && p.x <= n->visibleRect.right + 20)
            hit = 1;
        else if (n->dockEdge == EDGE_BOTTOM &&
                 p.y >= mi.rcMonitor.bottom - 1 - EDGE_TRIGGER &&
                 p.x >= n->visibleRect.left - 20 && p.x <= n->visibleRect.right + 20)
            hit = 1;

        if (hit) show_from_edge(n);
    } else {
        RECT r;
        GetWindowRect(n->hwnd, &r);
        if (!point_in_rect_margin(p, r, 8)) {
            if (++n->hideCounter >= HIDE_TICKS) hide_to_edge(n);
        } else {
            n->hideCounter = 0;
        }
    }
}

static void detect_and_snap(NoteState *n) {
    RECT r;
    POINT center;
    MONITORINFO mi;
    int dl;
    int dr;
    int dt;
    int db;
    int best;
    int edge;

    GetWindowRect(n->hwnd, &r);
    center.x = (r.left + r.right) / 2;
    center.y = (r.top + r.bottom) / 2;
    get_monitor_info_for(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &mi);

    dl = abs(r.left - mi.rcWork.left);
    dr = abs(mi.rcWork.right - r.right);
    dt = abs(r.top - mi.rcWork.top);
    db = abs(mi.rcWork.bottom - r.bottom);
    best = SNAP_DISTANCE + 1;
    edge = EDGE_NONE;

    if (dl < best) { best = dl; edge = EDGE_LEFT; }
    if (dr < best) { best = dr; edge = EDGE_RIGHT; }
    if (dt < best) { best = dt; edge = EDGE_TOP; }
    if (db < best) { best = db; edge = EDGE_BOTTOM; }

    n->dockEdge = edge;
    n->hidden = 0;
    n->visibleRect = r;
    if (edge != EDGE_NONE) place_visible_for_dock(n);
}

static int resize_hit_test(int x, int y, int w, int h) {
    int L = x < RESIZE_BORDER;
    int R = x >= w - RESIZE_BORDER;
    int T = y < RESIZE_BORDER;
    int B = y >= h - RESIZE_BORDER;

    if (T && L) return HTTOPLEFT;
    if (T && R) return HTTOPRIGHT;
    if (B && L) return HTBOTTOMLEFT;
    if (B && R) return HTBOTTOMRIGHT;
    if (L) return HTLEFT;
    if (R) return HTRIGHT;
    if (T) return HTTOP;
    if (B) return HTBOTTOM;
    return HTCLIENT;
}

static void begin_drag(NoteState *n, int mode, int hit) {
    n->dragMode = mode;
    n->dragHit = hit;
    n->inMoveSize = 1;
    n->hidden = 0;
    n->dockEdge = EDGE_NONE;
    GetCursorPos(&n->dragStart);
    GetWindowRect(n->hwnd, &n->dragRect);
    SetCapture(n->hwnd);
}

static void apply_drag(NoteState *n) {
    POINT p;
    int dx;
    int dy;
    RECT r;

    if (n->dragMode == DRAG_NONE) return;

    GetCursorPos(&p);
    dx = p.x - n->dragStart.x;
    dy = p.y - n->dragStart.y;
    r = n->dragRect;

    if (n->dragMode == DRAG_MOVE) {
        OffsetRect(&r, dx, dy);
    } else {
        if (n->dragHit == HTLEFT || n->dragHit == HTTOPLEFT || n->dragHit == HTBOTTOMLEFT)
            r.left += dx;
        if (n->dragHit == HTRIGHT || n->dragHit == HTTOPRIGHT || n->dragHit == HTBOTTOMRIGHT)
            r.right += dx;
        if (n->dragHit == HTTOP || n->dragHit == HTTOPLEFT || n->dragHit == HTTOPRIGHT)
            r.top += dy;
        if (n->dragHit == HTBOTTOM || n->dragHit == HTBOTTOMLEFT || n->dragHit == HTBOTTOMRIGHT)
            r.bottom += dy;

        if (r.right - r.left < MIN_W) {
            if (n->dragHit == HTLEFT || n->dragHit == HTTOPLEFT || n->dragHit == HTBOTTOMLEFT)
                r.left = r.right - MIN_W;
            else
                r.right = r.left + MIN_W;
        }

        if (r.bottom - r.top < MIN_H) {
            if (n->dragHit == HTTOP || n->dragHit == HTTOPLEFT || n->dragHit == HTTOPRIGHT)
                r.top = r.bottom - MIN_H;
            else
                r.bottom = r.top + MIN_H;
        }
    }

    SetWindowPos(n->hwnd, z_after(n), r.left, r.top,
                 r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void end_drag(NoteState *n) {
    if (n->dragMode == DRAG_NONE) return;

    ReleaseCapture();
    n->dragMode = DRAG_NONE;
    n->dragHit = HTCLIENT;
    n->inMoveSize = 0;
    GetWindowRect(n->hwnd, &n->visibleRect);
    detect_and_snap(n);
}

static HWND create_note(NoteState *from, int colorIndex) {
    NoteState *n;
    HWND hwnd;
    int x = 1100;
    int y = 180;
    int w = 360;
    int h = 430;

    n = (NoteState *)calloc(1, sizeof(NoteState));
    if (!n) return NULL;

    n->dockEdge = EDGE_NONE;
    n->alwaysOnTop = 1;
    n->autoHide = 1;
    n->fontSize = 13;
    n->dragHit = HTCLIENT;
    apply_preset_values(n, colorIndex);

    if (from && from->hwnd) {
        RECT r;
        MONITORINFO mi;
        POINT center;
        GetWindowRect(from->hwnd, &r);
        w = r.right - r.left;
        h = r.bottom - r.top;
        x = r.left + 28;
        y = r.top + 28;

        center.x = (r.left + r.right) / 2;
        center.y = (r.top + r.bottom) / 2;
        get_monitor_info_for(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &mi);

        if (x + w > mi.rcWork.right) x = mi.rcWork.left + 24;
        if (y + h > mi.rcWork.bottom) y = mi.rcWork.top + 24;
    }

    hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EdgeNoteBorderlessClass",
        L"EdgeNote",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, w, h,
        NULL, NULL, g_instance, n
    );

    if (!hwnd) {
        free(n);
        return NULL;
    }

    ++g_noteCount;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    if (n->edit) {
        SetFocus(n->edit);
        SendMessageW(n->edit, EM_SETSEL, 0, 0);
    }

    return hwnd;
}

static void show_menu(NoteState *n) {
    HMENU menu;
    HMENU colors;
    RECT wr;
    int cmd;

    menu = CreatePopupMenu();
    colors = CreatePopupMenu();
    if (!menu || !colors) {
        if (menu) DestroyMenu(menu);
        if (colors) DestroyMenu(colors);
        return;
    }

    AppendMenuW(colors, MF_STRING | (n->colorIndex == 0 ? MF_CHECKED : 0), IDM_COLOR_YELLOW, L"黄色");
    AppendMenuW(colors, MF_STRING | (n->colorIndex == 1 ? MF_CHECKED : 0), IDM_COLOR_PINK, L"粉色");
    AppendMenuW(colors, MF_STRING | (n->colorIndex == 2 ? MF_CHECKED : 0), IDM_COLOR_BLUE, L"蓝色");
    AppendMenuW(colors, MF_STRING | (n->colorIndex == 3 ? MF_CHECKED : 0), IDM_COLOR_GREEN, L"绿色");
    AppendMenuW(colors, MF_STRING | (n->colorIndex == 4 ? MF_CHECKED : 0), IDM_COLOR_PURPLE, L"紫色");
    AppendMenuW(colors, MF_STRING | (n->colorIndex == 5 ? MF_CHECKED : 0), IDM_COLOR_ORANGE, L"橙色");
    AppendMenuW(colors, MF_SEPARATOR, 0, NULL);
    AppendMenuW(colors, MF_STRING | (n->colorIndex == -1 ? MF_CHECKED : 0), IDM_COLOR_CUSTOM, L"自定义颜色...");

    AppendMenuW(menu, MF_STRING, IDM_NEWNOTE, L"新建便签");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)colors, L"便签颜色");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (n->alwaysOnTop ? MF_CHECKED : 0), IDM_TOPMOST, L"总在最前");
    AppendMenuW(menu, MF_STRING | (n->autoHide ? MF_CHECKED : 0), IDM_AUTOHIDE, L"贴边自动隐藏");
    AppendMenuW(menu, MF_STRING, IDM_UNDOCK, L"取消贴边");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_FONT_PLUS, L"字体放大");
    AppendMenuW(menu, MF_STRING, IDM_FONT_MINUS, L"字体缩小");
    AppendMenuW(menu, MF_STRING, IDM_CLEAR, L"清空");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"关闭此便签");

    GetWindowRect(n->hwnd, &wr);
    n->menuOpen = 1;
    cmd = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        wr.right - TITLE_BTN_W * 2,
        wr.top + TITLE_H,
        0, n->hwnd, NULL
    );
    n->menuOpen = 0;

    DestroyMenu(menu);
    if (cmd) SendMessageW(n->hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    NoteState *n = note_from(hwnd);

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        n = (NoteState *)cs->lpCreateParams;
        if (!n) return FALSE;
        n->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)n);
    }

    switch (msg) {
    case WM_CREATE:
        n->bodyBrush = CreateSolidBrush(n->bodyColor);
        n->titleBrush = CreateSolidBrush(n->titleColor);

        n->edit = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            RESIZE_BORDER, TITLE_H, 300, 300,
            hwnd, (HMENU)ID_EDIT, g_instance, NULL
        );

        if (!n->edit) return -1;

        if (!g_originalEditProc) {
            g_originalEditProc = (WNDPROC)SetWindowLongPtrW(
                n->edit, GWLP_WNDPROC, (LONG_PTR)EditProc
            );
        } else {
            SetWindowLongPtrW(n->edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
        }

        SendMessageW(n->edit, EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(5, 5));
        update_font(n);
        SetTimer(hwnd, TIMER_EDGE, 100, NULL);
        GetWindowRect(hwnd, &n->visibleRect);
        return 0;

    case WM_SETFOCUS:
        if (n && n->edit) SetFocus(n->edit);
        return 0;

    case WM_MOUSEWHEEL:
        if (n && n->edit) {
            SetFocus(n->edit);
            SendMessageW(n->edit, WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (n) {
            PAINTSTRUCT ps;
            HDC dc;
            RECT cr;
            RECT tr;
            RECT title;
            RECT menuR;
            RECT closeR;

            dc = BeginPaint(hwnd, &ps);
            GetClientRect(hwnd, &cr);

            FillRect(dc, &cr, n->bodyBrush);
            tr.left = 0;
            tr.top = 0;
            tr.right = cr.right;
            tr.bottom = TITLE_H;
            FillRect(dc, &tr, n->titleBrush);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, n->titleTextColor);

            title.left = 12;
            title.top = 0;
            title.right = cr.right - TITLE_BTN_W * 2 - 4;
            title.bottom = TITLE_H;
            DrawTextW(dc, L"便签", -1, &title,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            menuR.left = cr.right - TITLE_BTN_W * 2;
            menuR.top = 0;
            menuR.right = cr.right - TITLE_BTN_W;
            menuR.bottom = TITLE_H;
            DrawTextW(dc, L"⋯", -1, &menuR,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            closeR.left = cr.right - TITLE_BTN_W;
            closeR.top = 0;
            closeR.right = cr.right;
            closeR.bottom = TITLE_H;
            DrawTextW(dc, L"×", -1, &closeR,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_SIZE:
        if (n) {
            layout_edit(n);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (n && wParam == TIMER_EDGE) edge_timer(n);
        return 0;

    case WM_LBUTTONDOWN:
        if (n) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT cr;
            int hit;

            SetForegroundWindow(hwnd);
            GetClientRect(hwnd, &cr);

            if (y < TITLE_H && x >= cr.right - TITLE_BTN_W * 2)
                return 0;

            hit = resize_hit_test(x, y, cr.right, cr.bottom);
            if (hit != HTCLIENT) {
                begin_drag(n, DRAG_SIZE, hit);
                return 0;
            }

            if (y < TITLE_H) {
                begin_drag(n, DRAG_MOVE, HTCAPTION);
                return 0;
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (n && n->dragMode != DRAG_NONE) {
            apply_drag(n);
            return 0;
        }
        return 0;

    case WM_LBUTTONUP:
        if (n) {
            int x;
            int y;
            RECT cr;

            if (n->dragMode != DRAG_NONE) {
                end_drag(n);
                return 0;
            }

            x = GET_X_LPARAM(lParam);
            y = GET_Y_LPARAM(lParam);
            GetClientRect(hwnd, &cr);

            if (y >= 0 && y < TITLE_H) {
                if (x >= cr.right - TITLE_BTN_W) {
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (x >= cr.right - TITLE_BTN_W * 2) {
                    show_menu(n);
                    return 0;
                }
            }
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (n && n->dragMode != DRAG_NONE) {
            n->dragMode = DRAG_NONE;
            n->dragHit = HTCLIENT;
            n->inMoveSize = 0;
            GetWindowRect(hwnd, &n->visibleRect);
        }
        return 0;

    case WM_SETCURSOR:
        if (n) {
            POINT p;
            RECT cr;
            int hit;
            LPCWSTR cur = IDC_ARROW;

            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            GetClientRect(hwnd, &cr);
            hit = resize_hit_test(p.x, p.y, cr.right, cr.bottom);

            if (hit == HTLEFT || hit == HTRIGHT) cur = IDC_SIZEWE;
            else if (hit == HTTOP || hit == HTBOTTOM) cur = IDC_SIZENS;
            else if (hit == HTTOPLEFT || hit == HTBOTTOMRIGHT) cur = IDC_SIZENWSE;
            else if (hit == HTTOPRIGHT || hit == HTBOTTOMLEFT) cur = IDC_SIZENESW;

            SetCursor(LoadCursorW(NULL, cur));
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (n) {
            switch (LOWORD(wParam)) {
            case IDM_NEWNOTE:
                create_note(n, g_nextColor);
                g_nextColor = (g_nextColor + 1) % COLOR_PRESET_COUNT;
                return 0;

            case IDM_COLOR_YELLOW:
                set_preset_color(n, 0);
                return 0;
            case IDM_COLOR_PINK:
                set_preset_color(n, 1);
                return 0;
            case IDM_COLOR_BLUE:
                set_preset_color(n, 2);
                return 0;
            case IDM_COLOR_GREEN:
                set_preset_color(n, 3);
                return 0;
            case IDM_COLOR_PURPLE:
                set_preset_color(n, 4);
                return 0;
            case IDM_COLOR_ORANGE:
                set_preset_color(n, 5);
                return 0;
            case IDM_COLOR_CUSTOM:
                choose_custom_color(n);
                return 0;

            case IDM_TOPMOST:
                n->alwaysOnTop = !n->alwaysOnTop;
                SetWindowPos(hwnd, z_after(n), 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;

            case IDM_AUTOHIDE:
                n->autoHide = !n->autoHide;
                if (!n->autoHide && n->hidden) show_from_edge(n);
                return 0;

            case IDM_FONT_PLUS:
                if (n->fontSize < 36) ++n->fontSize;
                update_font(n);
                return 0;

            case IDM_FONT_MINUS:
                if (n->fontSize > 8) --n->fontSize;
                update_font(n);
                return 0;

            case IDM_UNDOCK:
                if (n->hidden) show_from_edge(n);
                n->dockEdge = EDGE_NONE;
                n->hidden = 0;
                return 0;

            case IDM_CLEAR:
                SetWindowTextW(n->edit, L"");
                SetFocus(n->edit);
                return 0;

            case IDM_EXIT:
                DestroyWindow(hwnd);
                return 0;
            }
        }
        break;

    case WM_CTLCOLOREDIT:
        if (n && (HWND)lParam == n->edit) {
            SetBkColor((HDC)wParam, n->bodyColor);
            SetTextColor((HDC)wParam, n->textColor);
            return (LRESULT)n->bodyBrush;
        }
        break;

    case WM_DESTROY:
        if (n) {
            KillTimer(hwnd, TIMER_EDGE);
            if (g_noteCount > 0) --g_noteCount;
            if (g_noteCount == 0) PostQuitMessage(0);
        }
        return 0;

    case WM_NCDESTROY:
        if (n) {
            if (n->editFont) DeleteObject(n->editFont);
            if (n->bodyBrush) DeleteObject(n->bodyBrush);
            if (n->titleBrush) DeleteObject(n->titleBrush);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            free(n);
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    WNDCLASSEXW wc;
    HWND first;

    (void)hPrev;
    (void)cmd;
    (void)show;

    g_instance = hInst;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"EdgeNoteBorderlessClass";

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;

    first = create_note(NULL, 0);
    if (!first) return 1;

    while (1) {
        MSG m;
        int r = GetMessageW(&m, NULL, 0, 0);
        if (r <= 0) break;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    return 0;
}
