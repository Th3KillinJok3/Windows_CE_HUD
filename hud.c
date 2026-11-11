/*
 * hud.c — prędkościomierz + licznik km + wysokość
 * Windows CE 5.0 / ARM (Lark FreeBird 35.7, S3C2412, 320x240)
 *
 * Kompilacja (CeGCC 9.3.0):
 *   arm-mingw32ce-gcc -O2 -Wall -DUNICODE -D_UNICODE -o hud.exe hud.c \
 *       -lcoredll -Wl,-subsystem,windows
 *
 * Wersja z LOGOWANIEM do \Storage Card\hud_log.txt
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <wchar.h>

/* ─────────── CONFIG ─────────── */
#define ODO_FILE_A    "\\Storage Card\\hud_odo.txt"
#define ODO_FILE_B    "\\SDMMC\\hud_odo.txt"
#define LOG_FILE      "\\Storage Card\\hud_log.txt"
#define LOG_FILE_B    "\\SDMMC\\hud_log.txt"

/* Po znalezieniu portu wpisz go tu — start będzie natychmiastowy.
 * Zostaw 0, aby skanować za każdym razem. */
#define FIXED_PORT    L""
#define FIXED_BAUD    0

/* ─────────── Kolory ─────────── */
#define C_BG        RGB(10, 15, 13)
#define C_PANEL     RGB(19, 26, 23)
#define C_LINE      RGB(36, 49, 43)
#define C_TEXT      RGB(232, 240, 236)
#define C_DIM       RGB(125, 145, 138)
#define C_ACCENT    RGB(74, 222, 128)
#define C_ACCENT_D  RGB(28, 58, 42)
#define C_WARN      RGB(251, 191, 36)
#define C_WARNBG    RGB(42, 34, 12)
#define C_ERR       RGB(248, 113, 113)
#define C_ERRBG     RGB(42, 26, 26)
#define C_BTN       RGB(60, 32, 32)
#define C_BTNLINE   RGB(120, 60, 60)

#define PI_D 3.14159265358979323846

/* ─────────── Log ─────────── */
static FILE *g_log = NULL;

static void log_open(void)
{
    g_log = fopen(LOG_FILE, "w");
    if (!g_log) g_log = fopen(LOG_FILE_B, "w");
    if (!g_log) return;
    fprintf(g_log, "=== Lark HUD log ===\n");
    fflush(g_log);
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

/* ─────────── Stan GPS ─────────── */
static volatile int    g_fix;
static volatile int    g_sats;
static volatile double g_speed_kmh;
static volatile double g_alt_m;
static volatile double g_lat, g_lon;

static HANDLE          g_hGps = INVALID_HANDLE_VALUE;
static volatile int    g_running = 1;

/* ─────────── Auto-skan ─────────── */
static const WCHAR *g_ports[] = { L"COM1:", L"COM2:", L"COM3:",
                                  L"COM6:", L"COM7:", L"COM9:" };
static const DWORD  g_bauds[] = { CBR_4800, CBR_9600 };
#define N_PORTS (int)(sizeof(g_ports) / sizeof(g_ports[0]))
#define N_BAUDS (int)(sizeof(g_bauds) / sizeof(g_bauds[0]))

static volatile int   g_scanning = 1;
static WCHAR          g_used_port[16] = L"?";
static volatile DWORD g_used_baud;
static volatile int   g_probe_result;   /* 0=szuka  1=OK  -1=brak */

/* ─────────── Licznik ─────────── */
static double g_odo_km = 0.0;
static double g_last_lat, g_last_lon;
static int    g_have_last;
static DWORD  g_last_tick;

/* ─────────── Fonty ─────────── */
static HFONT g_fSpeed, g_fUnit, g_fLabel, g_fVal, g_fBtn;

/* ─────────── Pomocnicze ─────────── */
static double deg2rad(double d) { return d * PI_D / 180.0; }

static void w2a(const WCHAR *src, char *dst, int maxlen)
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(deg2rad(lat1)) * cos(deg2rad(lat2)) *
               sin(dlon / 2) * sin(dlon / 2);
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

/* ─────────── Licznik: zapis / odczyt ─────────── */
static void odo_save(void)
{
    FILE *f = fopen(ODO_FILE_A, "w");
    if (!f) f = fopen(ODO_FILE_B, "w");
    if (f) { fprintf(f, "%.3f\n", g_odo_km); fclose(f); }
}

static void odo_load(void)
{
    FILE *f = fopen(ODO_FILE_A, "r");
    if (!f) f = fopen(ODO_FILE_B, "r");
    if (f) {
        double v = 0.0;
        if (fscanf(f, "%lf", &v) == 1 && v >= 0.0 && v < 1e7) g_odo_km = v;
        fclose(f);
    }
}

/* ─────────── NMEA ─────────── */
static int split_nmea(char *line, char **fields, int maxf)
{
    int n = 0;
    char *p = line;
    if (*p == '$' || *p == '!') p++;
    fields[n++] = p;
    while (*p && n < maxf) {
        if (*p == ',') { *p = 0; fields[n++] = p + 1; }
        p++;
    }
    if (n > 0) {
        char *star = strchr(fields[n - 1], '*');
        if (star) *star = 0;
    }
    return n;
}

static void parse_rmc(char **f, int n)
{
    double knots;
    if (n < 8) return;
    if (f[2][0] != 'A') { g_fix = 0; return; }

    knots = atof(f[7]);
    g_speed_kmh = knots * 1.852;

    if (f[3][0] && f[5][0]) {
        double rawlat = atof(f[3]);
        double rawlon = atof(f[5]);
        int    dlat = (int)(rawlat / 100.0);
        int    dlon = (int)(rawlon / 100.0);
        double lat = dlat + (rawlat - dlat * 100.0) / 60.0;
        double lon = dlon + (rawlon - dlon * 100.0) / 60.0;
        if (f[4][0] == 'S') lat = -lat;
        if (f[6][0] == 'W') lon = -lon;
        g_lat = lat;
        g_lon = lon;
        g_fix = 1;
    }
}

static void parse_gga(char **f, int n)
{
    if (n < 10) return;
    g_sats = atoi(f[7]);
    if (atoi(f[6]) > 0) {
        g_alt_m = atof(f[9]);
        g_fix = 1;
    } else {
        g_fix = 0;
    }
}

static void nmea_line(char *line)
{
    char *f[24];
    int n = split_nmea(line, f, 24);
    const char *s, *type;
    if (n < 1) return;
    s = f[0];
    if (strlen(s) < 5) return;
    type = s + strlen(s) - 3;
    if (!strcmp(type, "RMC"))      parse_rmc(f, n);
    else if (!strcmp(type, "GGA")) parse_gga(f, n);
}

/* ─────────── Otwarcie portu ─────────── */
static HANDLE open_port(const WCHAR *port, DWORD baud)
{
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    char name[32];

    w2a(port, name, sizeof(name));

    h = CreateFileW(port, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("OPEN %s @%d -> FAIL (err %d)\n",
              name, (int)baud, (int)GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    logf_("OPEN %s @%d -> OK\n", name, (int)baud);

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (GetCommState(h, &dcb)) {
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        if (!SetCommState(h, &dcb))
            logf_("  SetCommState FAIL err %d\n", (int)GetLastError());
    } else {
        logf_("  GetCommState FAIL err %d\n", (int)GetLastError());
    }

    to.ReadIntervalTimeout         = 50;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 200;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return h;
}

/* ─────────── Wątek GPS ─────────── */
static DWORD WINAPI gps_thread(LPVOID param)
{
    static char rx[4096];
    static char buf[512];
    DWORD i, got;
    int  rxn;
    (void)param;

    logf_("gps_thread start\n");

    /* ── FAZA 1: szukanie portu i prędkości ── */
    {
        int ok = 0;
        int pi, bi;

        if (FIXED_PORT[0]) {
            g_hGps = open_port(FIXED_PORT, FIXED_BAUD ? FIXED_BAUD : CBR_4800);
            if (g_hGps != INVALID_HANDLE_VALUE) {
                wcscpy(g_used_port, FIXED_PORT);
                g_used_baud    = FIXED_BAUD ? FIXED_BAUD : CBR_4800;
                g_scanning     = 0;
                g_probe_result = 1;
                ok = 1;
                logf_("using FIXED_PORT\n");
            }
        }

        for (pi = 0; pi < N_PORTS && !ok && g_running; pi++) {
            for (bi = 0; bi < N_BAUDS && !ok && g_running; bi++) {
                HANDLE h;
                int saw = 0;
                DWORD deadline, total = 0;
                int shown = 0;

                h = open_port(g_ports[pi], g_bauds[bi]);
                if (h == INVALID_HANDLE_VALUE) continue;

                rxn = 0;
                deadline = GetTickCount() + 2000;
                while (GetTickCount() < deadline && g_running && !saw) {
                    got = 0;
                    if (!ReadFile(h, buf, sizeof(buf), &got, NULL) || got == 0)
                        continue;
                    total += got;

                    if (!shown && g_log) {
                        int k, lim = got < 40 ? (int)got : 40;
                        logf_("  RX[%d]: ", (int)got);
                        for (k = 0; k < lim; k++) {
                            unsigned char c = (unsigned char)buf[k];
                            if (c >= 32 && c < 127) fputc(c, g_log);
                            else fprintf(g_log, "\\x%02X", c);
                        }
                        fprintf(g_log, "\n");
                        fflush(g_log);
                        shown = 1;
                    }

                    for (i = 0; i < got; i++) {
                        char c = buf[i];
                        if (c == '\n' || c == '\r') {
                            if (rxn > 6 && rx[0] == '$') { saw = 1; break; }
                            rxn = 0;
                        } else if (c == '$') {
                            rxn = 0;
                            rx[rxn++] = c;
                        } else if (rxn < (int)sizeof(rx) - 2) {
                            rx[rxn++] = c;
                        } else {
                            rxn = 0;
                        }
                    }
                }

                logf_("  total %d bytes, nmea=%s\n",
                      (int)total, saw ? "YES" : "no");

                if (saw) {
                    g_hGps         = h;
                    wcscpy(g_used_port, g_ports[pi]);
                    g_used_baud    = g_bauds[bi];
                    g_scanning     = 0;
                    g_probe_result = 1;
                    ok = 1;
                    logf_("FOUND %s @%d\n", (int)0, (int)g_bauds[bi]);
                } else {
                    CloseHandle(h);
                }
            }
        }

        if (!ok) {
            g_scanning     = 0;
            g_probe_result = -1;
            logf_("NO GPS PORT FOUND\n");
            return 0;
        }
    }

    /* ── FAZA 2: normalny odczyt ── */
    logf_("entering read loop\n");
    rxn = 0;
    {
        DWORD lines = 0;
        while (g_running) {
            got = 0;
            if (!ReadFile(g_hGps, buf, sizeof(buf), &got, NULL) || got == 0) {
                Sleep(50);
                continue;
            }
            for (i = 0; i < got; i++) {
                char c = buf[i];
                if (c == '\n' || c == '\r') {
                    if (rxn > 6) {
                        rx[rxn] = 0;
                        nmea_line(rx);
                        if ((lines++ % 40) == 0)
                            logf_("line %d: %.60s fix=%d sats=%d\n",
                                  (int)lines, rx, g_fix, g_sats);
                    }
                    rxn = 0;
                } else if (c == '$') {
                    rxn = 0;
                    rx[rxn++] = c;
                } else if (rxn < (int)sizeof(rx) - 2) {
                    rx[rxn++] = c;
                } else {
                    rxn = 0;
                }
            }
        }
    }
    logf_("gps_thread exit\n");
    return 0;
}

/* ─────────── Rysowanie ─────────── */
static HFONT mkfont(int size, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight         = size;
    lf.lfWeight         = weight;
    lf.lfCharSet        = DEFAULT_CHARSET;
    lf.lfQuality        = ANTIALIASED_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
    wcscpy(lf.lfFaceName, L"Tahoma");
    return CreateFontIndirectW(&lf);
}

static void make_fonts(void)
{
    g_fSpeed = mkfont(-110, FW_BOLD);
    g_fUnit  = mkfont(-22,  FW_BOLD);
    g_fLabel = mkfont(-13,  FW_BOLD);
    g_fVal   = mkfont(-26,  FW_BOLD);
    g_fBtn   = mkfont(-16,  FW_BOLD);
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(c);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text_at(HDC dc, int x, int y, const WCHAR *s, COLORREF c, HFONT f)
{
    RECT r;
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    r.left = x; r.top = y;
    r.right = x + 300; r.bottom = y + 200;
    DrawTextW(dc, s, -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(dc, old);
}

static int text_w(HDC dc, const WCHAR *s, HFONT f)
{
    RECT r;
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    r.left = 0; r.top = 0; r.right = 0; r.bottom = 0;
    DrawTextW(dc, s, -1, &r,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_CALCRECT | DT_NOCLIP);
    SelectObject(dc, old);
    return r.right - r.left;
}

static void text_center(HDC dc, int cx, int y, const WCHAR *s, COLORREF c, HFONT f)
{
    text_at(dc, cx - text_w(dc, s, f) / 2, y, s, c, f);
}

static void round_rect(HDC dc, int x, int y, int w, int h, int rad,
                       COLORREF fillc, COLORREF linec)
{
    HBRUSH b  = CreateSolidBrush(fillc);
    HPEN   p  = CreatePen(PS_SOLID, 1, linec);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN   op = (HPEN)SelectObject(dc, p);
    RoundRect(dc, x, y, x + w, y + h, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void odo_tick(void)
{
    DWORD now = GetTickCount();
    if (!g_fix) { g_have_last = 0; g_last_tick = now; return; }

    if (g_have_last && g_last_tick) {
        double dt = (double)(now - g_last_tick) / 1000.0;
        if (dt > 0.2 && dt < 30.0) {
            double d = haversine_m(g_last_lat, g_last_lon, g_lat, g_lon);
            if (d > 0.5 && d < 500.0) g_odo_km += d / 1000.0;
        }
        g_last_lat = g_lat;
        g_last_lon = g_lon;
    } else {
        g_last_lat = g_lat;
        g_last_lon = g_lon;
        g_have_last = 1;
    }
    g_last_tick = now;
}

static void draw_screen(HDC dc, int W, int H)
{
    WCHAR buf[64];
    int gpsOk = g_fix;
    int cx, spY, bw, bh, by, rw, rh, rx, ry;
    COLORREF cSp;

    fill(dc, 0, 0, W, H, C_BG);

    if (g_scanning) {
        fill(dc, 0, 0, W, 20, C_WARNBG);
        text_at(dc, 6, 3, L"szukam portu GPS...", C_WARN, g_fLabel);
    } else if (g_probe_result == -1) {
        fill(dc, 0, 0, W, 20, C_ERRBG);
        text_at(dc, 6, 3, L"BRAK GPS - zly port lub baud", C_ERR, g_fLabel);
    } else if (gpsOk) {
        fill(dc, 0, 0, W, 20, C_ACCENT_D);
        wsprintfW(buf, L"%s %d baud  \xB7  %d sat",
                  g_used_port, (int)g_used_baud, g_sats);
        text_at(dc, 6, 3, buf, C_ACCENT, g_fLabel);
    } else {
        fill(dc, 0, 0, W, 20, C_WARNBG);
        wsprintfW(buf, L"%s %d baud - czekam na fix",
                  g_used_port, (int)g_used_baud);
        text_at(dc, 6, 3, buf, C_WARN, g_fLabel);
    }

    if (gpsOk) {
        int v = (int)(g_speed_kmh + 0.5);
        if (v < 0) v = 0;
        if (v > 999) v = 999;
        wsprintfW(buf, L"%d", v);
    } else {
        wcscpy(buf, L"--");
    }
    cx  = W / 2;
    spY = 26;
    cSp = gpsOk ? C_TEXT : C_DIM;
    text_center(dc, cx, spY, buf, cSp, g_fSpeed);
    text_center(dc, cx, spY + 116, L"km/h", C_DIM, g_fUnit);

    bw = (W - 18) / 2;
    bh = 60;
    by = H - bh - 34;

    round_rect(dc, 6, by, bw, bh, 8, C_PANEL, C_LINE);
    text_at(dc, 16, by + 6, L"LICZNIK", C_DIM, g_fLabel);
    wsprintfW(buf, L"%.1f km", g_odo_km);
    text_at(dc, 16, by + 22, buf, C_ACCENT, g_fVal);

    round_rect(dc, 12 + bw, by, bw, bh, 8, C_PANEL, C_LINE);
    text_at(dc, 22 + bw, by + 6, L"WYSOKOSC", C_DIM, g_fLabel);
    if (gpsOk) wsprintfW(buf, L"%.0f m", g_alt_m);
    else       wcscpy(buf, L"-- m");
    text_at(dc, 22 + bw, by + 22, buf, gpsOk ? C_TEXT : C_DIM, g_fVal);

    rw = 100; rh = 24;
    rx = W - rw - 6;
    ry = H - rh - 6;
    round_rect(dc, rx, ry, rw, rh, 6, C_BTN, C_BTNLINE);
    text_center(dc, rx + rw / 2, ry + 4, L"RESET", C_ERR, g_fBtn);
}

/* ─────────── Okno ─────────── */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        make_fonts();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc;
        RECT r;
        dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &r);
        draw_screen(dc, r.right, r.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp), y = HIWORD(lp);
        RECT r;
        int rw = 100, rh = 24, rx, ry;
        GetClientRect(hwnd, &r);
        rx = r.right - rw - 6;
        ry = r.bottom - rh - 6;
        if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh) {
            g_odo_km = 0.0;
            g_have_last = 0;
            odo_save();
            logf_("odometer reset by user\n");
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_TIMER:
        odo_tick();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CLOSE:
        g_running = 0;
        odo_save();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASSW wc;
    MSG msg;
    HWND hwnd;
    int W, H;

    (void)hPrev; (void)cmd; (void)show;

    W = GetSystemMetrics(SM_CXSCREEN);
    H = GetSystemMetrics(SM_CYSCREEN);

    log_open();
    logf_("=== start, screen %dx%d ===\n", W, H);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.lpszClassName = L"LarkHUD";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassW(&wc)) {
        logf_("RegisterClass FAILED\n");
        return 1;
    }

    hwnd = CreateWindowExW(0, L"LarkHUD", L"Lark HUD",
                           WS_POPUP | WS_VISIBLE, 0, 0, W, H,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) {
        logf_("CreateWindow FAILED\n");
        return 1;
    }

    odo_load();
    logf_("odo loaded: %.3f km\n", g_odo_km);

    CreateThread(NULL, 0, gps_thread, NULL, 0, NULL);
    SetTimer(hwnd, 1, 500, NULL);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_running = 0;
    if (g_hGps != INVALID_HANDLE_VALUE) CloseHandle(g_hGps);
    odo_save();
    logf_("=== exit ===\n");
    if (g_log) fclose(g_log);
    return 0;
}
