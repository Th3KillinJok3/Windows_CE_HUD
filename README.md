# Lark HUD — prędkościomierz dla Lark FreeBird 35.7

Aplikacja Windows CE 5.0 (ARM) pokazująca prędkość, licznik kilometrów i wysokość.

## 1. Wrzuć projekt na GitHub

1. Załóż konto na github.com (darmowe).
2. Kliknij **New repository**, nazwij go np. `lark-hud`, wybierz **Public**, kliknij **Create**.
3. Kliknij **uploading an existing file** i przeciągnij **wszystkie** pliki z tego ZIP-a:
   - `hud.c`
   - `hud.rc`
   - `hud.ico` (opcjonalnie — jeśli go nie ma, usuń linię z `hud.rc`)
   - folder `.github/workflows/build.yml`
4. Kliknij **Commit changes**.

## 2. Poczekaj na kompilację

1. Wejdź w zakładkę **Actions**.
2. Zobaczysz uruchomiony workflow `build-wince` (żółta kropka = trwa).
3. Po 2–4 minutach zmieni się na zielony **check**.

**Uwaga:** folder `.github` jest ukryty — jeśli przeciągasz pliki przez przeglądarkę,
utwórz go ręcznie: **Add file → Create new file**, wpisz ścieżkę
`.github/workflows/build.yml` i wklej zawartość.

## 3. Pobierz hud.exe

1. W zakładce **Actions** kliknij najnowszy zielony run.
2. Na dole sekcji **Artifacts** kliknij `hud-exe`.
3. Rozpakuj ZIP — w środku jest `hud.exe`.

## 4. Wgraj na nawigację

1. Wyjmij kartę SD z Lark 35.7 i włóż do komputera (przez czytnik).
2. Skopiuj `hud.exe` do głównego katalogu karty.
3. Włóż kartę z powrotem do nawigacji.
4. W menu nawigacji (lub w pliku konfiguracji ścieżki programu) wskaż `Storage Cardhud.exe`
   jako program do uruchomienia — albo uruchom go przez eksplorator plików, jeśli masz odblokowane menu.

## Konfiguracja

Na górze `hud.c`:

```c
#define GPS_PORT  "COM2:"      /* najczęściej COM2 */
#define GPS_BAUD  CBR_4800     /* jeśli cisza — spróbuj CBR_9600 */
```

Po zmianie: commit → GitHub przebuduje → pobierz nowy `hud.exe`.

## Plik licznika

Licznik zapisuje się w `Storage Cardhud_odo.txt` i wczytuje po starcie.
Przycisk **RESET** (prawy dolny róg) zeruje go.

## Rozwiązywanie problemów

| Objaw | Co sprawdzić |
|---|---|
| Czerwony pasek „szukam satelitow” | port COM — zmień `GPS_PORT` na `COM1:` / `COM3:` |
| Dalej cisza | zmień `GPS_BAUD` na `CBR_9600` |
| Nie uruchamia się | upewnij się, że plik jest w `Storage Card`, nie w podfolderze |
| Licznik się nie zapisuje | karta SD musi być odblokowana (nie read-only) |
