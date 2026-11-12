#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
One-shot patcher for hud.c:
  1. new default theme 0 = "VERSO" (Toyota Verso 2017 orange overlay, but red)
  2. i18n table (PL/EN/DE/ES/FR/IT/RU/NL/PT/CZ) with auto-detect + ini override
  3. ini file hud.ini: units=metric|imperial, lang=pl|en|..., theme=
  4. all comments translated to English
Everything is plain C89-compatible (CeGCC gcc), no new imports.
"""
import io, re, sys

P = sys.argv[1] if len(sys.argv) > 1 else "hud.c"
s = io.open(P, encoding="utf-8").read()

def rep(old, new, count=1, label=""):
    global s
    if old not in s:
        print("!! NOT FOUND [%s]: %r" % (label, old[:120]))
        sys.exit(1)
    s = s.replace(old, new, count)
    print("ok  %s" % label)

# ---------------------------------------------------------------- 1. header
rep("""/*
 * hud.c — predkosciomierz + licznik km + wysokosc
 * Windows CE 5.0 / ARM (Lark FreeBird 35.7, 320x240)
 * Wersja MAX: GPD0 power-on, SiRF+MTK wakeup, binarny SiRF,
 * skan COM0-9, log co minute, X, 3 motywy, pixlowaty font.
 *
 * Kompilacja (CeGCC 9.3.0):
 *   arm-mingw32ce-gcc -O2 -Wall -DUNICODE -D_UNICODE -o hud.exe hud.c \\
 *       -lcoredll -Wl,-subsystem,windows
 */""",
"""/*
 * hud.c - speedometer + odometer + altitude HUD
 * Windows CE 5.0 / ARM (Lark FreeBird 35.7 - 480x272, 42.1 - 480x272)
 *
 * Features: GPD0 power-on, SiRF+MTK wakeup, binary SiRF, COM0-9 scan,
 * heartbeat log every 60 s, X close button, 4 themes, pixel fonts,
 * metric/imperial units and 10 UI languages (see hud.ini).
 *
 * Build (CeGCC 9.3.0):
 *   arm-mingw32ce-gcc -O2 -Wall -DUNICODE -D_UNICODE -o hud.exe hud.c \\
 *       -lcoredll -Wl,-subsystem,windows
 */""", label="header")

# ---------------------------------------------------------------- 2. config defines
rep("""#define FONT_FILE_A   "\\\\Storage Card\\\\hud_font.txt"
#define FONT_FILE_B   "\\\\SDMMC\\\\hud_font.txt\"""",
"""#define FONT_FILE_A   "\\\\Storage Card\\\\hud_font.txt"
#define FONT_FILE_B   "\\\\SDMMC\\\\hud_font.txt"
#define INI_FILE_A    "\\\\Storage Card\\\\hud.ini"
#define INI_FILE_B    "\\\\SDMMC\\\\hud.ini\"""",
label="ini paths")

# ---------------------------------------------------------------- 3. themes: 4th theme + names
rep("""static const Theme THEMES[3] = {""",
"""/* Theme 0 - VERSO: red overlay taken from the Toyota Verso 2017
 * instrument cluster (orange glow) recoloured to red. Dark, high
 * contrast, soft red halo around the digits. */
static const Theme THEMES[4] = {
    { RGB(8,4,6), RGB(30,10,14), RGB(96,18,26), RGB(255,86,72),
      RGB(150,60,58), RGB(255,34,34), RGB(120,12,16),
      RGB(255,150,40), RGB(70,36,6), RGB(255,80,60), RGB(80,10,10),
      RGB(46,12,16), RGB(140,28,34) },""",
label="theme table head")

rep("""static const WCHAR *THEME_NAMES[3] = { L"CZERWONY", L"AMBER", L"NIEBIESKI" };""",
"""#define THEME_COUNT 4
static const WCHAR *THEME_NAMES[THEME_COUNT] = {
    L"VERSO", L"CZERWONY", L"AMBER", L"NIEBIESKI"
};""",
label="theme names")

rep("""static int g_theme = 0;""",
"""static int g_theme = 0;          /* 0 = VERSO (default) */""",
label="g_theme")

# ---------------------------------------------------------------- 4. i18n block
I18N = r'''
/* ============ UNITS + LANGUAGES ============ */
/* Everything is stored internally in metric (km/h, km, m).
 * Imperial is only a display conversion: mph, miles, feet. */
static int g_imperial = 0;   /* 0 = metric, 1 = imperial (hud.ini: units=imperial) */

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
 /* RU */ { L"ODOMETR", L"VYSOTA", L"max", L"SBROS", L"TEMA", L"SHRI FT",
            L"poisk porta GPS...", L"NET GPS - 5 s", L"zhdu fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* NL */ { L"KILOMETER", L"HOOGTE", L"max", L"RESET", L"THEMA", L"LETTER",
            L"GPS-poort zoeken...", L"GEEN GPS - 5 s", L"wacht op fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* PT */ { L"ODOMETRO", L"ALTITUDE", L"max", L"RESET", L"TEMA", L"FONTE",
            L"procurando porta GPS...", L"SEM GPS - 5 s", L"aguardando fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
 /* CZ */ { L"POCITADLO", L"VYSKA", L"max", L"RESET", L"VZHLED", L"FONT",
            L"hledam port GPS...", L"ZADNE GPS - 5 s", L"cekám na fix",
            L"sat", L"km/h", L"mph", L"m", L"ft", L"km", L"mi" },
};

static int g_lang = LANG_PL;

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
    } else if (_stricmp(key, "theme") == 0) {
        int t = atoi(val);
        if (t >= 0 && t < THEME_COUNT) { g_theme = t; logf_("ini: theme=%d\n", t); }
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
    default:   g_lang = LANG_EN; break;
    }
    logf_("jezyk z systemu: %s\n", LANG_CODES[g_lang]);
}

/* ============ UNIT CONVERSION ============ */
#define KMH_TO_MPH  0.6213712
#define M_TO_FT     3.2808399
#define KM_TO_MI    0.6213712

/* Speed for display, already in the active unit. */
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
/* Odometer in the active unit, whole part + one decimal. */
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
'''

rep("""/* ============ LOG (bez bufora) ============ */""",
    I18N + """
/* ============ LOG (no buffering) ============ */""",
    label="i18n + units block")

io.open(P, "w", encoding="utf-8").write(s)
print("\n-- part 1 written --")
