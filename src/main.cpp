// Tides for the LilyGo T5 4.7" e-paper.
//
// Port of the Tides Mac/iPhone app's "Today" view: current height, rising/falling,
// next high and low, today's curve, and a five-day high/low table.
// Data: NOAA CO-OPS high/low predictions (free, no key), heights above MLLW.
// The curve between highs and lows is a half-cosine, the same as the app uses
// for stations that only publish high/low times.
//
// Power: the board deep-sleeps between redraws and only turns Wi-Fi on every
// FETCH_HOURS; tide events and the clock survive sleep in RTC memory.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_adc_cal.h>
#include <time.h>

#include "epd_driver.h"
#include "roboto12.h"
#include "roboto18.h"
#include "roboto32.h"
#include "utilities.h"

#include "config.h"

// ---------------------------------------------------------------- state ----

struct TideEvent {
    time_t when;
    float feet;
    bool high;
};

static const int MAX_EVENTS = 40;
static const int TABLE_DAYS = 5;

RTC_DATA_ATTR static TideEvent events[MAX_EVENTS];
RTC_DATA_ATTR static int eventCount = 0;
RTC_DATA_ATTR static time_t fetchedAt = 0;
RTC_DATA_ATTR static int failures = 0;

static uint8_t *fb = nullptr;

// Station settings: defaults from config.h, replaced by whatever was saved from the setup page.
static String stationId, stationName, timeZone;

static void loadSettings()
{
    Preferences prefs;
    prefs.begin("tides", false);  // read-write so the first boot creates the store
    stationId = prefs.isKey("id") ? prefs.getString("id") : String(STATION_ID);
    stationName = prefs.isKey("name") ? prefs.getString("name") : String(STATION_NAME);
    timeZone = prefs.isKey("tz") ? prefs.getString("tz") : String(TIME_ZONE);
    prefs.end();
    setenv("TZ", timeZone.c_str(), 1);
    tzset();
}

static void saveSettings()
{
    Preferences prefs;
    prefs.begin("tides", false);
    prefs.putString("id", stationId);
    prefs.putString("name", stationName);
    prefs.putString("tz", timeZone);
    prefs.end();
}

static const uint8_t BLACK = 0x00, DARK = 0x60, MID = 0xB8, LIGHT = 0xE4, WHITE = 0xFF;

// ------------------------------------------------------------ utilities ----

static time_t utcToEpoch(int y, int mo, int d, int h, int mi)
{
    // Days from civil (Howard Hinnant's algorithm).
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + h * 3600 + mi * 60;
}

static time_t localMidnight(time_t t, int dayOffset = 0)
{
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday += dayOffset;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static bool clockIsSet() { return time(nullptr) > 1700000000; }

static String heightText(float feet, bool withUnit)
{
    float v = USE_METERS ? feet * 0.3048f : feet;
    if (v > -0.05f && v < 0) v = 0;  // avoid "-0.0"
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f%s", v, !withUnit ? "" : USE_METERS ? " m" : " ft");
    return buf;
}

static String timeText(time_t t, bool compact)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12;
    if (h == 0) h = 12;
    char buf[16];
    if (compact)
        snprintf(buf, sizeof buf, "%d:%02d%c", h, tm.tm_min, tm.tm_hour < 12 ? 'a' : 'p');
    else
        snprintf(buf, sizeof buf, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    return buf;
}

static String countdownText(time_t from, time_t to)
{
    long mins = (to - from + 30) / 60;
    char buf[20];
    if (mins < 60)
        snprintf(buf, sizeof buf, "in %ldm", mins);
    else
        snprintf(buf, sizeof buf, "in %ldh %02ldm", mins / 60, mins % 60);
    return buf;
}

/// Height at `t`, half-cosine between the surrounding high and low. NAN outside the data.
static float heightAt(time_t t)
{
    for (int i = 0; i + 1 < eventCount; i++) {
        const TideEvent &a = events[i], &b = events[i + 1];
        if (t >= a.when && t <= b.when) {
            float f = (float)(t - a.when) / (float)(b.when - a.when);
            return a.feet + (b.feet - a.feet) * (1 - cosf(PI * f)) / 2;
        }
    }
    return NAN;
}

static const TideEvent *nextEvent(time_t after, bool high)
{
    for (int i = 0; i < eventCount; i++)
        if (events[i].when > after && events[i].high == high) return &events[i];
    return nullptr;
}

// -------------------------------------------------------------- drawing ----

static int textWidth(const GFXfont *font, const String &s)
{
    int32_t x = 0, y = 0, x1, y1, w, h;
    get_text_bounds(font, s.c_str(), &x, &y, &x1, &y1, &w, &h, nullptr);
    return w;
}

enum Align { LEFT, CENTER, RIGHT };

/// Draws `s` with its baseline at `y`.
static void text(const GFXfont *font, const String &s, int x, int y, Align align = LEFT)
{
    if (s.isEmpty()) return;
    if (align != LEFT) {
        int w = textWidth(font, s);
        x -= align == CENTER ? w / 2 : w;
    }
    int32_t cx = x, cy = y;
    writeln(font, s.c_str(), &cx, &cy, fb);
}

static void arrow(int x, int y, int size, bool up, uint8_t color)
{
    if (up)
        epd_fill_triangle(x, y + size, x + size, y + size, x + size / 2, y, color, fb);
    else
        epd_fill_triangle(x, y, x + size, y, x + size / 2, y + size, color, fb);
}

static float readBatteryVolts()
{
    esp_adc_cal_characteristics_t chars;
    uint32_t vref = 1100;
    if (esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &chars) ==
        ESP_ADC_CAL_VAL_EFUSE_VREF)
        vref = chars.vref;
    // The battery divider is only powered while the display rail is on.
    epd_poweron();
    delay(20);
    uint16_t raw = analogRead(BATT_PIN);
    epd_poweroff();
    return raw / 4095.0f * 2.0f * 3.3f * (vref / 1000.0f);
}

static String batteryText(float volts)
{
    if (volts > 4.35f) return "USB";
    int pct = constrain((int)((volts - 3.3f) / (4.2f - 3.3f) * 100), 0, 100);
    return String(pct) + "%";
}

static void drawHeader(time_t now, float volts)
{
    text(&Roboto18, stationName, 24, 46);

    struct tm tm;
    localtime_r(&now, &tm);
    char date[32];
    strftime(date, sizeof date, "%a %b %e", &tm);
    String right = String(date) + "   " + timeText(now, false) + "   " + batteryText(volts);
    text(&Roboto12, right, EPD_WIDTH - 24, 40, RIGHT);
    epd_draw_hline(24, 64, EPD_WIDTH - 48, DARK, fb);
}

static void drawNowPanel(time_t now)
{
    const int x = 24, right = 330;
    float h = heightAt(now), ahead = heightAt(now + 600);
    text(&Roboto12, "Now", x, 98);
    if (isnan(h)) {
        text(&Roboto32, "--", x, 164);
    } else {
        text(&Roboto32, heightText(h, true), x, 164);
        bool rising = !isnan(ahead) && ahead > h;
        arrow(x, 184, 22, rising, BLACK);
        text(&Roboto18, rising ? "Rising" : "Falling", x + 34, 208);
    }

    // Next high and next low, soonest first.
    const TideEvent *next[2] = {nextEvent(now, true), nextEvent(now, false)};
    if (next[0] && next[1] && next[1]->when < next[0]->when) std::swap(next[0], next[1]);
    int y = 250;
    for (const TideEvent *e : next) {
        if (!e) continue;
        text(&Roboto12, e->high ? "Next high" : "Next low", x, y);
        text(&Roboto12, countdownText(now, e->when), right, y, RIGHT);
        text(&Roboto18, timeText(e->when, false), x, y + 36);
        text(&Roboto12, heightText(e->feet, true), right, y + 34, RIGHT);
        y += 66;
    }
}

static void drawChart(time_t now)
{
    const int x0 = 380, x1 = EPD_WIDTH - 24, yTop = 100, yBot = 300;
    const time_t start = localMidnight(now), end = localMidnight(now, 1);
    const float span = (float)(end - start);

    // Vertical range from today's curve, padded so labels fit above and below.
    float lo = 1e9, hi = -1e9;
    for (time_t t = start; t <= end; t += 600) {
        float h = heightAt(t);
        if (isnan(h)) continue;
        lo = min(lo, h);
        hi = max(hi, h);
    }
    if (lo > hi) return;
    lo = min(lo, 0.0f);
    float range = max(hi - lo, 1.0f);
    lo -= range * 0.08f;
    hi += range * 0.20f;

    auto xOf = [&](time_t t) { return x0 + (int)((t - start) / span * (x1 - x0)); };
    auto yOf = [&](float h) { return yBot - (int)((h - lo) / (hi - lo) * (yBot - yTop)); };

    // Filled curve: darker for the part of the day already past.
    int prevY = -1;
    for (int x = x0; x <= x1; x++) {
        time_t t = start + (time_t)((float)(x - x0) / (x1 - x0) * span);
        float h = heightAt(t);
        if (isnan(h)) {
            prevY = -1;
            continue;
        }
        int y = yOf(h);
        epd_draw_vline(x, y, yBot - y, t <= now ? MID : LIGHT, fb);
        if (prevY >= 0)
            for (int k = -1; k <= 1; k++) epd_draw_line(x - 1, prevY + k, x, y + k, BLACK, fb);
        prevY = y;
    }

    // Zero (MLLW) line and time axis.
    int yZero = yOf(0);
    for (int x = x0; x < x1; x += 8) epd_draw_hline(x, yZero, 4, DARK, fb);
    epd_draw_hline(x0, yBot, x1 - x0, BLACK, fb);
    static const char *labels[] = {"12a", "6a", "12p", "6p", "12a"};
    for (int i = 0; i <= 4; i++) {
        int x = x0 + (x1 - x0) * i / 4;
        epd_draw_vline(x, yBot, 8, BLACK, fb);
        text(&Roboto12, labels[i], x, yBot + 36, i == 0 ? LEFT : i == 4 ? RIGHT : CENTER);
    }

    // Highs and lows.
    for (int i = 0; i < eventCount; i++) {
        const TideEvent &e = events[i];
        if (e.when < start || e.when > end) continue;
        int x = xOf(e.when), y = yOf(e.feet);
        epd_fill_circle(x, y, 6, BLACK, fb);
        epd_fill_circle(x, y, 3, WHITE, fb);
        String label = heightText(e.feet, false);
        int w = textWidth(&Roboto12, label);
        int lx = constrain(x - w / 2, x0, x1 - w);
        text(&Roboto12, label, lx, y - 14);
    }

    // Now marker.
    if (now >= start && now <= end) {
        int x = xOf(now);
        float h = heightAt(now);
        epd_draw_vline(x, yTop - 14, yBot - yTop + 14, BLACK, fb);
        epd_draw_vline(x + 1, yTop - 14, yBot - yTop + 14, BLACK, fb);
        if (!isnan(h)) epd_fill_circle(x, yOf(h), 8, BLACK, fb);
    }
}

static void drawTable(time_t now)
{
    const int top = 366, colW = (EPD_WIDTH - 48) / TABLE_DAYS;
    epd_draw_hline(24, top, EPD_WIDTH - 48, DARK, fb);
    for (int d = 0; d < TABLE_DAYS; d++) {
        time_t start = localMidnight(now, d), end = localMidnight(now, d + 1);
        int x = 24 + d * colW;
        if (d > 0) epd_draw_vline(x - 10, top + 10, EPD_HEIGHT - top - 20, LIGHT, fb);

        struct tm tm;
        localtime_r(&start, &tm);
        char day[16];
        strftime(day, sizeof day, "%a %e", &tm);
        text(&Roboto12, d == 0 ? String("Today") : String(day), x, top + 34);

        int y = top + 66;
        for (int i = 0; i < eventCount && y < EPD_HEIGHT; i++) {
            const TideEvent &e = events[i];
            if (e.when < start || e.when >= end) continue;
            arrow(x, y - 16, 14, e.high, e.high ? BLACK : DARK);
            text(&Roboto12, timeText(e.when, true), x + 22, y);
            text(&Roboto12, heightText(e.feet, false), x + colW - 24, y, RIGHT);
            y += 30;
        }
    }
}

static void clearBuffer() { memset(fb, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2); }

static void show()
{
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), fb);
    epd_poweroff();
}

static void showMessage(const String &title, const String &line1, const String &line2 = "",
                        const String &line3 = "")
{
    clearBuffer();
    text(&Roboto32, "Tides", 60, 130);
    text(&Roboto18, title, 60, 230);
    text(&Roboto12, line1, 60, 300);
    text(&Roboto12, line2, 60, 344);
    text(&Roboto12, line3, 60, 388);
    show();
}

static void drawTides(float volts)
{
    time_t now = time(nullptr);
    clearBuffer();
    drawHeader(now, volts);
    drawNowPanel(now);
    drawChart(now);
    drawTable(now);
    show();
}

// -------------------------------------------------------------- network ----

static bool connectWiFi()
{
    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConnectTimeout(20);
    wm.setConfigPortalTimeout(300);
    wm.setAPCallback([](WiFiManager *) {
        Serial.println("Wi-Fi setup portal open");
        showMessage("Wi-Fi setup",
                    String("1. On your phone, join the Wi-Fi network \"") + SETUP_AP_NAME + "\".",
                    "2. Pick your home network (2.4 GHz) and enter its password.",
                    "This screen closes by itself after 5 minutes.");
    });
    bool ok = wm.autoConnect(SETUP_AP_NAME);
    Serial.println(ok ? "Wi-Fi connected" : "Wi-Fi not connected");
    return ok;
}

/// Opens the setup network with a page for the station number. Returns the number
/// entered, or "" if nothing was saved within 5 minutes.
static String askForStation()
{
    showMessage("Change tide station",
                String("1. On your phone, join the Wi-Fi network \"") + SETUP_AP_NAME + "\".",
                "2. Tap Setup, enter the NOAA station number, and save.",
                "Now: " + stationName + " (" + stationId + "). Closes after 5 minutes.");

    static bool saved;
    saved = false;
    WiFiManager wm;
    wm.setDebugOutput(false);
    WiFiManagerParameter field("station", "NOAA tide station number", stationId.c_str(), 8);
    wm.addParameter(&field);
    wm.setParamsPage(true);
    std::vector<const char *> menu = {"param", "wifi", "exit"};
    wm.setMenu(menu);
    wm.setSaveParamsCallback([]() { saved = true; });
    wm.setConfigPortalBlocking(false);
    wm.setConfigPortalTimeout(300);
    wm.startConfigPortal(SETUP_AP_NAME);
    Serial.println("Station setup portal open");

    uint32_t started = millis();
    while (!saved && millis() - started < 300000UL && wm.getConfigPortalActive()) {
        wm.process();
        delay(5);
    }
    for (int i = 0; i < 200; i++) {  // let the "saved" page reach the phone
        wm.process();
        delay(5);
    }
    String value = field.getValue();
    value.trim();
    wm.stopConfigPortal();
    return saved ? value : String();
}

/// "TRINIDAD HARBOR" -> "Trinidad Harbor"
static String titleCase(String s)
{
    bool start = true;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        s[i] = start ? toupper(c) : tolower(c);
        start = !isalpha(c) && c != '\'';
    }
    return s;
}

/// Looks the station up at NOAA and, if it is a tide station, switches to it.
/// Keeps the current station and says why on screen if it is not.
static void applyStation(const String &wanted)
{
    if (wanted.isEmpty() || wanted == stationId) return;
    String problem;
    JsonDocument doc;
    bool digits = wanted.length() >= 5;
    for (char c : wanted) digits = digits && isdigit(c);
    if (!digits) {
        problem = "\"" + wanted + "\" is not a station number.";
    } else {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.setTimeout(15000);
        http.begin(client, "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/" + wanted + ".json");
        int code = http.GET();
        if (code != HTTP_CODE_OK)
            problem = code == 404 ? "NOAA has no station " + wanted + "." : "NOAA did not answer (" + String(code) + ").";
        else if (deserializeJson(doc, http.getString()) || doc["stations"][0]["id"].isNull())
            problem = "NOAA has no station " + wanted + ".";
        else if (!(doc["stations"][0]["tidal"] | true))
            problem = "Station " + wanted + " has no tide predictions.";
        http.end();
    }
    if (!problem.isEmpty()) {
        Serial.println(problem);
        showMessage("Station not changed", problem, "Still showing " + stationName + ".");
        delay(8000);
        return;
    }

    JsonObject st = doc["stations"][0];
    String state = st["state"] | "";
    stationName = titleCase(st["name"] | wanted.c_str());
    if (!state.isEmpty()) stationName += ", " + state;
    stationId = wanted;

    // POSIX zone from NOAA's standard-time offset, with the US daylight-saving rule if observed.
    int corr = st["timezonecorr"] | 0;
    char tz[48];
    if (st["observedst"] | false)
        snprintf(tz, sizeof tz, "<%+03d>%d<%+03d>,M3.2.0,M11.1.0", corr, -corr, corr + 1);
    else
        snprintf(tz, sizeof tz, "<%+03d>%d", corr, -corr);
    timeZone = tz;
    setenv("TZ", tz, 1);
    tzset();
    saveSettings();
    eventCount = 0;  // stored tides belong to the old station
    Serial.printf("Station set to %s (%s), zone %s\n", stationName.c_str(), stationId.c_str(), tz);
}

static bool syncClock()
{
    configTzTime(timeZone.c_str(), "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 100 && !clockIsSet(); i++) delay(100);
    if (clockIsSet()) {
        Serial.println("Clock set (NTP)");
        return true;
    }

    // NTP is sometimes blocked or slow; fall back to the Date header of an HTTPS reply.
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    const char *wanted[] = {"Date"};
    http.collectHeaders(wanted, 1);
    if (http.begin(client, "https://api.tidesandcurrents.noaa.gov/")) {
        http.sendRequest("HEAD");
        String date = http.header("Date");  // e.g. "Tue, 24 Nov 2026 03:40:00 GMT"
        http.end();
        static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        char mon[4] = "";
        int d, y, h, mi, s;
        if (sscanf(date.c_str(), "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) == 6) {
            const char *m = strstr(months, mon);
            if (m) {
                timeval tv = {utcToEpoch(y, (m - months) / 3 + 1, d, h, mi) + s, 0};
                settimeofday(&tv, nullptr);
            }
        }
    }
    Serial.println(clockIsSet() ? "Clock set (HTTP)" : "Clock not set");
    return clockIsSet();
}

static bool fetchTides()
{
    // Start 12 h before local midnight so the curve is defined at the start of today.
    time_t begin = localMidnight(time(nullptr)) - 12 * 3600;
    struct tm g;
    gmtime_r(&begin, &g);
    char url[320];
    snprintf(url, sizeof url,
             "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions"
             "&application=TidesT5&begin_date=%04d%02d%02d%%20%02d:%02d&range=%d&datum=MLLW"
             "&station=%s&time_zone=gmt&units=english&interval=hilo&format=json",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min,
             (TABLE_DAYS + 1) * 24 + 24, stationId.c_str());

    // Public, read-only tide tables: the server certificate is not verified.
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("NOAA HTTP %d\n", code);
        http.end();
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err) {
        Serial.printf("NOAA JSON: %s\n", err.c_str());
        return false;
    }
    JsonArray rows = doc["predictions"];
    if (rows.isNull() || rows.size() < 2) {
        Serial.printf("NOAA: %s\n", doc["error"]["message"] | "no predictions");
        return false;
    }

    int n = 0;
    for (JsonObject row : rows) {
        if (n >= MAX_EVENTS) break;
        int y, mo, d, h, mi;
        if (sscanf(row["t"] | "", "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) continue;
        events[n].when = utcToEpoch(y, mo, d, h, mi);
        events[n].feet = atof(row["v"] | "0");
        events[n].high = (row["type"] | "L")[0] == 'H';
        n++;
    }
    if (n < 2) return false;
    eventCount = n;
    fetchedAt = time(nullptr);
    Serial.printf("NOAA: %d tide events\n", n);
    return true;
}

static bool dataIsFresh()
{
    if (!clockIsSet() || eventCount < 2) return false;
    time_t now = time(nullptr);
    if (now - fetchedAt > FETCH_HOURS * 3600L) return false;
    // Still reaches into the last day of the table?
    return events[eventCount - 1].when >= localMidnight(now, TABLE_DAYS - 1);
}

// ---------------------------------------------------------------- sleep ----

static void sleepFor(uint32_t minutes)
{
    Serial.printf("Sleeping %u min\n", minutes);
    Serial.flush();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    epd_poweroff_all();
    esp_sleep_enable_timer_wakeup((uint64_t)minutes * 60ULL * 1000000ULL);
    // GPIO 39 button: refresh now. GPIO 34 button: change the tide station.
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_3, 0);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_1, ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
}

/// Minutes until the next REDRAW_MINUTES boundary, so redraws land on :00 and :30.
static uint32_t minutesToNextRedraw()
{
    time_t now = time(nullptr);
    long period = REDRAW_MINUTES * 60L;
    long wait = period - (now % period);
    if (wait < 90) wait += period;
    return (wait + 59) / 60;
}

// ----------------------------------------------------------------- main ----

void setup()
{
    Serial.begin(115200);
    Serial.println("\nTides for T5 4.7");
    loadSettings();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool timerWake = cause == ESP_SLEEP_WAKEUP_TIMER;

    epd_init();
    fb = (uint8_t *)ps_calloc(1, EPD_WIDTH * EPD_HEIGHT / 2);
    if (!fb) {
        Serial.println("No PSRAM for the framebuffer");
        sleepFor(60);
    }
    float volts = readBatteryVolts();
    Serial.printf("Battery %.2f V\n", volts);

    // The station button opens the setup page before anything else.
    String wanted;
    if (cause == ESP_SLEEP_WAKEUP_EXT1) wanted = askForStation();

    // Power-on and button presses always refresh over Wi-Fi; timer wakes only when data is stale.
    if (!timerWake || !dataIsFresh()) {
        if (connectWiFi() && (applyStation(wanted), syncClock()) && fetchTides()) {
            failures = 0;
        } else {
            failures++;
            // Ride out brief outages on stored data while it still covers the present.
            bool usable = clockIsSet() && eventCount >= 2 && !isnan(heightAt(time(nullptr)));
            if (!usable || failures >= 4) {
                showMessage("Can't get tide data",
                            WiFi.isConnected() ? "Connected to Wi-Fi, but NOAA did not answer."
                                               : "Not connected to Wi-Fi.",
                            "Trying again in 10 minutes.",
                            "Press the button to retry now.");
                sleepFor(10);
            }
        }
    }

    drawTides(volts);
    sleepFor(minutesToNextRedraw());
}

void loop() {}
