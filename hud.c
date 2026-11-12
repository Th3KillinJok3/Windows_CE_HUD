/*
 * hud.c - speedometer + odometer + altitude HUD
 * Windows CE 5.0 / ARM (Lark FreeBird 35.7 - 480x272, 42.1 - 480x272)
 *
 * Features: GPD0 power-on, SiRF+MTK wakeup, binary SiRF, COM0-9 scan,
 * heartbeat log every 60 s, X close button, 4 themes, pixel fonts,
 * metric/imperial units and 10 UI languages (see hud.ini).
 *
 * Build (CeGCC 9.3.0):
 *   arm-mingw32ce-gcc -O2 -Wall -DUNICODE -D_UNICODE -o hud.exe hud.c \
 *       -lcoredll -Wl,-subsystem,windows
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

/* ============ CONFIG ============ */
#define ODO_FILE_A    "\\Storage Card\\hud_odo.txt"
#define ODO_FILE_B    "\\SDMMC\\hud_odo.txt"
#define LOG_FILE      "\\Storage Card\\hud_log.txt"
#define LOG_FILE_B    "\\SDMMC\\hud_log.txt"
#define THEME_FILE_A  "\\Storage Card\\hud_theme.txt"
#define THEME_FILE_B  "\\SDMMC\\hud_theme.txt"
#define FONT_FILE_A   "\\Storage Card\\hud_font.txt"
#define FONT_FILE_B   "\\SDMMC\\hud_font.txt"
#define INI_FILE_A    "\\Storage Card\\hud.ini"
#define INI_FILE_B    "\\SDMMC\\hud.ini"

#define FIXED_PORT    L"COM2:"
#define FIXED_PORT2   L"COM7:"
#define FIXED_BAUD    CBR_4800

#define PI_D 3.14159265358979323846
#define IOCTL_SERVICE_REFRESH 0x4100000C
#define IOCTL_SERVICE_START   0x41000004

/* ============ MOTYWY ============ */
typedef struct {
    COLORREF bg, panel, line, text, dim;
    COLORREF accent, accentD, warn, warnBg, err, errBg, btn, btnLine;
} Theme;

/* Theme 0 - VERSO: red overlay taken from the Toyota Verso 2017
 * instrument cluster (orange glow) recoloured to red. Dark, high
 * contrast, soft red halo around the digits. */
static const Theme THEMES[4] = {
    { RGB(8,4,6), RGB(30,10,14), RGB(96,18,26), RGB(255,86,72),
      RGB(150,60,58), RGB(255,34,34), RGB(120,12,16),
      RGB(255,150,40), RGB(70,36,6), RGB(255,80,60), RGB(80,10,10),
      RGB(46,12,16), RGB(140,28,34) },
    /* 1 CZERWONY - dark background, red accent, bright digits */
    { RGB(12,10,11), RGB(24,18,20), RGB(52,32,36), RGB(245,235,236),
      RGB(155,125,130), RGB(255,64,64), RGB(70,16,18),
      RGB(255,196,64), RGB(46,34,8), RGB(255,120,120), RGB(56,14,14),
      RGB(58,26,28), RGB(112,44,48) },
    { RGB(12,10,6), RGB(24,19,10), RGB(52,40,16), RGB(255,243,220),
      RGB(160,140,110), RGB(251,191,36), RGB(66,48,10),
      RGB(125,211,252), RGB(20,32,44), RGB(248,113,113), RGB(42,26,26),
      RGB(60,32,32), RGB(120,60,60) },
    { RGB(6,10,16), RGB(12,20,28), RGB(24,42,58), RGB(220,238,255),
      RGB(120,150,175), RGB(96,200,255), RGB(16,48,72),
      RGB(251,191,36), RGB(42,34,12), RGB(248,113,113), RGB(42,26,26),
      RGB(60,32,32), RGB(120,60,60) },
    /* 4 DOTMATRIX - 1980s VFD green/amber */
    { RGB(2,8,4), RGB(6,20,10), RGB(10,40,18), RGB(120,255,140),
      RGB(60,160,80), RGB(80,255,100), RGB(6,30,12),
      RGB(255,190,60), RGB(40,28,6), RGB(255,120,80), RGB(30,14,4),
      RGB(10,30,14), RGB(30,80,40) },
    /* 5 EL_GLOW - electroluminescent aqua-cyan */
    { RGB(2,8,12), RGB(6,20,26), RGB(8,40,52), RGB(200,255,250),
      RGB(80,180,190), RGB(0,245,230), RGB(0,60,56),
      RGB(255,200,80), RGB(40,30,8), RGB(255,120,120), RGB(30,8,8),
      RGB(6,26,28), RGB(0,120,110) },
};
#define THEME_COUNT 6
static const WCHAR *THEME_NAMES[THEME_COUNT] = {
    L"VERSO", L"CZERWONY", L"AMBER", L"NIEBIESKI",
    L"DOTMATRIX", L"EL_GLOW"
};
static int g_theme = 0;          /* 0 = VERSO (default) */
static int g_menu_open = 0;      /* on-screen settings menu */
#define TBG     (THEMES[g_theme].bg)
#define TPANEL  (THEMES[g_theme].panel)
#define TLINE   (THEMES[g_theme].line)
#define TTEXT   (THEMES[g_theme].text)
#define TDIM    (THEMES[g_theme].dim)
#define TAC     (THEMES[g_theme].accent)
#define TACD    (THEMES[g_theme].accentD)
#define TWN     (THEMES[g_theme].warn)
#define TWNBG   (THEMES[g_theme].warnBg)
#define TER     (THEMES[g_theme].err)
#define TERBG   (THEMES[g_theme].errBg)
#define TBTN    (THEMES[g_theme].btn)
#define TBTNL   (THEMES[g_theme].btnLine)

/* Theme style helpers - which rendering path a theme uses. */
static int dotmatrix_theme(void) { return (g_theme == 0 || g_theme == 4); }
static int vfd_theme(void)      { return (g_theme == 4); }
static int elglow_theme(void)   { return (g_theme == 5); }


/* forward declaration: the ini reader logs before logf_() is defined */
static void logf_(const char *fmt, ...);

/* ============ UNITS + LANGUAGES ============ */
/* Everything is stored internally in metric (km/h, km, m).
 * Imperial is only a display conversion: mph, miles, feet. */
static char g_font_name[32] = "";   /* font= from hud.ini (forward decl) */
static int g_imperial = 0;   /* 0 = metric, 1 = imperial (hud.ini: units=imperial) */
static int g_clock_24h = 1;  /* hud.ini: clock24=0 for a 12-hour clock */

/* Language ids. Auto-detected from the OS locale at startup, overridable
 * with lang=xx in hud.ini. */
enum { LANG_PL=0, LANG_EN, LANG_DE, LANG_ES, LANG_FR, LANG_IT,
       LANG_RU, LANG_NL, LANG_PT, LANG_CZ, LANG_COUNT };

static const char *LANG_CODES[LANG_COUNT] = {
    "pl","en","de","es","fr","it","ru","nl","pt","cz"
};

/* String ids - keep in sync with STR_TABLE columns. */
enum {
    S_ODO=0, S_ALT, S_MAX, S_RESET, S_THEME, S_FONT,
    S_SCAN, S_NOGPS, S_WAITFIX, S_SAT, S_KMH, S_MPH, S_M, S_FT,
    S_KM, S_MI, S_COUNT
};

/* Every row has S_COUNT entries; order must match the enum above. */
static const WCHAR *STR_TABLE[LANG_COUNT][S_COUNT] = {
 /* PL */ { L"LICZNIK", L"WYSOKOSC", L"max", L"RESET", L"WYGLAD", L"FONT",
            L"szukam portu GPS...", L"BRAK GPS - retry 5 s", L"czekam na fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* EN */ { L"ODOMETER", L"ALTITUDE", L"max", L"RESET", L"THEME", L"FONT",
            L"scanning GPS ports...", L"NO GPS - retry 5 s", L"waiting for fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* DE */ { L"KILOMETER", L"HOEHE", L"max", L"RESET", L"DESIGN", L"SCHRIFT",
            L"suche GPS-Port...", L"KEIN GPS - 5 s", L"warte auf Fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* ES */ { L"CUENTAKM", L"ALTITUD", L"max", L"RESET", L"TEMA", L"FUENTE",
            L"buscando puerto GPS...", L"SIN GPS - 5 s", L"esperando fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* FR */ { L"COMPTEUR", L"ALTITUDE", L"max", L"RESET", L"THEME", L"POLICE",
            L"recherche port GPS...", L"PAS DE GPS - 5 s", L"attente du fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* IT */ { L"CONTACHILOM.", L"ALTITUDINE", L"max", L"RESET", L"TEMA", L"CARATTERE",
            L"cerco porta GPS...", L"NO GPS - 5 s", L"attendo il fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* RU */ { L"ODOMETR", L"VYSOTA", L"max", L"SBROS", L"TEMA", L"SHRIFT",
            L"poisk porta GPS...", L"NET GPS - 5 s", L"zhdu fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* NL */ { L"KILOMETER", L"HOOGTE", L"max", L"RESET", L"THEMA", L"LETTER",
            L"GPS-poort zoeken...", L"GEEN GPS - 5 s", L"wacht op fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* PT */ { L"ODOMETRO", L"ALTITUDE", L"max", L"RESET", L"TEMA", L"FONTE",
            L"procurando porta GPS...", L"SEM GPS - 5 s", L"aguardando fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* CZ */ { L"POCITADLO", L"VYSKA", L"max", L"RESET", L"VZHLED", L"FONT",
            L"hledam port GPS...", L"ZADNE GPS - 5 s", L"cekam na fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
};

static int g_lang = LANG_PL;   /* default: Polish */

/* Shortcut: localized string by id. */
static const WCHAR *T(int id)
{
    if (id < 0 || id >= S_COUNT) return L"";
    return STR_TABLE[g_lang][id];
}

/* Map a 2-letter code to a language id; -1 when unknown. */
static int lang_from_code(const char *c)
{
    int i;
    char lo[3];
    if (!c || !c[0]) return -1;
    lo[0] = (char)((c[0] >= 'A' && c[0] <= 'Z') ? c[0] + 32 : c[0]);
    lo[1] = (char)((c[1] >= 'A' && c[1] <= 'Z') ? c[1] + 32 : c[1]);
    lo[2] = 0;
    for (i = 0; i < LANG_COUNT; i++)
        if (strcmp(lo, LANG_CODES[i]) == 0) return i;
    return -1;
}

/* ============ hud.ini ============ */
/* Format, one key per line, ';' or '#' starts a comment:
 *     units = metric | imperial
 *     lang  = pl | en | de | es | fr | it | ru | nl | pt | cz
 *     theme = 0..3
 */
static void ini_apply_line(const char *line)
{
    char key[32], val[48];
    const char *p = line;
    int k = 0, v = 0;
    /* font= handling needs the candidate list, which is defined above */

    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == ';' || *p == '#' || *p == '[') return;

    while (*p && *p != '=' && *p != ' ' && *p != '\t' && k < 31) key[k++] = *p++;
    key[k] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '=') p++;
    while (*p == ' ' || *p == '\t') p++;
    while (*p && *p != '\r' && *p != '\n' && *p != ';' && *p != '#' && v < 47) {
        if (*p != ' ' && *p != '\t') { val[v++] = *p; }
        p++;
    }
    val[v] = 0;
    if (k == 0 || v == 0) return;

    if (_stricmp(key, "units") == 0) {
        g_imperial = (_stricmp(val, "imperial") == 0 || _stricmp(val, "mi") == 0) ? 1 : 0;
        logf_("ini: units=%s\n", g_imperial ? "imperial" : "metric");
    } else if (_stricmp(key, "lang") == 0 || _stricmp(key, "language") == 0) {
        int li = lang_from_code(val);
        if (li >= 0) { g_lang = li; logf_("ini: lang=%s\n", val); }
        else logf_("ini: nieznany jezyk '%s'\n", val);
    } else if (_stricmp(key, "font") == 0) {
        /* remembered here, resolved once the font table is known */
        g_font_name[0] = 0;
        {
            int wi;
            for (wi = 0; wi < 31 && val[wi]; wi++) g_font_name[wi] = val[wi];
            g_font_name[wi] = 0;
        }
        logf_("ini: font=%s\n", val);
    } else if (_stricmp(key, "clock24") == 0) {
        g_clock_24h = (atoi(val) != 0) ? 1 : 0;
        logf_("ini: clock24=%d\n", g_clock_24h);
    } else if (_stricmp(key, "theme") == 0) {
        int t = -1;
        if (val[0] >= '0' && val[0] <= '9') {
            t = atoi(val);
        } else {
            int i;
            for (i = 0; i < THEME_COUNT; i++) {
                char wn[32]; int wi;
                for (wi = 0; wi < 31 && THEME_NAMES[i][wi]; wi++)
                    wn[wi] = (char)THEME_NAMES[i][wi];
                wn[wi] = 0;
                if (_stricmp(wn, val) == 0) { t = i; break; }
            }
        }
        if (t >= 0 && t < THEME_COUNT) { g_theme = t; logf_("ini: theme=%d\n", t); }
        else logf_("ini: nieznany motyw '%s'\n", val);
    }
}

static void ini_load(void)
{
    FILE *f = fopen(INI_FILE_A, "r");
    char line[128];
    if (!f) f = fopen(INI_FILE_B, "r");
    if (!f) { logf_("hud.ini brak - metric + jezyk z systemu\n"); return; }
    while (fgets(line, sizeof(line), f)) ini_apply_line(line);
    fclose(f);
}

/* Locale from the OS: WinCE GetUserDefaultLCID / GetSystemDefaultLCID
 * primary language id. Fallback when hud.ini has no lang= key. */
static void lang_detect(void)
{
    LCID lcid = GetUserDefaultLCID();
    if (!lcid) lcid = GetSystemDefaultLCID();
    switch (lcid & 0xFF) {
    case 0x15: g_lang = LANG_PL; break;   /* Polish        */
    case 0x09: g_lang = LANG_EN; break;   /* English       */
    case 0x07: g_lang = LANG_DE; break;   /* German        */
    case 0x0A: g_lang = LANG_ES; break;   /* Spanish       */
    case 0x0C: g_lang = LANG_FR; break;   /* French        */
    case 0x10: g_lang = LANG_IT; break;   /* Italian       */
    case 0x19: g_lang = LANG_RU; break;   /* Russian       */
    case 0x13: g_lang = LANG_NL; break;   /* Dutch         */
    case 0x16: g_lang = LANG_PT; break;   /* Portuguese    */
    case 0x05: g_lang = LANG_CZ; break;   /* Czech         */
    default:   g_lang = LANG_PL; break;   /* PL is the default */
    }
    logf_("jezyk z systemu: %s\n", LANG_CODES[g_lang]);
}

/* ============ UNIT CONVERSION ============ */
#define KMH_TO_MPH  0.6213712
#define M_TO_FT     3.2808399
#define KM_TO_MI    0.6213712

/* Display helpers - defined after the GPS state globals they read. */
static int  speed_disp(void);
static int  speed_max_disp(void);
static void odo_disp(int *whole, int *frac);
static int  alt_disp(void);

/* ============ LOG (no buffering) ============ */
static FILE *g_log = NULL;

static void log_open(void)
{
    g_log = fopen(LOG_FILE, "w");
    if (!g_log) g_log = fopen(LOG_FILE_B, "w");
    if (!g_log) return;
    setvbuf(g_log, NULL, _IONBF, 0);
    fprintf(g_log, "=== Lark HUD log ===\n");
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

/* ============ STAN GPS ============ */
static volatile int    g_fix;
static volatile int    g_sats;       /* uzyte (GGA) */
static volatile int    g_sats_view;  /* widoczne (GSV) */
static volatile int    g_speed_kmh;
static volatile int    g_alt_m;
static volatile int    g_alt_ok;   /* GGA kiedykolwiek podalo wysokosc */
static volatile int    g_hdop10;   /* HDOP * 10, 9999 = brak */
static volatile int    g_course;   /* azymut 0-359 stopni */
static volatile int    g_course_ok;
static volatile int    g_speed_max; /* maks predkosc w sesji */
static volatile int    g_batt_pct;  /* -1 = nieznane */
static volatile int    g_batt_ac;   /* 1 = na ladowaniu */
static volatile int    g_fixmode;   /* GSA pole 2: 1=brak,2=2D,3=3D */
static volatile int    g_marq;      /* przewijanie paska statusu (px) */
static volatile double g_lat, g_lon;

static HANDLE       g_hGps = INVALID_HANDLE_VALUE;
static volatile int g_running = 1;

/* ============ AUTO-SKAN ============ */
static const WCHAR *g_ports[] = {
    L"COM1:", L"COM2:", L"COM3:", L"COM4:", L"COM5:",
    L"COM6:", L"COM7:", L"COM8:", L"COM9:",
    L"COM0:", L"GPD0:", L"GPS1:"
};
static const DWORD g_bauds[] = {
    CBR_4800, CBR_9600, CBR_38400, CBR_57600, CBR_19200, CBR_115200
};
#define N_PORTS (int)(sizeof(g_ports)/sizeof(g_ports[0]))
#define N_BAUDS (int)(sizeof(g_bauds)/sizeof(g_bauds[0]))

static volatile int   g_scanning = 1;
static WCHAR          g_used_port[16] = L"?";
static volatile DWORD g_used_baud;
static volatile int   g_probe_result;
static volatile DWORD g_total_bytes;
static volatile DWORD g_nmea_count;
static volatile DWORD g_last_rx_tick;

/* ============ LICZNIK ============ */
static double g_odo_km = 0.0;

/* ============ UNIT CONVERSION (bodies) ============ */
/* Everything is stored in metric; these only convert for display. */
static int speed_disp(void)
{
    if (!g_imperial) return g_speed_kmh;
    return (int)(g_speed_kmh * KMH_TO_MPH + 0.5);
}
static int speed_max_disp(void)
{
    if (!g_imperial) return g_speed_max;
    return (int)(g_speed_max * KMH_TO_MPH + 0.5);
}
static void odo_disp(int *whole, int *frac)
{
    double v = g_imperial ? (g_odo_km * KM_TO_MI) : g_odo_km;
    int w = (int)v;
    int f = (int)((v - w) * 10.0);
    if (f < 0) f = 0;
    if (f > 9) f = 9;
    *whole = w; *frac = f;
}
static int alt_disp(void)
{
    if (!g_imperial) return g_alt_m;
    return (int)(g_alt_m * M_TO_FT + 0.5);
}
static double g_last_lat, g_last_lon;
static int    g_have_last;
static DWORD  g_last_tick;

/* ============ FONTY (pixlowate) ============ */
static HFONT g_fSpeed, g_fUnit, g_fLabel, g_fVal, g_fBtn, g_fTop;

/* ============ NMEA SENTENCES (queue for the status bar) ============ */
#define MSG_N     8
#define MSG_LEN   72
static char  g_msgs[MSG_N][MSG_LEN];
static volatile int g_msg_head;     /* 0 = najstarszy */
static volatile int g_msg_count;
static char  g_cks_bad[MSG_N][MSG_LEN];   /* ostatnie linie z bledna suma kontrolna */
static volatile int g_cks_bad_count;

static void msg_push(const char *line)
{
    int slot;
    if (g_msg_count < MSG_N) {
        slot = (g_msg_head + g_msg_count) % MSG_N;
        g_msg_count++;
    } else {
        slot = g_msg_head;
        g_msg_head = (g_msg_head + 1) % MSG_N;
    }
    {
        int i;
        for (i = 0; i < MSG_LEN - 1 && line[i]; i++) g_msgs[slot][i] = line[i];
        g_msgs[slot][i] = 0;
    }
}

static const char *msg_get(int i)
{
    if (i < 0 || i >= g_msg_count) return "";
    return g_msgs[(g_msg_head + i) % MSG_N];
}

static void cks_bad_push(const char *line)
{
    int slot = g_cks_bad_count % MSG_N;
    int i;
    for (i = 0; i < MSG_LEN - 1 && line[i]; i++) g_cks_bad[slot][i] = line[i];
    g_cks_bad[slot][i] = 0;
    if (g_cks_bad_count < MSG_N) g_cks_bad_count++;
}

/* ============ MATH (no libc sin/cos) ============ */
static double deg2rad(double d) { return d * PI_D / 180.0; }

static double my_cos(double x)
{
    double x2, x4, x6, x8, x10, t;
    while (x > PI_D)  x -= 2*PI_D;
    while (x < -PI_D) x += 2*PI_D;
    x2 = x*x;
    t = 1.0 - x2/2.0;
    x4 = x2*x2;  t += x4/24.0;
    x6 = x4*x2;  t -= x6/720.0;
    x8 = x4*x4;  t += x8/40320.0;
    x10 = x8*x2; t -= x10/3628800.0;
    return t;
}

static double my_sin(double x) { return my_cos(x - PI_D / 2.0); }
static double my_sqrt(double x)
{
    double r; int i;
    if (x <= 0) return 0;
    r = x > 1 ? x : 1;
    for (i = 0; i < 20; i++) r = 0.5*(r + x/r);
    return r;
}

static double dist_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double avg = deg2rad((lat1+lat2)/2.0);
    double dx = deg2rad(lon2-lon1) * my_cos(avg);
    double dy = deg2rad(lat2-lat1);
    return R * my_sqrt(dx*dx + dy*dy);
}

static void w2a(const WCHAR *src, char *dst, int maxlen)
{
    int i;
    for (i = 0; i < maxlen-1 && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

/* ============ WYBOR FONTU ============ */
/* Search paths for the Minecraft TTF (SD card or \Windows) */
#define FONT_TTF_A    "\\Storage Card\\Minecraft.ttf"
#define FONT_TTF_B    "\\SDMMC\\Minecraft.ttf"
#define FONT_TTF_C    "\\Windows\\Minecraft.ttf"

static int g_minecraft_on = 0;   /* 1 = Minecraft.ttf loaded via AddFontResourceW */

static const WCHAR *g_font_candidates[] = {
    L"Minecraft",       /* z pliku Minecraft.ttf (AddFontResourceW) */
    L"Terminal",        /* pixel bitmap font, present on every WinCE */
    L"Fixedsys",
    L"MS Sans Serif",
    L"Tahoma",
    L"Courier New",
    L"Arial"
};
static int g_font_pick = -1;
static int font_works(const WCHAR *face, int size, int idx);
#define FONT_COUNT ((int)(sizeof(g_font_candidates)/sizeof(g_font_candidates[0])))

static void font_load(void)
{
    FILE *f = fopen(FONT_FILE_A, "r");
    int v = -1;
    if (!f) f = fopen(FONT_FILE_B, "r");
    if (f) {
        if (fscanf(f, "%d", &v) == 1) { }
        fclose(f);
    }
    if (v >= 0 && v < FONT_COUNT) {
        /* make sure the saved font actually exists in the system */
        if (font_works(g_font_candidates[v], -16, v)) {
            g_font_pick = v;
            logf_("font z pliku: %S (idx=%d)\n", g_font_candidates[v], v);
            return;
        }
        logf_("font z pliku %S niedostepny, auto\n", g_font_candidates[v]);
    }
    g_font_pick = -1;
}

static void font_save(void)
{
    FILE *f = fopen(FONT_FILE_A, "w");
    if (!f) f = fopen(FONT_FILE_B, "w");
    if (f) { fprintf(f, "%d\n", g_font_pick); fclose(f); }
}

static void font_resolve(void)
{
    int i;
    if (g_font_pick >= 0) return;
    for (i = 0; i < FONT_COUNT; i++) {
        if (font_works(g_font_candidates[i], -16, i)) { g_font_pick = i; break; }
    }
    /* 1) hud.ini font= wins over everything (unless the face is missing) */
    if (g_font_pick < 0 && g_font_name[0]) {
        int i, want = -1;
        if (g_font_name[0] >= '0' && g_font_name[0] <= '9') {
            int t = atoi(g_font_name);
            if (t >= 0 && t < FONT_COUNT) want = t;
        } else {
            WCHAR wv[32];
            int wi;
            for (wi = 0; wi < 31 && g_font_name[wi]; wi++)
                wv[wi] = (WCHAR)g_font_name[wi];
            wv[wi] = 0;
            for (i = 0; i < FONT_COUNT; i++)
                if (_wcsicmp(wv, g_font_candidates[i]) == 0) { want = i; break; }
        }
        if (want >= 0 && font_works(g_font_candidates[want], -16, want)) {
            g_font_pick = want;
            logf_("font z ini: %S (idx=%d)\n", g_font_candidates[want], want);
            return;
        }
        logf_("font z ini '%s' niedostepny, auto\n", g_font_name);
    }
    /* 2) default font = Minecraft (index 0) when its TTF is loaded, else the
     * first face the system really has */
    if (g_font_pick < 0 && g_minecraft_on &&
        font_works(g_font_candidates[0], -16, 0))
        g_font_pick = 0;
    if (g_font_pick < 0) g_font_pick = FONT_COUNT - 1;
    logf_("font wybrany: %S (idx=%d)\n", g_font_candidates[g_font_pick], g_font_pick);
}

/* ============ THEME / ODOMETER files ============ */
static void theme_load(void)
{
    FILE *f = fopen(THEME_FILE_A, "r");
    int v = 0;
    if (!f) f = fopen(THEME_FILE_B, "r");
    if (f) {
        if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < THEME_COUNT) g_theme = v;
        fclose(f);
    }
}

static void theme_save(void)
{
    FILE *f = fopen(THEME_FILE_A, "w");
    if (!f) f = fopen(THEME_FILE_B, "w");
    if (f) { fprintf(f, "%d\n", g_theme); fclose(f); }
}

static void odo_save(void)
{
    FILE *f = fopen(ODO_FILE_A, "w");
    int whole, frac;
    if (!f) f = fopen(ODO_FILE_B, "w");
    if (!f) { logf_("ODO SAVE FAIL\n"); return; }
    whole = (int)g_odo_km;
    frac  = (int)((g_odo_km - whole) * 1000.0);
    if (frac < 0) frac = 0;
    if (frac > 999) frac = 999;
    fprintf(f, "%d.%03d\n", whole, frac);
    fclose(f);
}

static void odo_load(void)
{
    FILE *f = fopen(ODO_FILE_A, "r");
    double v = 0.0;
    if (!f) f = fopen(ODO_FILE_B, "r");
    if (f) {
        if (fscanf(f, "%lf", &v) == 1 && v >= 0.0 && v < 1e7) g_odo_km = v;
        fclose(f);
    }
}

/* ============ NMEA ============ */
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
        char *star = strchr(fields[n-1], '*');
        if (star) *star = 0;
    }
    return n;
}

static void parse_rmc(char **f, int n)
{
    double knots;
    if (n < 8) return;
    if (f[2][0] != 'A') { g_fix = 0; return; }
    if (g_fixmode == 0) g_fixmode = 3;
    knots = atof(f[7]);
    g_speed_kmh = (int)(knots * 1.852 + 0.5);
    if (g_speed_kmh < 0) g_speed_kmh = 0;
    if (g_speed_kmh > 999) g_speed_kmh = 999;
    if (g_speed_kmh > g_speed_max) g_speed_max = g_speed_kmh;
    if (n > 8 && f[8] && f[8][0]) {
        int c = atoi(f[8]);
        if (c < 0) c = 0;
        g_course = c % 360;
        g_course_ok = 1;
    }
    if (f[3][0] && f[5][0]) {
        double rawlat = atof(f[3]);
        double rawlon = atof(f[5]);
        int dlat = (int)(rawlat/100.0);
        int dlon = (int)(rawlon/100.0);
        double lat = dlat + (rawlat - dlat*100.0)/60.0;
        double lon = dlon + (rawlon - dlon*100.0)/60.0;
        if (f[4][0]=='S') lat = -lat;
        if (f[6][0]=='W') lon = -lon;
        g_lat = lat; g_lon = lon; g_fix = 1;
    }
}

static void parse_gga(char **f, int n)
{
    if (n < 10) return;
    g_sats = atoi(f[7]);
    if (f[8] && f[8][0]) {
        /* HDOP like "0.9" -> 9 */
        int w = atoi(f[8]);
        int d = 0;
        const char *q = f[8];
        while (*q && *q != '.') q++;
        if (*q == '.') {
            if (q[1] >= '0' && q[1] <= '9') d = q[1] - '0';
        }
        g_hdop10 = w * 10 + d;
        if (g_hdop10 > 999) g_hdop10 = 999;
    } else {
        g_hdop10 = 9999;
    }
    if (atoi(f[6]) > 0) {
        g_fix = 1;
        if (f[9] && f[9][0]) {
            g_alt_m = atoi(f[9]);
            g_alt_ok = 1;
        }
        /* position from GGA as a fallback when RMC does not carry it */
        if (n > 5 && f[2][0] && f[4][0]) {
            double rawlat = atof(f[2]);
            double rawlon = atof(f[4]);
            int dlat = (int)(rawlat/100.0);
            int dlon = (int)(rawlon/100.0);
            double lat = dlat + (rawlat - dlat*100.0)/60.0;
            double lon = dlon + (rawlon - dlon*100.0)/60.0;
            if (f[3][0]=='S') lat = -lat;
            if (f[5][0]=='W') lon = -lon;
            if (lat > -90.0 && lat < 90.0 && lon > -180.0 && lon < 180.0) {
                g_lat = lat; g_lon = lon;
                g_course_ok = g_course_ok;   /* unchanged */
            }
        }
    } else {
        g_fix = 0;
    }
}

static void parse_gsv(char **f, int n)
{
    if (n < 4) return;
    {
        int v = atoi(f[3]);
        if (v >= 0 && v <= 32) g_sats_view = v;
    }
}

/* NMEA checksum: XOR between '$' and '*'. Returns 1 = OK. */
static int nmea_cks_ok(const char *line)
{
    const char *a = line;
    const char *star;
    unsigned char cs = 0;
    if (*a == '$') a++;
    star = strchr(a, '*');
    if (!star) return -1;            /* no checksum - accept it */
    while (a < star) cs ^= (unsigned char)*a++;
    {
        int hi = star[1], lo = star[2], v;
        if (!hi || !lo) return -1;
        hi = (hi >= '0' && hi <= '9') ? hi - '0' :
             (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 :
             (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
        lo = (lo >= '0' && lo <= '9') ? lo - '0' :
             (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 :
             (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        v = (hi << 4) | lo;
        return (v == (int)cs) ? 1 : 0;
    }
}

static void parse_gsa(char **f, int n)
{
    /* $GPGSA,A,3,05,12,...,1.8,1.2,1.4
       f[1]=A(auto), f[2]=1/2/3 (fix), f[15]=PDOP f[16]=HDOP f[17]=VDOP */
    if (n < 3) return;
    {
        int v = atoi(f[2]);
        if (v >= 1 && v <= 3) g_fixmode = v;
    }
    if (n > 16 && f[16] && f[16][0] && g_hdop10 == 9999) {
        int w = atoi(f[16]), d = 0;
        const char *q = f[16];
        while (*q && *q != '.') q++;
        if (*q == '.') d = (q[1] >= '0' && q[1] <= '9') ? q[1] - '0' : 0;
        g_hdop10 = w * 10 + d;
        if (g_hdop10 > 999) g_hdop10 = 999;
    }
}

static void nmea_line(char *line)
{
    char *f[24];
    int n, cks;
    const char *s, *type;
    if (line[0] != '$') return;

    cks = nmea_cks_ok(line);
    if (cks == 0) {                       /* checksum mismatch - reject, but log it */
        cks_bad_push(line);
        g_nmea_count++;
        return;
    }

    /* copy into the message queue (before split_nmea destroys the buffer) */
    msg_push(line);

    n = split_nmea(line, f, 24);
    if (n < 1) return;
    s = f[0];
    if (strlen(s) < 5) return;
    type = s + strlen(s) - 3;
    g_nmea_count++;
    if (!strcmp(type,"RMC")) parse_rmc(f,n);
    else if (!strcmp(type,"GGA")) parse_gga(f,n);
    else if (!strcmp(type,"GSA")) parse_gsa(f,n);
    else if (!strcmp(type,"VTG")) {
        /* $GPVTG,course,T,courseM,M,speedN,N,speedK,K
           f[7] = speed in km/h - independent of RMC */
        if (n > 7 && f[7] && f[7][0]) {
            double k = atof(f[7]);
            int kmh = (int)(k + 0.5);
            if (kmh >= 0 && kmh <= 999) {
                g_speed_kmh = kmh;
                if (kmh > g_speed_max) g_speed_max = kmh;
            }
        }
        if (n > 1 && f[1] && f[1][0] && !g_course_ok) {
            int c = (int)(atof(f[1]) + 0.5) % 360;
            g_course = c; g_course_ok = 1;
        }
    }
    else if (!strcmp(type,"GNS")) {
        if (n >= 10 && f[9] && f[9][0]) { g_alt_m = atoi(f[9]); g_alt_ok = 1; }
        /* GNS f[6] = mode: A/D/P = autonomous/differential/precise */
        if (n > 6 && f[6] && (f[6][0]=='A' || f[6][0]=='D' || f[6][0]=='P'))
            g_fixmode = 3;
    }
    else if (!strcmp(type,"GSV")) parse_gsv(f,n);
}

/* ============ GPS POWER (GPD0) ============ */
static void gps_power_on(void)
{
    HANDLE h;
    DWORD ret = 0;
    logf_("GPD0 power-on proba...\n");
    h = CreateFileW(L"GPD0:", GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("  GPD0 brak (err=%d), pomijam\n", (int)GetLastError());
        return;
    }
    if (DeviceIoControl(h, IOCTL_SERVICE_REFRESH, NULL,0,NULL,0,&ret,NULL))
        logf_("  GPD0 REFRESH ok\n");
    else
        logf_("  GPD0 REFRESH fail err=%d\n", (int)GetLastError());
    if (DeviceIoControl(h, IOCTL_SERVICE_START, NULL,0,NULL,0,&ret,NULL))
        logf_("  GPD0 START ok\n");
    else
        logf_("  GPD0 START fail err=%d\n", (int)GetLastError());
    CloseHandle(h);
}

/* ============ WYSYLANIE KOMEND (checksum liczony!) ============ */
static void send_body(HANDLE h, const char *body)
{
    unsigned char cs = 0;
    const char *p;
    char full[128];
    int len = 0, i;
    DWORD w = 0;
    for (p = body; *p; p++) cs ^= (unsigned char)(*p);
    len = 0;
    full[len++] = '$';
    for (p = body; *p && len < 110; p++) full[len++] = *p;
    full[len++] = '*';
    {
        const char *hex = "0123456789ABCDEF";
        full[len++] = hex[(cs >> 4) & 15];
        full[len++] = hex[cs & 15];
    }
    full[len++] = '\r'; full[len++] = '\n'; full[len] = 0;
    WriteFile(h, full, (DWORD)len, &w, NULL);
    for (i = 0; full[i]; i++) {
        if (full[i] == '\r' || full[i] == '\n') break;
    }
    logf_("  TX: %s (w=%d)\n", body, (int)w);
}

static void wakeup_for_baud(HANDLE h, DWORD baud)
{
    (void)h; (void)baud;
    /* NO TX AT ALL: the receiver is already configured for NMEA 4800.
     * Wysylanie PSRF/PMTK moze przelaczyc chip w tryb binarny i zabic port. */
    logf_("  (no TX - port already NMEA 4800)\n");
}

/* ============ PORT ============ */
static HANDLE open_port(const WCHAR *port, DWORD baud, int do_wakeup)
{
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    char name[32];
    DWORD errs = 0;
    COMSTAT st;

    w2a(port, name, sizeof(name));
    h = CreateFileW(port, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("OPEN %s @%d -> FAIL err=%d\n", name, (int)baud, (int)GetLastError());
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
        if (!SetCommState(h,&dcb))
            logf_("  SetCommState FAIL err=%d\n",(int)GetLastError());
    } else {
        logf_("  GetCommState FAIL err=%d\n",(int)GetLastError());
    }
    to.ReadIntervalTimeout = 10;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 30;   /* was 100 ms */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 0;
    SetCommTimeouts(h,&to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    if (do_wakeup) {
        /* Wakeup used to be followed by Sleep(1500): with two candidate
         * ports that alone delayed the first NMEA line by ~4.5 s. The
         * receiver answers within a few hundred ms, so we only give it
         * 300 ms and then start reading straight away. */
        wakeup_for_baud(h, baud);
        Sleep(300);
    }
    memset(&st, 0, sizeof(st));
    if (ClearCommError(h, &errs, &st))
        logf_("  COMSTAT in=%d out=%d errs=%d\n", (int)st.cbInQue, (int)st.cbOutQue, (int)errs);
    else
        logf_("  ClearCommError FAIL err=%d\n", (int)GetLastError());
    return h;
}


/* ============ DIAGNOSTICS: COM ports from the registry ============ */
static void log_com_registry(void)
{
    static const WCHAR *keys[] = {
        L"Drivers\\BuiltIn\\Serial",
        L"Drivers\\BuiltIn\\Serial2",
        L"Drivers\\BuiltIn\\GPS",
        L"Drivers\\BuiltIn\\GPSID",
        L"Drivers\\BuiltIn\\SerialGPS"
    };
    int k;
    logf_("--- rejestr COM ---\n");
    for (k = 0; k < (int)(sizeof(keys)/sizeof(keys[0])); k++) {
        HKEY hk;
        WCHAR name[64], prefix[32];
        DWORD idx = 0, type, sz;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[k], 0, 0, &hk) != ERROR_SUCCESS) {
            logf_("  %S: brak\n", keys[k]);
            continue;
        }
        sz = sizeof(prefix); prefix[0] = 0;
        if (RegQueryValueExW(hk, L"Prefix", NULL, &type, (BYTE*)prefix, &sz) == ERROR_SUCCESS) {
            sz = sizeof(idx);
            RegQueryValueExW(hk, L"Index", NULL, &type, (BYTE*)&idx, &sz);
            wsprintfW(name, L"%s%d:", prefix, (int)idx);
            logf_("  %S -> %S\n", keys[k], name);
        } else {
            logf_("  %S: jest, brak Prefix\n", keys[k]);
        }
        RegCloseKey(hk);
    }
    logf_("--- koniec rejestru ---\n");
}

/* ============ WATEK GPS ============ */
static DWORD WINAPI gps_thread(LPVOID param)
{
    static char rx[4096];
    static char buf[512];
    (void)param;
    logf_("gps_thread start\n");

    log_com_registry();

    while (g_running) {
        int ok = 0, pi, bi;

        g_scanning = 1;
        g_probe_result = 0;

        if (FIXED_PORT[0] && !ok) {
            HANDLE h = open_port(FIXED_PORT, FIXED_BAUD ? FIXED_BAUD : CBR_4800, 1);
            if (h != INVALID_HANDLE_VALUE) {
                /* make sure data is really flowing before accepting the port */
                int probe_ok = 0, prxn = 0;
                DWORD pdl = GetTickCount() + 1200;
                DWORD pgot;
                while (GetTickCount() < pdl && g_running && !probe_ok) {
                    pgot = 0;
                    if (!ReadFile(h, buf, sizeof(buf), &pgot, NULL) || pgot == 0) {
                        Sleep(10);   /* do not spin, but do not sleep long */
                        continue;
                    }
                    g_total_bytes += pgot;
                    g_last_rx_tick = GetTickCount();
                    {
                        DWORD k;
                        for (k = 0; k < pgot; k++) {
                            char c = buf[k];
                            if (c == '\n' || c == '\r') {
                                if (prxn > 6 && rx[0] == '$') { probe_ok = 1; break; }
                                prxn = 0;
                            } else if (c == '$') { prxn = 0; rx[prxn++] = c; }
                            else if (prxn < (int)sizeof(rx)-2) rx[prxn++] = c;
                            else prxn = 0;
                        }
                    }
                }
                logf_("FIXED_PORT probe: %s\n", probe_ok ? "NMEA OK" : "cisza");
                if (probe_ok) {
                    wcscpy(g_used_port, FIXED_PORT);
                    g_used_baud = FIXED_BAUD ? FIXED_BAUD : CBR_4800;
                    g_hGps = h; g_scanning = 0; g_probe_result = 1; ok = 1;
                    logf_("using FIXED_PORT\n");
                } else {
                    CloseHandle(h);
                    logf_("FIXED_PORT cisza, lece ze skanem\n");
                }
            }
        }

        if (FIXED_PORT2[0] && !ok) {
            HANDLE h = open_port(FIXED_PORT2, FIXED_BAUD ? FIXED_BAUD : CBR_4800, 1);
            if (h != INVALID_HANDLE_VALUE) {
                int probe_ok = 0, prxn = 0;
                DWORD pdl = GetTickCount() + 1200;
                DWORD pgot;
                while (GetTickCount() < pdl && g_running && !probe_ok) {
                    pgot = 0;
                    if (!ReadFile(h, buf, sizeof(buf), &pgot, NULL) || pgot == 0) {
                        Sleep(10);
                        continue;
                    }
                    g_total_bytes += pgot;
                    g_last_rx_tick = GetTickCount();
                    {
                        DWORD k;
                        for (k = 0; k < pgot; k++) {
                            char c = buf[k];
                            if (c == '\n' || c == '\r') {
                                if (prxn > 6 && rx[0] == '$') { probe_ok = 1; break; }
                                prxn = 0;
                            } else if (c == '$') { prxn = 0; rx[prxn++] = c; }
                            else if (prxn < (int)sizeof(rx)-2) rx[prxn++] = c;
                            else prxn = 0;
                        }
                    }
                }
                logf_("FIXED_PORT2 probe: %s\n", probe_ok ? "NMEA OK" : "cisza");
                if (probe_ok) {
                    wcscpy(g_used_port, FIXED_PORT2);
                    g_used_baud = FIXED_BAUD ? FIXED_BAUD : CBR_4800;
                    g_hGps = h; g_scanning = 0; g_probe_result = 1; ok = 1;
                    logf_("using FIXED_PORT2\n");
                } else {
                    CloseHandle(h);
                    logf_("FIXED_PORT2 cisza, lece ze skanem\n");
                }
            }
        }

        for (pi = 0; pi < N_PORTS && !ok && g_running; pi++) {
            for (bi = 0; bi < N_BAUDS && !ok && g_running; bi++) {
                HANDLE h;
                int saw = 0, sawbin = 0, rxn = 0;
                DWORD deadline, total = 0, i, got;
                int shown = 0;

                h = open_port(g_ports[pi], g_bauds[bi], 0);
                if (h == INVALID_HANDLE_VALUE) continue;

                /* PHASE A: listen for 1.5 s WITHOUT transmitting anything */
                rxn = 0;
                deadline = GetTickCount() + 1500;
                while (GetTickCount() < deadline && g_running && !saw) {
                    got = 0;
                    if (!ReadFile(h, buf, sizeof(buf), &got, NULL) || got == 0)
                        continue;
                    total += got;
                    g_total_bytes += got;
                    g_last_rx_tick = GetTickCount();
                    {
                        DWORD k;
                        for (k = 0; k + 1 < got; k++)
                            if ((unsigned char)buf[k]==0xA0 && (unsigned char)buf[k+1]==0xA2)
                                sawbin = 1;
                    }
                    if (!shown && g_log) {
                        int k, lim = got < 48 ? (int)got : 48;
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
                        } else if (c == '$') { rxn = 0; rx[rxn++] = c; }
                        else if (rxn < (int)sizeof(rx)-2) rx[rxn++] = c;
                        else rxn = 0;
                    }
                }

                /* PHASE B: silence -> send wakeup and listen 2.5 s more */
                if (!saw && !sawbin) {
                    wakeup_for_baud(h, g_bauds[bi]);
                    deadline = GetTickCount() + 2500;
                    while (GetTickCount() < deadline && g_running && !saw) {
                        got = 0;
                        if (!ReadFile(h, buf, sizeof(buf), &got, NULL) || got == 0)
                            continue;
                        total += got;
                        g_total_bytes += got;
                        g_last_rx_tick = GetTickCount();
                        {
                            DWORD k;
                            for (k = 0; k + 1 < got; k++)
                                if ((unsigned char)buf[k]==0xA0 && (unsigned char)buf[k+1]==0xA2)
                                    sawbin = 1;
                        }
                        if (!shown && g_log) {
                            int k, lim = got < 48 ? (int)got : 48;
                            logf_("  RX-POST[%d]: ", (int)got);
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
                            } else if (c == '$') { rxn = 0; rx[rxn++] = c; }
                            else if (rxn < (int)sizeof(rx)-2) rx[rxn++] = c;
                            else rxn = 0;
                        }
                    }
                }

                logf_("  total %d bytes nmea=%s bin=%s\n",
                      (int)total, saw ? "YES" : "no", sawbin ? "YES" : "no");
                if (saw || sawbin) {
                    char pn[32];
                    g_hGps = h;
                    wcscpy(g_used_port, g_ports[pi]);
                    g_used_baud = g_bauds[bi];
                    g_scanning = 0; g_probe_result = 1; ok = 1;
                    w2a(g_ports[pi], pn, sizeof(pn));
                    logf_("FOUND %s @%d%s\n", pn, (int)g_bauds[bi],
                          sawbin && !saw ? " (binary)" : "");
                } else CloseHandle(h);
            }
        }

        if (!ok) {
            g_scanning = 0; g_probe_result = -1;
            logf_("NO GPS PORT FOUND — retry za 5 s\n");
            {
                DWORD t0 = GetTickCount();
                while (g_running && GetTickCount() - t0 < 5000) Sleep(500);
            }
            continue;
        }

        {
            int rxn = 0;
            DWORD lines = 0, lastwake = GetTickCount();
            logf_("entering read loop\n");
            g_last_rx_tick = GetTickCount();
            while (g_running) {
                DWORD i, got = 0;
                if (!ReadFile(g_hGps, buf, sizeof(buf), &got, NULL) || got == 0) {
                    if (GetTickCount() - g_last_rx_tick > 10000) {
                        logf_("WATCHDOG: 10 s ciszy, reskan\n");
                        break;
                    }
                    /* keep-alive: wakeup every 5 s */
                    if (GetTickCount() - lastwake > 5000) {
                        wakeup_for_baud(g_hGps, g_used_baud);
                        lastwake = GetTickCount();
                    }
                    Sleep(15);   /* was 50 ms - that alone capped updates at 20 Hz */
                    continue;
                }
                g_last_rx_tick = GetTickCount();
                g_total_bytes += got;
                for (i = 0; i < got; i++) {
                    char c = buf[i];
                    if (c == '\n' || c == '\r') {
                        if (rxn > 6) {
                            rx[rxn] = 0;
                            nmea_line(rx);
                            if ((lines++ % 60) == 0)
                                logf_("line %d: %.64s fix=%d sats=%d/%d spd=%d\n",
                                      (int)lines, rx, g_fix, g_sats,
                                      g_sats_view, g_speed_kmh);
                        }
                        rxn = 0;
                    } else if (c == '$') {
                        rxn = 0; rx[rxn++] = c;
                    } else if (rxn < (int)sizeof(rx)-2) {
                        rx[rxn++] = c;
                    } else rxn = 0;
                }
            }
            if (g_hGps != INVALID_HANDLE_VALUE) {
                CloseHandle(g_hGps);
                g_hGps = INVALID_HANDLE_VALUE;
            }
            logf_("read loop exit, reskan\n");
        }
    }
    logf_("gps_thread exit\n");
    return 0;
}

/* ============ BATERIA ============ */
#ifndef AC_LINE_ONLINE
#define AC_LINE_ONLINE 0x01
#endif
#ifndef BATTERY_FLAG_CHARGING
#define BATTERY_FLAG_CHARGING 0x08
#endif

typedef struct {
    BYTE  ACLineStatus;
    BYTE  BatteryFlag;
    BYTE  BatteryLifePercent;
    BYTE  Reserved1;
    DWORD BatteryLifeTime;
    DWORD BatteryFullLifeTime;
} MY_POWER;

static void batt_read(void)
{
    HINSTANCE hC = LoadLibraryW(L"coredll.dll");
    int got = 0;
    if (hC) {
        typedef BOOL (WINAPI *PFN1)(MY_POWER *, DWORD, DWORD *);
        typedef BOOL (WINAPI *PFN0)(SYSTEM_POWER_STATUS_EX *, BOOL);
        PFN1 f2 = (PFN1)GetProcAddress(hC, L"GetSystemPowerStatusEx2");
        PFN0 f1 = (PFN0)GetProcAddress(hC, L"GetSystemPowerStatusEx");
        if (f2) {
            MY_POWER ps; DWORD sz = sizeof(ps);
            memset(&ps, 0, sizeof(ps));
            if (f2(&ps, sizeof(ps), &sz)) {
                if (ps.BatteryLifePercent != 255) g_batt_pct = (int)ps.BatteryLifePercent;
                else g_batt_pct = -1;
                g_batt_ac = (ps.ACLineStatus == AC_LINE_ONLINE ||
                             (ps.BatteryFlag & BATTERY_FLAG_CHARGING)) ? 1 : 0;
                got = 1;
            }
        }
        if (!got && f1) {
            SYSTEM_POWER_STATUS_EX ps; DWORD sz = 0;
            memset(&ps, 0, sizeof(ps));
            if (f1(&ps, TRUE)) {
                if (ps.BatteryLifePercent != 255) g_batt_pct = (int)ps.BatteryLifePercent;
                else g_batt_pct = -1;
                g_batt_ac = (ps.ACLineStatus == AC_LINE_ONLINE ||
                             (ps.BatteryFlag & BATTERY_FLAG_CHARGING)) ? 1 : 0;
                (void)sz;
                got = 1;
            }
        }
        FreeLibrary(hC);
    }
    if (!got && g_batt_pct == 0) g_batt_pct = -1;
}

/* ============ DRAWING ============ */
/* Fonts in order of preference: pixel/retro ones first.
 * The first one the system actually has is used. */

static HFONT mkfont_face(const WCHAR *face, int size, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = size;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = NONANTIALIASED_QUALITY;   /* zero wygladzania = ostry pixel */
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE; /* do not force FIXED - proportional TTF (Minecraft/Tahoma/Arial)
                           * would otherwise resolve to the same fallback face */
    wcsncpy(lf.lfFaceName, face, 31);
    lf.lfFaceName[31] = 0;
    return CreateFontIndirectW(&lf);
}

static int g_font_tested[8];
static int g_font_ok[8];
/* note: FONT_COUNT = 7, a cache of 8 is enough */

static int font_works(const WCHAR *face, int size, int idx)
{
    /* idx 0 = Minecraft: do not cache, availability changes
     * po AddFontResourceW (plik moze pojawic sie / zniknac). */
    if (idx != 0 && idx >= 0 && idx < 8 && g_font_tested[idx]) return g_font_ok[idx];
    HFONT f = mkfont_face(face, size, FW_BOLD);
    HDC dc; HFONT old; TEXTMETRICW tm; int ok = 0;
    if (!f) return 0;
    dc = GetDC(NULL);
    if (dc) {
        old = (HFONT)SelectObject(dc, f);
        if (GetTextMetricsW(dc, &tm)) {
            /* if the system substituted another font, the name differs */
            WCHAR real[32];
            if (GetTextFaceW(dc, 32, real) > 0) {
                if (_wcsicmp(real, face) == 0) ok = 1;
            } else ok = 1;
        }
        SelectObject(dc, old);
        ReleaseDC(NULL, dc);
    }
    DeleteObject(f);
    if (idx != 0 && idx >= 0 && idx < 8) { g_font_tested[idx] = 1; g_font_ok[idx] = ok; }
    return ok;
}

static HFONT mkfont(int size, int weight)
{
    font_resolve();
    return mkfont_face(g_font_candidates[g_font_pick], size, weight);
}

static void make_fonts(void)
{
    if (g_fSpeed) DeleteObject(g_fSpeed);
    if (g_fUnit)  DeleteObject(g_fUnit);
    if (g_fLabel) DeleteObject(g_fLabel);
    if (g_fVal)   DeleteObject(g_fVal);
    if (g_fBtn)   DeleteObject(g_fBtn);
    if (g_fTop)   DeleteObject(g_fTop);
    g_fSpeed = mkfont(-96, FW_BOLD);
    g_fUnit  = mkfont(-22, FW_BOLD);
    g_fLabel = mkfont(-14, FW_BOLD);
    g_fVal   = mkfont(-24, FW_BOLD);
    g_fBtn   = mkfont(-16, FW_BOLD);
    g_fTop   = mkfont(-12, FW_BOLD);
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r; HBRUSH b = CreateSolidBrush(c);
    r.left=x; r.top=y; r.right=x+w; r.bottom=y+h;
    FillRect(dc,&r,b); DeleteObject(b);
}

static void text_at(HDC dc, int x, int y, const WCHAR *s, COLORREF c, HFONT f)
{
    RECT r; HFONT old = (HFONT)SelectObject(dc,f);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc,c);
    r.left=x; r.top=y; r.right=x+300; r.bottom=y+200;
    DrawTextW(dc,s,-1,&r,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOCLIP);
    SelectObject(dc,old);
}

static int text_w(HDC dc, const WCHAR *s, HFONT f)
{
    RECT r; HFONT old = (HFONT)SelectObject(dc,f);
    SetBkMode(dc,TRANSPARENT);
    r.left=0; r.top=0; r.right=0; r.bottom=0;
    DrawTextW(dc,s,-1,&r,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_CALCRECT|DT_NOCLIP);
    SelectObject(dc,old);
    return r.right - r.left;
}

static int text_h(HDC dc, const WCHAR *s, HFONT f)
{
    RECT r; HFONT old = (HFONT)SelectObject(dc,f);
    SetBkMode(dc,TRANSPARENT);
    r.left=0; r.top=0; r.right=0; r.bottom=0;
    DrawTextW(dc,s,-1,&r,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_CALCRECT|DT_NOCLIP);
    SelectObject(dc,old);
    return r.bottom - r.top;
}

static void text_center(HDC dc, int cx, int y, const WCHAR *s, COLORREF c, HFONT f)
{
    text_at(dc, cx - text_w(dc,s,f)/2, y, s, c, f);
}


/* ============ DOT-MATRIX RENDERER ============
 * Draws digits/letters as 5x7 cells of square "LED" dots. Every lit dot is
 * drawn with a darker 1px border, so the grid stays visible even inside a
 * lit character - that is what gives the VERSO theme its dot-matrix LCD
 * (OLED behind glass) look. Unlit dots are drawn very dark red, like the
 * switched-off segments of a real matrix display.
 */
#define DOT_W 5
#define DOT_H 7

/* 5x7 font, one string per row, '#' = lit. Only the glyphs the HUD uses. */
static const char *dm_glyph(WCHAR c)
{
    switch (c) {
    case L'0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
    case L'1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
    case L'2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
    case L'3': return "11111" "00010" "00100" "00010" "00001" "10001" "01110";
    case L'4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
    case L'5': return "11111" "10000" "11110" "00001" "00001" "10001" "01110";
    case L'6': return "00110" "01000" "10000" "11110" "10001" "10001" "01110";
    case L'7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
    case L'8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
    case L'9': return "01110" "10001" "10001" "01111" "00001" "00010" "01100";
    case L'-': return "00000" "00000" "00000" "11111" "00000" "00000" "00000";
    case L'.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
    case L' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    case L'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case L'B': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
    case L'C': return "01110" "10001" "10000" "10000" "10000" "10001" "01110";
    case L'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
    case L'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case L'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case L'G': return "01110" "10001" "10000" "10111" "10001" "10001" "01111";
    case L'H': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
    case L'I': return "01110" "00100" "00100" "00100" "00100" "00100" "01110";
    case L'J': return "00111" "00010" "00010" "00010" "00010" "10010" "01100";
    case L'K': return "10001" "10010" "10100" "11000" "10100" "10010" "10001";
    case L'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    case L'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case L'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
    case L'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case L'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
    case L'Q': return "01110" "10001" "10001" "10001" "10101" "10010" "01101";
    case L'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case L'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
    case L'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case L'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
    case L'V': return "10001" "10001" "10001" "10001" "10001" "01010" "00100";
    case L'W': return "10001" "10001" "10001" "10101" "10101" "11011" "10001";
    case L'X': return "10001" "10001" "01010" "00100" "01010" "10001" "10001";
    case L'Y': return "10001" "10001" "01010" "00100" "00100" "00100" "00100";
    case L'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
    default:  return 0;
    }
}

/* Width of a dot-matrix string in pixels: cell = scale+gap, 1 cell gap between chars. */
static int dm_w(const WCHAR *s, int scale, int gap)
{
    int n = 0, cw = (scale + gap);
    while (*s) { if (*s != L' ') n++; s++; }
    if (n == 0) return 0;
    return n * DOT_W * cw + (n - 1) * cw;
}
static int dm_h(int scale, int gap)
{
    return DOT_H * (scale + gap) - gap;
}

/* Draw one dot: lit = bright core + darker bevel so the grid stays visible. */
static void dm_dot(HDC dc, int x, int y, int scale, int gap,
                   int lit, COLORREF on, COLORREF off, COLORREF grid)
{
    int sz = scale + gap;
    (void)grid;                                        /* grid overlay removed */
    if (lit) {
        fill(dc, x, y, sz, sz, on);                    /* solid lit dot */
    } else {
        fill(dc, x, y, sz, sz, off);                   /* solid unlit dot */
    }
}

/* Draw a string as dot matrix. scale = px per dot, gap = px of grid between. */
static void dm_text(HDC dc, int x, int y, const WCHAR *s, int scale, int gap,
                    int lit_all, COLORREF on, COLORREF off, COLORREF grid)
{
    int cw = (DOT_W * (scale + gap));
    int cx = x;
    while (*s) {
        const char *g = dm_glyph(*s);
        if (g) {
            int r, c;
            for (r = 0; r < DOT_H; r++) {
                for (c = 0; c < DOT_W; c++) {
                    int lit = (g[r * DOT_W + c] == '#') && lit_all;
                    dm_dot(dc, cx + c * (scale + gap), y + r * (scale + gap),
                           scale, gap, lit, on, off, grid);
                }
            }
        }
        cx += cw + (scale + gap);
        s++;
    }
}

/* Centre a dot-matrix string. */
static void dm_center(HDC dc, int cxmid, int y, const WCHAR *s,
                      int scale, int gap, int lit_all,
                      COLORREF on, COLORREF off, COLORREF grid)
{
    dm_text(dc, cxmid - dm_w(s, scale, gap) / 2, y, s, scale, gap,
            lit_all, on, off, grid);
}

/* Dot-matrix colours for the active theme (used only by VERSO). */
#define TM_ON   TAC
#define TM_OFF  TACD
#define TM_GRID TLINE

static void round_rect(HDC dc,int x,int y,int w,int h,int rad,COLORREF fc,COLORREF lc)
{
    HBRUSH b=(HBRUSH)CreateSolidBrush(fc); HPEN p=CreatePen(PS_SOLID,1,lc);
    HBRUSH ob=(HBRUSH)SelectObject(dc,b); HPEN op=(HPEN)SelectObject(dc,p);
    RoundRect(dc,x,y,x+w,y+h,rad,rad);
    SelectObject(dc,ob); SelectObject(dc,op);
    DeleteObject(b); DeleteObject(p);
}

/* panel_box: square recessed VFD box for VFD theme, rounded otherwise. */
static void panel_box(HDC dc, int x, int y, int w, int h)
{
    if (vfd_theme()) {
        fill(dc, x, y, w, h, TPANEL);
        {
            HPEN pen = CreatePen(PS_SOLID, 1, TLINE);
            HPEN op = (HPEN)SelectObject(dc, pen);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, x, y, x + w, y + h);
            {
                HPEN sp = CreatePen(PS_SOLID, 1, RGB(0,0,0));
                HPEN op2 = (HPEN)SelectObject(dc, sp);
                MoveToEx(dc, x + 1, y + h - 2, NULL);
                LineTo(dc, x + 1, y + 1);
                LineTo(dc, x + w - 2, y + 1);
                SelectObject(dc, op2);
                DeleteObject(sp);
            }
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        }
    } else {
        round_rect(dc, x, y, w, h, 8, TPANEL, TLINE);
    }
}

static void odo_tick(void)
{
    DWORD now = GetTickCount();
    if (!g_fix) { g_have_last = 0; g_last_tick = now; return; }
    if (g_have_last && g_last_tick) {
        double dt = (double)(now - g_last_tick)/1000.0;
        if (dt > 0.2 && dt < 30.0) {
            double d = dist_m(g_last_lat,g_last_lon,g_lat,g_lon);
            if (d > 0.5 && d < 500.0) g_odo_km += d/1000.0;
        }
        g_last_lat = g_lat; g_last_lon = g_lon;
    } else {
        g_last_lat = g_lat; g_last_lon = g_lon; g_have_last = 1;
    }
    g_last_tick = now;
}

/* ============ BUTTON LAYOUT (shared by drawing and touch) ============
 * Przycisk X jest w prawym GORNYM rogu, wiec pasek statusu konczy sie przed nim.
 * Guziki FONT / WYGLAD / RESET stoja w JEDNYM rzedzie przy DOLNEJ krawedzi. */
#define BTN_H    22          /* kept for the X button geometry only */
#define BTN_GAP  4
#define BTN_R    6
#define BOT_H    18          /* bottom status bar height */
#define TOP_H    18          /* wysokosc paska statusu */
#define X_W      26          /* szerokosc guzika X */

static void btn_row(int W, int H, int *rw, int *rx, int *ry)
{
    *rw = (W - BTN_R*2 - BTN_GAP*2) / 3;
    if (*rw > 110) *rw = 110;
    *ry = H - BTN_H - BTN_R;
    *rx = (W - (*rw * 3 + BTN_GAP * 2)) / 2;
}

static void draw_screen(HDC dc, int W, int H)
{
    WCHAR buf[80];
    int gpsOk = g_fix;
    int cx, spY, bw, bh, by, rw, rh, rx, ry;
    int odo_w, odo_f;
    int sats_show = gpsOk ? g_sats : g_sats_view;

    fill(dc,0,0,W,H,TBG);

    /* ---------- STATUS BAR (scrolling) ---------- */
    {
        int px = 6;                       /* lewa krawedz tekstu */
        int ph = TOP_H;
        int avail = W - X_W - 6 - px;     /* miejsce do guzika X */
        COLORREF bg = TACD, fg = TAC;

        if (g_scanning)          { bg = TWNBG; fg = TWN; }
        else if (g_probe_result == -1) { bg = TERBG; fg = TER; }
        else if (!gpsOk)         { bg = TWNBG; fg = TWN; }

        fill(dc, 0, 0, W, ph, bg);

        /* bar text: either status or the last NMEA sentence (scrolling) */
        {
            WCHAR wbuf[160];

            /* right side: battery + signal, always visible */
            {
                int rxp = W - X_W - 4;

                /* clock: HH:MM, top right corner, left of the X button */
                {
                    WCHAR tb[16];
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    if (g_clock_24h)
                        wsprintfW(tb, L"%02d:%02d", st.wHour, st.wMinute);
                    else {
                        int h12 = st.wHour % 12;
                        if (h12 == 0) h12 = 12;
                        wsprintfW(tb, L"%d:%02d", h12, st.wMinute);
                    }
                    if (dotmatrix_theme()) {
                        /* VERSO: dot-matrix clock, same look as the digits */
                        int tw = dm_w(tb, 2, 1);
                        dm_text(dc, rxp - tw, (ph - dm_h(2,1)) / 2, tb,
                                2, 1, 1, TTEXT, TACD, TLINE);
                        rxp -= (tw + 8);
                    } else {
                        int tw = text_w(dc, tb, g_fBtn);
                        text_at(dc, rxp - tw, (ph - 12) / 2 - 1, tb, TTEXT, g_fBtn);
                        rxp -= (tw + 8);
                    }
                }

                /* signal: HDOP bars */
                {
                    int bars = 0, i;
                    COLORREF sc;
                    if (gpsOk && g_hdop10 != 9999 && g_hdop10 < 200) {
                        if (g_hdop10 <= 10)      bars = 4;
                        else if (g_hdop10 <= 20) bars = 3;
                        else if (g_hdop10 <= 40) bars = 2;
                        else if (g_hdop10 <= 80) bars = 1;
                    } else if (g_sats_view > 0) {
                        bars = (g_sats_view >= 8) ? 3 : (g_sats_view >= 4 ? 2 : 1);
                    }
                    sc = (bars >= 3) ? TAC : (bars >= 1 ? TWN : TER);
                    for (i = 0; i < 4; i++) {
                        int bh2 = 3 + i * 2;
                        fill(dc, rxp - 26 + i * 5, ph - 3 - bh2, 4, bh2,
                             (i < bars) ? sc : TPANEL);
                    }
                    rxp -= 32;
                }
                /* battery: tiny icon + percent */
                if (g_batt_pct >= 0) {
                    COLORREF bc = g_batt_ac ? TAC : (g_batt_pct < 20 ? TER : TTEXT);
                    WCHAR tw[8];
                    int iw = 18, ih = 9;
                    fill(dc, rxp - iw - 2, (ph - ih) / 2 + 1, iw, ih, bc);
                    fill(dc, rxp - iw + 1, (ph - ih) / 2 + 3,
                         (iw - 2) * g_batt_pct / 100, ih - 4, bg);
                    fill(dc, rxp - 1, (ph - ih) / 2 + 3, 2, ih - 4, bc);
                    rxp -= (iw + 6);
                    wsprintfW(tw, L"%d", g_batt_pct);
                    text_at(dc, rxp - text_w(dc, tw, g_fTop), (ph - 12) / 2,
                            tw, bc, g_fTop);
                    rxp -= (text_w(dc, tw, g_fTop) + 6);
                }
                avail = rxp - px;
            }
            if (avail < 40) avail = 40;

            /* TOP BAR TEXT REMOVED ON REQUEST:
             * the GPS status lives in the bottom bar now, the top bar only
             * keeps the background, the clock, battery, signal and X. */

            /* content: port status first, then the last sentence */
#if 0
            if (g_scanning)
                wcscpy(wbuf, T(S_SCAN));
            else if (g_probe_result == -1)
                wcscpy(wbuf, T(S_NOGPS));
            else if (gpsOk) {
                WCHAR wtmp[40];
                wsprintfW(wtmp, L"%s %d %db %dsat ", g_used_port,
                          (int)g_used_baud, g_sats);
                wcscpy(wbuf, wtmp);
                /* append the last NMEA sentence - that is what scrolls */
                if (g_msg_count > 0) {
                    WCHAR wm[MSG_LEN + 2];
                    int i;
                    const char *m = msg_get(g_msg_count - 1);
                    for (i = 0; i < MSG_LEN - 1 && m[i]; i++) wm[i] = (WCHAR)m[i];
                    wm[i] = 0;
                    wcscat(wbuf, wm);
                }
            } else {
                wsprintfW(wbuf, L"%s %d - %s %d/%d",
                          g_used_port, (int)g_used_baud, T(S_WAITFIX), g_sats, g_sats_view);
                if (g_msg_count > 0) {
                    WCHAR wm[MSG_LEN + 2];
                    int i;
                    const char *m = msg_get(g_msg_count - 1);
                    for (i = 0; i < MSG_LEN - 1 && m[i]; i++) wm[i] = (WCHAR)m[i];
                    wm[i] = 0;
                    wcscat(wbuf, L" ");
                    wcscat(wbuf, wm);
                }
            }

            /* scroll only when the text does not fit the available width */
            {
                int tw = text_w(dc, wbuf, g_fTop);
                int off = 0;
                if (tw > avail) {
                    int span = tw + 30;               /* + przerwa */
                    off = g_marq % span;
                }
                {
                    HRGN rgn = CreateRectRgn(px, 0, px + avail, ph);
                    SelectClipRgn(dc, rgn);
                    text_at(dc, px - off, (ph - 12) / 2, wbuf, fg, g_fTop);
                    SelectClipRgn(dc, NULL);
                    DeleteObject(rgn);
                }
            }
#endif
        }

        /* X button - top right corner, full bar height */
        fill(dc, W - X_W, 0, X_W, ph, TERBG);
        text_center(dc, W - X_W / 2, (ph - 14) / 2 + 1, L"X", TER, g_fBtn);
    }

    /* ---------- SPEED ---------- */
    if (gpsOk) wsprintfW(buf,L"%d",g_speed_kmh);
    else wcscpy(buf,L"--");
    /* Speed block: vertically centred in the free area between the top bar
     * and the compass row, horizontally centred on the whole screen. */
    {
        const WCHAR *unit = g_imperial ? T(S_MPH) : T(S_KMH);
        int wunit = text_w(dc, unit, g_fUnit);
        int hUnit = text_h(dc, unit, g_fUnit);
        int gap = 12;
        int freeTop  = TOP_H + 2;
        int freeBot  = H - BOT_H - 8 - 14 - 6;   /* above the compass row */
        int freeH    = freeBot - freeTop;

        cx = W / 2;
        if (freeH < 40) freeH = 40;

        if (elglow_theme()) {
            /* GAUGE: analog speedometer with a needle that moves with speed */
            int cy = freeTop + freeH / 2;
            int radius = freeH / 2 - 6;
            int i, spd;
            double maxv, frac;
            if (radius > 95) radius = 95;
            if (radius < 40) radius = 40;
            spd = gpsOk ? g_speed_kmh : 0;
            maxv = g_imperial ? 120.0 : 200.0;
            frac = (double)spd / maxv;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            /* dial face */
            {
                HBRUSH br = CreateSolidBrush(TPANEL);
                HPEN pen = CreatePen(PS_SOLID, 2, TLINE);
                HBRUSH ob = (HBRUSH)SelectObject(dc, br);
                HPEN op = (HPEN)SelectObject(dc, pen);
                Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(br); DeleteObject(pen);
            }
            /* tick marks */
            for (i = 0; i <= 10; i++) {
                double a = PI_D * (1.0 - (double)i / 10.0);
                int x1 = cx + (int)((radius - 4) * my_cos(a));
                int y1 = cy - (int)((radius - 4) * my_sin(a));
                int x2 = cx + (int)((radius - 14) * my_cos(a));
                int y2 = cy - (int)((radius - 14) * my_sin(a));
                HPEN tp = CreatePen(PS_SOLID, 1, TDIM);
                HPEN op = (HPEN)SelectObject(dc, tp);
                MoveToEx(dc, x1, y1, NULL);
                LineTo(dc, x2, y2);
                SelectObject(dc, op);
                DeleteObject(tp);
            }
            /* needle */
            {
                double a = PI_D * (1.0 - frac);
                int nx = cx + (int)((radius - 18) * my_cos(a));
                int ny = cy - (int)((radius - 18) * my_sin(a));
                HPEN np = CreatePen(PS_SOLID, 3, TAC);
                HPEN op = (HPEN)SelectObject(dc, np);
                MoveToEx(dc, cx, cy, NULL);
                LineTo(dc, nx, ny);
                SelectObject(dc, op);
                DeleteObject(np);
            }
            /* center cap */
            {
                HBRUSH br = CreateSolidBrush(TAC);
                HPEN pen = CreatePen(PS_SOLID, 1, TAC);
                HBRUSH ob = (HBRUSH)SelectObject(dc, br);
                HPEN op = (HPEN)SelectObject(dc, pen);
                Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(br); DeleteObject(pen);
            }
            /* speed value below the dial */
            {
                WCHAR nb[16];
                int th;
                if (gpsOk) wsprintfW(nb, L"%d", speed_disp());
                else wcscpy(nb, L"--");
                th = text_h(dc, nb, g_fVal);
                text_center(dc, cx, cy + radius + 4, nb, gpsOk ? TTEXT : TDIM, g_fVal);
                text_center(dc, cx, cy + radius + 4 + th + 2, unit, TDIM, g_fUnit);
            }
        } else if (dotmatrix_theme()) {
            /* VERSO / VFD: large dot-matrix digits, scaled to the free height */
            int sc = 12, gp = 2;
            int wn, hh, total, x0;
            /* VFD: 24-block segmented speed bar at the top of the free area */
            if (vfd_theme()) {
                int spd = gpsOk ? g_speed_kmh : 0;
                double maxv = g_imperial ? 120.0 : 200.0;
                int filled = (int)((double)spd / maxv * 24.0 + 0.5);
                int segs = 24, i;
                int segW = (W - 20) / segs;
                int barY = freeTop + 2;
                int barH = 10;
                if (filled < 0) filled = 0;
                if (filled > segs) filled = segs;
                for (i = 0; i < segs; i++) {
                    int sx = 10 + i * segW;
                    COLORREF fc = (i < filled) ? TAC : TPANEL;
                    fill(dc, sx + 1, barY, segW - 2, barH, fc);
                    /* recessed cell outline */
                    {
                        HPEN pen = CreatePen(PS_SOLID, 1, TLINE);
                        HPEN op = (HPEN)SelectObject(dc, pen);
                        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                        Rectangle(dc, sx + 1, barY, sx + segW - 1, barY + barH);
                        SelectObject(dc, op); SelectObject(dc, ob);
                        DeleteObject(pen);
                    }
                }
                freeTop += barH + 6;
                freeH = freeBot - freeTop;
                if (freeH < 40) freeH = 40;
            }
            /* shrink the dot size until the block fits the free area */
            while (sc > 4 && dm_h(sc, gp) > freeH) sc--;
            wn = dm_w(buf, sc, gp);
            hh = dm_h(sc, gp);
            total = wn + gap + wunit;
            x0 = cx - total / 2;
            spY = freeTop + (freeH - hh) / 2;
            dm_text(dc, x0, spY, buf, sc, gp, gpsOk, TM_ON, TM_OFF, TM_GRID);
            /* VFD: scanline effect over the center speed digits */
            if (vfd_theme()) {
                int sy;
                for (sy = spY; sy < spY + hh; sy += 3) {
                    fill(dc, x0, sy, wn, 1, RGB(0,0,0));
                }
            }
            text_at(dc, x0 + wn + gap, spY + hh - hUnit - 4, unit, TDIM, g_fUnit);
        } else {
            HFONT fBig = g_fSpeed;
            int wh, wnum, total, x0, hSpeed, yUnit;
            /* the built-in face is fixed-size: centre it instead */
            wnum   = text_w(dc, buf, fBig);
            hSpeed = text_h(dc, buf, fBig);
            wh     = hSpeed;
            if (wh > freeH && wh > 0) {
                /* font too tall for the area - fall back to the value face */
                fBig = g_fVal;
                wnum = text_w(dc, buf, fBig);
                hSpeed = text_h(dc, buf, fBig);
            }
            total = wnum + gap + wunit;
            x0 = cx - total / 2;
            spY = freeTop + (freeH - hSpeed) / 2;
            yUnit = spY + hSpeed - hUnit - 6;
            text_at(dc, x0, spY, buf, gpsOk?TTEXT:TDIM, fBig);
            text_at(dc, x0 + wnum + gap, yUnit, unit, TDIM, g_fUnit);
        }
    }
    {
        /* compass (left) + max speed (right) */
        static const WCHAR *DIRS[8] = { L"N",L"NE",L"E",L"SE",L"S",L"SW",L"W",L"NW" };
        WCHAR cb[40];
        int cdx = (g_course + 22) / 45;
        int yrow;
        if (cdx > 7) cdx = 0;
        if (gpsOk && g_course_ok)
            wsprintfW(cb, L"%s %d\xb0", DIRS[cdx], g_course);
        else
            wcscpy(cb, L"--");
        yrow = H - BOT_H - 8 - 14;
        text_center(dc, W/2 - 70, yrow, cb, gpsOk?TAC:TDIM, g_fLabel);
        wsprintfW(cb, L"max %d", g_speed_max);
        text_center(dc, W/2 + 70, yrow, cb, TDIM, g_fLabel);
    }

    /* ---------- PANELS: odometer + altitude (above the button row) ---------- */
    {
        int panel_h = 46;
        int panel_by = H - BOT_H - 8 - panel_h;
        bw=(W-18)/2; bh=panel_h; by=panel_by;

        panel_box(dc,6,by,bw,bh);
        text_at(dc,14,by+4,T(S_ODO),TDIM,g_fLabel);
        odo_disp(&odo_w, &odo_f);
        wsprintfW(buf,L"%d.%d %s",odo_w,odo_f,
                  g_imperial ? T(S_MI) : T(S_KM));
        if (dotmatrix_theme()) {
            WCHAR nb[24], ub[8];
            wsprintfW(nb,L"%d.%d",odo_w,odo_f);
            wsprintfW(ub,L"%s", g_imperial ? T(S_MI) : T(S_KM));
            dm_text(dc,14,by+19,nb,3,1,1,TM_ON,TM_OFF,TM_GRID);
            text_at(dc,14+dm_w(nb,3,1)+5,by+26,ub,TDIM,g_fLabel);
        } else {
            text_at(dc,14,by+20,buf,TAC,g_fVal);
        }

        panel_box(dc,12+bw,by,bw,bh);
        text_at(dc,20+bw,by+4,T(S_ALT),TDIM,g_fLabel);
        {
            const WCHAR *au = g_imperial ? T(S_FT) : T(S_M);
            if (gpsOk && g_alt_ok) wsprintfW(buf,L"%d %s",alt_disp(),au);
            else if (gpsOk)        wsprintfW(buf,L"--- %s",au);
            else                   wsprintfW(buf,L"-- %s",au);
        }
        if (dotmatrix_theme()) {
            WCHAR nb[24], ub[8];
            if (gpsOk && g_alt_ok) wsprintfW(nb,L"%d",alt_disp());
            else wcscpy(nb,L"---");
            wsprintfW(ub,L"%s", g_imperial ? T(S_FT) : T(S_M));
            dm_text(dc,20+bw,by+19,nb,3,1,gpsOk,TTEXT,TPANEL,TLINE);
            text_at(dc,20+bw+dm_w(nb,3,1)+5,by+26,ub,TDIM,g_fLabel);
        } else {
            text_at(dc,20+bw,by+20,buf,gpsOk?TTEXT:TDIM,g_fVal);
        }
    }

    /* ---------- BOTTOM BAR: same GPS data as the top left corner ----------
     * The three on-screen buttons are gone; theme, font and units are set
     * from hud.ini only. This bar mirrors what the top bar shows on the
     * left, so it stays readable while the top bar is scrolling. */
    {
        int by2 = H - BOT_H;
        int px = 6;
        int avail = W - 62 - px;
        COLORREF bg = TACD, fg = TAC;
        WCHAR wbuf[160];

        if (g_scanning)                { bg = TWNBG; fg = TWN; }
        else if (g_probe_result == -1) { bg = TERBG; fg = TER; }
        else if (!gpsOk)               { bg = TWNBG; fg = TWN; }

        fill(dc, 0, by2, W, BOT_H, bg);

        if (g_scanning)
            wcscpy(wbuf, T(S_SCAN));
        else if (g_probe_result == -1)
            wcscpy(wbuf, T(S_NOGPS));
        else if (gpsOk)
            wsprintfW(wbuf, L"%s %d %db %d/%d", g_used_port,
                      (int)g_used_baud, g_sats, g_sats_view);
        else
            wsprintfW(wbuf, L"%s %d - %s %d/%d", g_used_port,
                      (int)g_used_baud, T(S_WAITFIX), g_sats, g_sats_view);

        {
            int tw = text_w(dc, wbuf, g_fTop);
            int off = 0;
            if (tw > avail) off = g_marq % (tw + 30);
            text_at(dc, px - off, by2 + (BOT_H - 12) / 2, wbuf, fg, g_fTop);
        }
        /* MENU button - bottom right */
        {
            int mx = W - 58;
            fill(dc, mx, by2, 54, BOT_H, TBTN);
            {
                HPEN pen = CreatePen(PS_SOLID, 1, TBTNL);
                HPEN op = (HPEN)SelectObject(dc, pen);
                HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, mx, by2, mx + 54, by2 + BOT_H);
                SelectObject(dc, op); SelectObject(dc, ob);
                DeleteObject(pen);
            }
            text_center(dc, mx + 27, by2 + (BOT_H - 12) / 2, L"MENU", TTEXT, g_fTop);
        }
    }
    /* on-screen settings menu overlay */
    if (g_menu_open) {
        int mx = W - 250;
        int my = TOP_H + 4;
        int mw = 240;
        int mh = 6 * 34 + 8;
        int i;
        const WCHAR *items[6];
        items[0] = L"Theme";
        items[1] = L"Font";
        items[2] = L"Units";
        items[3] = L"Language";
        items[4] = L"Clock 24h";
        items[5] = L"Close";
        /* panel */
        fill(dc, mx, my, mw, mh, TPANEL);
        {
            HPEN pen = CreatePen(PS_SOLID, 1, TLINE);
            HPEN op = (HPEN)SelectObject(dc, pen);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, mx, my, mx + mw, my + mh);
            SelectObject(dc, op); SelectObject(dc, ob);
            DeleteObject(pen);
        }
        for (i = 0; i < 6; i++) {
            int iy = my + 4 + i * 34;
            fill(dc, mx + 4, iy, mw - 8, 30, (i == 5) ? TERBG : TBTN);
            {
                HPEN pen = CreatePen(PS_SOLID, 1, TBTNL);
                HPEN op = (HPEN)SelectObject(dc, pen);
                HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, mx + 4, iy, mx + mw - 4, iy + 30);
                SelectObject(dc, op); SelectObject(dc, ob);
                DeleteObject(pen);
            }
            text_at(dc, mx + 10, iy + (30 - 12) / 2, items[i], TTEXT, g_fBtn);
            /* value on the right */
            {
                WCHAR val[32];
                if (i == 0) wsprintfW(val, L"%s", THEME_NAMES[g_theme]);
                else if (i == 1) wsprintfW(val, L"%s", g_font_candidates[g_font_pick]);
                else if (i == 2) wcscpy(val, g_imperial ? L"imperial" : L"metric");
                else if (i == 3) {
                    char c0 = LANG_CODES[g_lang][0], c1 = LANG_CODES[g_lang][1];
                    wsprintfW(val, L"%c%c", (WCHAR)c0, (WCHAR)c1);
                }
                else if (i == 4) wcscpy(val, g_clock_24h ? L"1" : L"0");
                else wcscpy(val, L"");
                if (i < 5) {
                    int vw = text_w(dc, val, g_fBtn);
                    text_at(dc, mx + mw - 10 - vw, iy + (30 - 12) / 2, val, TAC, g_fBtn);
                }
            }
        }
    }
}

/* ============ OKNO ============ */
static HWND g_hwnd;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        {
            /* Try to load Minecraft.ttf from the card or from \Windows.
             * On WinCE the file just has to be there - AddFontResourceW
             * registers it without a restart. */
            const char *ttf_try[] = { FONT_TTF_A, FONT_TTF_B, FONT_TTF_C };
            int ti;
            for (ti = 0; ti < 3; ti++) {
                FILE *tf = fopen(ttf_try[ti], "rb");
                if (tf) {
                    fclose(tf);
                    {
                        WCHAR wttf[64];
                        int ki;
                        for (ki = 0; ki < 63 && ttf_try[ti][ki]; ki++)
                            wttf[ki] = (WCHAR)(unsigned char)ttf_try[ti][ki];
                        wttf[ki] = 0;
                        if (AddFontResourceW(wttf) > 0) {
                            g_minecraft_on = 1;
                            logf_("Minecraft.ttf zaladowany: %s\n", ttf_try[ti]);
                            break;
                        } else {
                            logf_("AddFontResource FAIL: %s\n", ttf_try[ti]);
                        }
                    }
                }
            }
            if (!g_minecraft_on)
                logf_("Minecraft.ttf nie znaleziony (karta/Windows)\n");
        }
        font_load();
        make_fonts();
        logf_("WM_CREATE: okno gotowe\n");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc; RECT r;
        dc = BeginPaint(hwnd,&ps);
        GetClientRect(hwnd,&r);
        /* Double buffering: draw the WHOLE screen into memory, then blit
         * it in one BitBlt. Without this every FillRect/DrawText went to
         * the screen separately and the result flickered. */
        {
            HDC mem = CreateCompatibleDC(dc);
            if (mem) {
                HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
                if (bmp) {
                    HGDIOBJ old = SelectObject(mem, bmp);
                    draw_screen(mem, r.right, r.bottom);
                    BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteObject(bmp);
                } else {
                    draw_screen(dc, r.right, r.bottom);
                }
                DeleteDC(mem);
            } else {
                draw_screen(dc, r.right, r.bottom);
            }
        }
        EndPaint(hwnd,&ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x=LOWORD(lp), y=HIWORD(lp);
        RECT r;
        int W, H;
        GetClientRect(hwnd,&r);
        W = r.right; H = r.bottom;

        /* X - top right corner, full status bar height */
        if (x >= W - X_W && y < TOP_H) {
            logf_("X: zamykam\n");
            g_running = 0;
            odo_save();
            if (g_log) fclose(g_log);
            DestroyWindow(hwnd);
            return 0;
        }

        /* MENU button - bottom right */
        if (!g_menu_open && x >= W - 58 && y >= H - BOT_H) {
            g_menu_open = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (g_menu_open) {
            int mx = W - 250;
            int my = TOP_H + 4;
            int mw = 240;
            int i;
            int handled = 0;
            /* click outside panel closes menu */
            if (x < mx || x > mx + mw || y < my || y > my + 6 * 34 + 8) {
                g_menu_open = 0;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            for (i = 0; i < 6; i++) {
                int iy = my + 4 + i * 34;
                if (x >= mx + 4 && x <= mx + mw - 4 && y >= iy && y <= iy + 30) {
                    handled = 1;
                    if (i == 0) { /* Theme */
                        g_theme = (g_theme + 1) % THEME_COUNT;
                        theme_save();
                    } else if (i == 1) { /* Font */
                        g_font_pick = (g_font_pick + 1) % FONT_COUNT;
                        font_save();
                        make_fonts();
                    } else if (i == 2) { /* Units */
                        g_imperial = !g_imperial;
                    } else if (i == 3) { /* Language */
                        g_lang = (g_lang + 1) % LANG_COUNT;
                    } else if (i == 4) { /* Clock 24h */
                        g_clock_24h = !g_clock_24h;
                    } else if (i == 5) { /* Close */
                        g_menu_open = 0;
                    }
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;
                }
            }
            if (!handled) {
                g_menu_open = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        return 0;
    }
    case WM_ERASEBKGND:
        /* We paint the whole screen ourselves (double buffer) - there is
         * zamalowywac. Bez tego widac biale/zielone migniecie pod ramka. */
        return 1;
    case WM_TIMER: {
        static int tick = 0;
        static int last_key = -1;
        int key, need = 0;
        odo_tick();
        tick++;
        if ((tick % 20) == 0) batt_read();

        /* --- ODRYSOWYWANIE TYLKO GDY COS SIE ZMIENILO ---
         * Wczesniej InvalidateRect() ladowalo co 500 ms bez wzgledu na
         * zmiany, co dawalo widoczne drganie/migotanie ekranu.
         * Teraz: odswiezamy co tick, jesli zmienila sie jakakolwiek
         * wartosc widoczna na ekranie; inaczej tylko raz na 2 s. */
        {
            /* bar scrolling: advance only when the text overflows */
            static int last_msgn = -1;
            if (g_msg_count != last_msgn) { g_marq = 0; last_msgn = g_msg_count; }
            g_marq += 3;
            if (g_marq > 20000) g_marq = 0;
        }
        /* the clock is part of the key, so the bar repaints when a minute
         * rolls over even if nothing else changed */
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            key = (g_fix << 0) ^ (g_speed_kmh << 1) ^ (g_sats << 11)
                ^ (g_alt_m << 17) ^ (g_course << 28) ^ (g_hdop10 << 9)
                ^ (g_batt_pct << 21) ^ (g_theme << 3) ^ (g_font_pick << 25)
                ^ (g_scanning << 5) ^ (g_probe_result << 7)
                ^ ((int)g_odo_km << 13) ^ (g_speed_max << 19)
                ^ (st.wHour << 7) ^ (st.wMinute << 16);
        }
        if (key != last_key) { need = 1; last_key = key; }
        if ((tick % 4) == 0) need = 1;      /* 2 s - odswiez pasek baterii */
        if ((tick % 120) == 0) {
            int ow=(int)g_odo_km, of=(int)((g_odo_km-ow)*10.0);
            if (of<0)of=0; if(of>9)of=9;
            odo_save();
            logf_("HB 60s: fix=%d sats=%d/%d spd=%d max=%d crs=%d hdop=%d bat=%d ac=%d odo=%d.%d alt=%d altok=%d rx=%d nmea=%d\n",
                  g_fix,g_sats,g_sats_view,g_speed_kmh,g_speed_max,g_course,g_hdop10,
                  g_batt_pct,g_batt_ac,ow,of,g_alt_m,g_alt_ok,
                  (int)g_total_bytes,(int)g_nmea_count);
            logf_("  fixmode=%d hdop=%d sats=%d/%d cksbad=%d msgs=%d\n",
                  g_fixmode, g_hdop10, g_sats, g_sats_view,
                  g_cks_bad_count, g_msg_count);
            if (g_cks_bad_count > 0) {
                logf_("  CKS-BAD ostatni: %s\n",
                      g_cks_bad[(g_cks_bad_count-1) % MSG_N]);
            }
            if (g_msg_count > 0) {
                logf_("  NMEA ostatni: %s\n", msg_get(g_msg_count-1));
            }
        }
        if (need) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        logf_("WM_CLOSE\n");
        g_running = 0;
        odo_save();
        if (g_log) { fprintf(g_log,"=== exit ===\n"); fclose(g_log); g_log=NULL; }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show)
{
    WNDCLASSW wc; MSG msg; HWND hwnd; int W,H;
    (void)hPrev; (void)cmd; (void)show;

    W = GetSystemMetrics(SM_CXSCREEN);
    H = GetSystemMetrics(SM_CYSCREEN);

    log_open();
    logf_("=== start %dx%d ===\n", W, H);

    gps_power_on();

    lang_detect();
    ini_load();                 /* units= / lang= / theme= from hud.ini */
    theme_load();               /* hud_theme.txt wins over ini */
    logf_("motyw=%d units=%s lang=%s\n", g_theme,
          g_imperial ? "imperial" : "metric", LANG_CODES[g_lang]);

    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"LarkHUD";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassW(&wc)) {
        logf_("RegisterClass FAIL\n");
        MessageBoxW(NULL,L"RegisterClass fail",L"LarkHUD",MB_OK);
        return 1;
    }

    hwnd = CreateWindowExW(0,L"LarkHUD",L"Lark HUD",
                           WS_POPUP|WS_VISIBLE,0,0,W,H,
                           NULL,NULL,hInst,NULL);
    if (!hwnd) {
        logf_("CreateWindow FAIL\n");
        MessageBoxW(NULL,L"CreateWindow fail",L"LarkHUD",MB_OK);
        return 1;
    }
    g_hwnd = hwnd;

    odo_load();
    {
        int ow=(int)g_odo_km, of=(int)((g_odo_km-ow)*10.0);
        if(of<0)of=0; if(of>9)of=9;
        logf_("odo=%d.%d km\n",ow,of);
    }

    CreateThread(NULL,0,gps_thread,NULL,0,NULL);
    SetTimer(hwnd,1,500,NULL);

    while (GetMessageW(&msg,NULL,0,0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    logf_("WinMain exit\n");
    g_running = 0;
    if (g_hGps != INVALID_HANDLE_VALUE) CloseHandle(g_hGps);
    odo_save();
    if (g_log) fclose(g_log);
    return 0;
}
