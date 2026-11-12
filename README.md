# Lark HUD — speedometer for Lark FreeBird 35.7 / 42.1

A Windows CE 5.0 (ARM) application showing speed, odometer, altitude, compass,
max speed, HDOP signal bars and battery level.

* **Themes:** VERSO (red Toyota Verso 2017 cluster look, default), CZERWONY, AMBER, NIEBIESKI
* **Units:** metric (km/h, km, m) or imperial (mph, mi, ft) — from `hud.ini`
* **Languages:** PL, EN, DE, ES, FR, IT, RU, NL, PT, CZ — auto-detected or from `hud.ini`
* **Fonts:** Minecraft.ttf (if present), Terminal, Fixedsys, MS Sans Serif, Tahoma, Courier New, Arial
* **GPS:** automatic COM0–9 scan at 4800 baud, NMEA 0183, no TX (safe for the receiver)

---

## 1. Put the project on GitHub

1. Create a free account at github.com.
2. Click **New repository**, name it e.g. `lark-hud`, choose **Public**, click **Create**.
3. Click **uploading an existing file** and drag in **all** files:
   - `hud.c`
   - `hud.ini`
   - `hud.rc`
   - `hud.ico` (optional — if missing, delete that line from `hud.rc`)
   - the `.github/workflows/build.yml` folder
4. Click **Commit changes**.

## 2. Wait for the build

1. Open the **Actions** tab.
2. You will see the `build-wince` workflow running (yellow dot = in progress).
3. After 2–4 minutes it turns into a green **check**.

**Note:** the `.github` folder is hidden. If you drag files in through the
browser, create it by hand: **Add file → Create new file**, type the path
`.github/workflows/build.yml` and paste the contents.

## 3. Download hud.exe

1. In the **Actions** tab click the newest green run.
2. At the bottom, in the **Artifacts** section, click `hud-exe`.
3. Unzip the archive — `hud.exe` is inside.

## 4. Copy to the navigation device

1. Take the SD card out of the Lark 35.7 and put it into a card reader.
2. Copy `hud.exe` to the **root** folder of the card.
3. Put the card back into the device.
4. Start `hud.exe` from the device menu or from a file explorer.

---

## Configuration — `hud.ini`

Put `hud.ini` in the **root** of the SD card (or in `\Windows\`). Everything is
optional; delete the file to return to the defaults.

```ini
; metric (default) or imperial
units = imperial

; pl | en | de | es | fr | it | ru | nl | pt | cz
; omit the line to follow the device locale
lang = en

; 0 = VERSO (default), 1 = CZERWONY, 2 = AMBER, 3 = NIEBIESKI
theme = 0
```

Accepted spellings: `units = metric | imperial` (or `km` / `mi`).
Unknown keys and lines starting with `;` or `#` are ignored.

### Units

| Internal | Metric display | Imperial display |
|---|---|---|
| speed | km/h | mph |
| odometer | km | mi |
| altitude | m | ft |
| max speed | km/h | mph |

Everything is stored internally in metric units, so switching back and forth
never corrupts the odometer.

### Languages

The language is picked from the device locale at startup. Add `lang=xx` to
`hud.ini` to override it. Russian and Czech are transliterated to ASCII where
the WinCE core font has no glyphs, so no text is lost.

---

## Themes

| # | Name | Look |
|---|---|---|
| 0 | DOTMATRIX | Toyota Verso center dot-matrix MFD |
| 1 | EL_GLOW | Citroen BX Digit full-width VFD |
| 2 | SUBARU_XT | 1985 Subaru XT Turbo digital cluster |
| 3 | CZERWONY | classic dark red |

The **THEME** button cycles them and writes the choice to `hud_theme.txt`,
which wins over `theme=` in `hud.ini`.

---

## Fonts

Drop `Minecraft.ttf` in the card root, in `\SDMMC\` or in `\Windows\`. The
program registers it at startup with `AddFontResourceW` (no restart needed).
The **FONT** button cycles the available fonts.

---

## Files the program writes

| File | Contents |
|---|---|
| `hud_odo.txt` | odometer (km, always metric) |
| `hud_theme.txt` | theme index |
| `hud_font.txt` | font index |
| `hud_log.txt` | log: port scan, NMEA, heartbeat every 60 s, battery |

All of them are written to the card root, falling back to `\SDMMC\`.

---

## Troubleshooting

| Symptom | What to check |
|---|---|
| Bar "scanning GPS ports…" forever | the GPS port is busy — close other navigation software |
| `NO GPS - retry 5 s` | receiver not answering — check the card is not read-only |
| No fix but satellites counted | wait for a cold start (up to 5 min outdoors) |
| Odometer does not save | the SD card must be writable |
| Wrong language | add `lang=xx` to `hud.ini` |
| Wrong units | add `units=imperial` (or `metric`) to `hud.ini` |
| Minecraft font not listed | the TTF is not in the card root / `\SDMMC\` / `\Windows\` — see `hud_log.txt` |

The log file `hud_log.txt` is the fastest way to see what happened:

```
=== start 480x272 ===
jezyk z systemu: pl
ini: units=imperial
motyw=0 units=imperial lang=en
Minecraft.ttf zaladowany: \Storage Card\Minecraft.ttf
```

---

## Building without GitHub

```sh
arm-mingw32ce-gcc -O2 -Wall -DUNICODE -D_UNICODE -o hud.exe hud.c \
    -lcoredll -Wl,-subsystem,windows
```

Requires CeGCC 9.3.0 (arm-mingw32ce).
