/*
 * hud.c — prędkościomierz + licznik km + wysokość dla Windows CE 5.0 (ARM)
 * Cel: Lark FreeBird 35.7 (Samsung S3C2412, 320x240, SiRF Star III)
 *
 * Budowanie: patrz README.md (GH Actions + CeGCC 9.3.0)
 * Licencja: robisz z tym co chcesz.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ─────────── CONFIG — tu zmieniasz port i prędkość GPS ─────────── */
#define GPS_PORT     "COM2:"
#define GPS_BAUD     CBR_4800
#define ODO_FILE     "\\Storage Card\\hud_odo.txt"
#define ODO_FILE2    "\\SDMMC\\hud_odo.txt"
#define REG_KEY      TEXT("Software\\LarkHUD")

/* ─────────── Kolory / wygląd ─────────── */
#define C_BG        RGB(10, 15, 13)
#define C_PANEL     RGB(19, 26, 23)
#define C_TEXT      RGB(232, 240, 236)
#define C_DIM       RGB(125, 145, 138)
#define C_ACCENT    RGB(74, 222, 128)
#define C_ACCENT_D  RGB(28, 58, 42)
#define C_WARN      RGB(251, 191, 36)
#define C_ERR       RGB(248, 113, 113)

/* ─────────── Stan GPS ─────────── */
typedef struct {
    volatile int   fix;        /* 0 = brak, 1 = jest */
    volatile int   sats;
    volatile double speed_kmh;
    volatile double alt_m;
    volatile double lat, lon;
    volatile double track;
    volatile int   valid;
} GpsState;

static GpsState      g_gps;
static HANDLE        g_hGps   = INVALID_HANDLE_VALUE;
static volatile int  g_running = 1;

/* ─────────── Licznik ─────────── */
static double  g_odo_km      = 0.0;   /* zapisany dystans */
static double  g_last_lat    = 0.0;
static double  g_last_lon    = 0.0;
static int     g_have_last   = 0;
static ULONGLONG g_last_tick = 0;

/* ─────────── Pomocnicze: dystans ─────────── */
static double deg2rad(double d) { return d * 3.14159265358979323846 / 180.0; }

static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double a = sin(dlat/2)*sin(dlat/2) +
               cos(deg2rad(lat1))*cos(deg2rad(lat2))*sin(dlon/2)*sin(dlon/2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

/* ─────────── Zapis / odczyt licznika ─────────── */
static void odo_save(void)
{
    FILE *f = fopen(ODO_FILE, "w");
    if (!f) f = fopen(ODO_FILE2, "w");
    if (f) { fprintf(f, "%.3f\n", g_odo_km); fclose(f); }
}

static void odo_load(void)
{
    FILE *f = fopen(ODO_FILE, "r");
    if (!f) f = fopen(ODO_FILE2, "r");
    if (f) {
        double v = 0.0;
        if (fscanf(f, "%lf", &v) == 1 && v >= 0.0 && v < 1e7) g_odo_km = v;
        fclose(f);
    }
}

/* ─────────── Parsowanie NMEA ─────────── */
/* Pola rozdzielone przecinkami; pole 0 to np. $GPRMC. */
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
    /* ucięcie sumy kontrolnej w ostatnim polu */
    if (n > 0) {
        char *star = strchr(fields[n-1], '*');
        if (star) *star = 0;
    }
    return n;
}

/* "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A" */
static void parse_rmc(char **f, int n)
{
    if (n < 8) return;
    if (f[2][0] != 'A') { g_gps.fix = 0; return; }

    /* prędkość w węzłach -> km/h */
    double knots = atof(f[7]);
    g_gps.speed_kmh = knots * 1.852;

    /* kurs */
    if (f[8] && f[8][0]) g_gps.track = atof(f[8]);

    /* pozycja: ddmm.mmmm / dddmm.mmmm */
    if (f[3][0] && f[5][0]) {
        double rawlat = atof(f[3]);
        double rawlon = atof(f[5]);
        int    dlat = (int)(rawlat / 100.0);
        int    dlon = (int)(rawlon / 100.0);
        double lat = dlat + (rawlat - dlat * 100.0) / 60.0;
        double lon = dlon + (rawlon - dlon * 100.0) / 60.0;
        if (f[4][0] == 'S') lat = -lat;
        if (f[6][0] == 'W') lon = -lon;
        g_gps.lat = lat;
        g_gps.lon = lon;
        g_gps.fix = 1;
        g_gps.valid = 1;
    }
}

/* "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47" */
static void parse_gga(char **f, int n)
{
    if (n < 10) return;
    int quality = atoi(f[6]);
    g_gps.sats = atoi(f[7]);
    if (quality > 0) {
        g_gps.alt_m = atof(f[9]);
        g_gps.fix = 1;
    } else {
        g_gps.fix = 0;
    }
}

static void nmea_line(char *line)
{
    char *f[24];
    int n = split_nmea(line, f, 24);
    if (n < 1) return;

    /* akceptujemy GPRMC/GNRMC/GPGGA/GNGGA i warianty */
    const char *s = f[0];
    if (strlen(s) < 5) return;
    const char *type = s + strlen(s) - 3;
    if (!strcmp(type, "RMC")) parse_rmc(f, n);
    else if (!strcmp(type, "GGA")) parse_gga(f, n);
}

/* ─────────── Wątek czytający port ─────────── */
static DWORD WINAPI gps_thread(LPVOID param)
{
    (void)param;
    static char rx[4096];
    int  rxn = 0;

    while (g_running) {
        if (g_hGps == INVALID_HANDLE_VALUE) {
            g_hGps = CreateFileW(L"COM2:", GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, 0, NULL);
            if (g_hGps == INVALID_HANDLE_VALUE) { Sleep(1500); continue; }

            DCB dcb;
            memset(&dcb, 0, sizeof(dcb));
            dcb.DCBlength = sizeof(dcb);
            if (GetCommState(g_hGps, &dcb)) {
                dcb.BaudRate = GPS_BAUD;
                dcb.ByteSize = 8;
                dcb.Parity   = NOPARITY;
                dcb.StopBits = ONESTOPBIT;
                SetCommState(g_hGps, &dcb);
            }
            COMMTIMEOUTS to;
            to.ReadIntervalTimeout        = 20;
            to.ReadTotalTimeoutMultiplier = 0;
            to.ReadTotalTimeoutConstant   = 200;
            to.WriteTotalTimeoutMultiplier= 0;
            to.WriteTotalTimeoutConstant  = 0;
            SetCommTimeouts(g_hGps, &to);
            rxn = 0;
        }

        char buf[512];
        DWORD got = 0;
        if (!ReadFile(g_hGps, buf, sizeof(buf), &got, NULL) || got == 0) {
            /* brak danych — sprawdź czy port nie zniknął */
            Sleep(50);
            continue;
        }

        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (rxn > 6) { rx[rxn] = 0; nmea_line(rx); }
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
    return 0;
}

/* ─────────── Rysowanie ─────────── */
static HFONT g_fSpeed, g_fUnit, g_fLabel, g_fVal, g_fBtn;

static void make_fonts(void)
{
    g_fSpeed = CreateFont(-110, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, TEXT("Tahoma"));
    g_fUnit  = CreateFont(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, TEXT("Tahoma"));
    g_fLabel = CreateFont(-13, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, TEXT("Tahoma"));
    g_fVal   = CreateFont(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, TEXT("Tahoma"));
    g_fBtn   = CreateFont(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, TEXT("Tahoma"));
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    RECT r; r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text_at(HDC dc, int x, int y, const TCHAR *s, COLORREF c, HFONT f)
{
    HFONT old = (HFONT)SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOut(dc, x, y, s, lstrlen(s));
    SelectObject(dc, old);
}

static int text_w(HDC dc, const TCHAR *s, HFONT f)
{
    SIZE sz;
    HFONT old = (HFONT)SelectObject(dc, f);
    GetTextExtentPoint32(dc, s, lstrlen(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static void text_center(HDC dc, int cx, int y, const TCHAR *s, COLORREF c, HFONT f)
{
    text_at(dc, cx - text_w(dc, s, f) / 2, y, s, c, f);
}

/* Prostokąt z zaokrąglonymi narożnikami */
static void round_rect(HDC dc, int x, int y, int w, int h, int r, COLORREF fillc, COLORREF linec)
{
    HBRUSH b  = CreateSolidBrush(fillc);
    HPEN   p  = CreatePen(PS_SOLID, 1, linec);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN   op = (HPEN)SelectObject(dc, p);
    RoundRect(dc, x, y, x + w, y + h, r, r);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

/* Aktualizacja licznika — wywoływana z wątku UI co ~500 ms */
static void odo_tick(void)
{
    ULONGLONG now = GetTickCount();
    if (!g_gps.fix) { g_have_last = 0; g_last_tick = now; return; }

    if (g_have_last && g_last_tick) {
        double dt = (double)(now - g_last_tick) / 1000.0;
        if (dt > 0.2 && dt < 30.0) {
            double d = haversine_m(g_last_lat, g_last_lon, g_gps.lat, g_gps.lon);
            /* odrzucamy skoki > 500 m w jednym kroku (błąd fixu) */
            if (d < 500.0 && d > 0.5) g_odo_km += d / 1000.0;
        }
        g_last_lat = g_gps.lat;
        g_last_lon = g_gps.lon;
    } else {
        g_last_lat = g_gps.lat;
        g_last_lon = g_gps.lon;
        g_have_last = 1;
    }
    g_last_tick = now;
}

/* ─────────── Okno ─────────── */
static HWND g_hwnd;

static void draw_screen(HDC dc, int W, int H)
{
    TCHAR buf[64];

    fill(dc, 0, 0, W, H, C_BG);

    /* górny pasek: status GPS */
    int gpsOk = g_gps.fix;
    fill(dc, 0, 0, W, 20, gpsOk ? C_ACCENT_D : RGB(42, 26, 26));
    if (gpsOk) {
        wsprintf(buf, TEXT("GPS OK  ·  %d sat"), (int)g_gps.sats);
        text_at(dc, 6, 3, buf, C_ACCENT, g_fLabel);
    } else {
        text_at(dc, 6, 3, TEXT("szukam satelitow..."), C_WARN, g_fLabel);
    }

    /* środek: prędkość */
    if (gpsOk) {
        int v = (int)(g_gps.speed_kmh + 0.5);
        if (v < 0) v = 0;
        if (v > 999) v = 999;
        wsprintf(buf, TEXT("%d"), v);
    } else {
        lstrcpy(buf, TEXT("--"));
    }
    int cx = W / 2;
    int spY = 26;
    COLORREF cSp = gpsOk ? C_TEXT : C_DIM;
    text_center(dc, cx, spY, buf, cSp, g_fSpeed);
    text_center(dc, cx, spY + 116, TEXT("km/h"), C_DIM, g_fUnit);

    /* dolna sekcja: licznik + wysokość */
    int bw = (W - 18) / 2;
    int bh = 60;
    int by = H - bh - 34;

    round_rect(dc, 6, by, bw, bh, 8, C_PANEL, RGB(36, 49, 43));
    text_at(dc, 16, by + 6, TEXT("LICZNIK"), C_DIM, g_fLabel);
    wsprintf(buf, TEXT("%.1f km"), g_odo_km);
    text_at(dc, 16, by + 22, buf, C_ACCENT, g_fVal);

    round_rect(dc, 12 + bw, by, bw, bh, 8, C_PANEL, RGB(36, 49, 43));
    text_at(dc, 22 + bw, by + 6, TEXT("WYSOKOSC"), C_DIM, g_fLabel);
    if (gpsOk) wsprintf(buf, TEXT("%.0f m"), g_gps.alt_m);
    else       lstrcpy(buf, TEXT("-- m"));
    text_at(dc, 22 + bw, by + 22, buf, gpsOk ? C_TEXT : C_DIM, g_fVal);

    /* przycisk RESET */
    int rw = 100, rh = 24;
    int rx = W - rw - 6, ry = H - rh - 6;
    round_rect(dc, rx, ry, rw, rh, 6, RGB(60, 32, 32), RGB(120, 60, 60));
    text_center(dc, rx + rw / 2, ry + 4, TEXT("RESET"), C_ERR, g_fBtn);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        make_fonts();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        draw_screen(dc, r.right, r.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp), y = HIWORD(lp);
        RECT r; GetClientRect(hwnd, &r);
        int rw = 100, rh = 24;
        int rx = r.right - rw - 6, ry = r.bottom - rh - 6;
        if (x >= rx && x <= rx + rw && y >= ry && y <= ry + rh) {
            g_odo_km = 0.0;
            g_have_last = 0;
            odo_save();
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
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    (void)hPrev; (void)cmd;

    /* pełny ekran, bez paska zadań */
    int W = GetSystemMetrics(SM_CXSCREEN);
    int H = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASS wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.lpszClassName = TEXT("LarkHUD");
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClass(&wc)) return 1;

    g_hwnd = CreateWindow(TEXT("LarkHUD"), TEXT("Lark HUD"),
                          WS_POPUP | WS_VISIBLE,
                          0, 0, W, H, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    odo_load();

    g_hGps = INVALID_HANDLE_VALUE;
    CreateThread(NULL, 0, gps_thread, NULL, 0, NULL);

    SetTimer(g_hwnd, 1, 500, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = 0;
    if (g_hGps != INVALID_HANDLE_VALUE) CloseHandle(g_hGps);
    odo_save();
    return 0;
}
