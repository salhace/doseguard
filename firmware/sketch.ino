/*
  DoseGuard — smart cap for insulin pens
  ======================================

  The most common question people with diabetes ask themselves is
  "Did I already take my insulin?". Injecting twice can cause severe
  hypoglycaemia; skipping a dose causes high blood sugar. Insulin is also
  fragile: frozen or overheated insulin silently loses its effect, and an
  opened pen is only good for about 28 days.

  DoseGuard replaces the pen's normal cap and answers all of this:

    * Dose detection     Taking the cap off for at least 6 s and putting it
                         back is logged as a dose (shorter = just a peek).
    * Double-dose guard  The moment the cap comes off, DoseGuard checks the
                         last dose. If it was less than 3 h ago, it alarms
                         BEFORE the injection happens.
    * Missed dose        Reminds the user if the daily dose (21:00) has not
                         been taken 60 min after it was due.
    * Insulin health     NTC thermistor tracks the pen's temperature:
                         freezing = do not use, above 30 °C = heat exposure
                         (minutes accumulated), and a 28-day in-use countdown.
    * Cap left off       Reminds the user to put the cap back.
    * Memory             Dose log, pen age and heat exposure are stored in
                         flash (NVS) and survive power loss.
    * Companion app      Everything is published over MQTT to a web app.

  Controls: short press = silence / acknowledge,
            long press (3 s) = new pen (resets 28-day timer and heat log).

  Author:  Salha Alwahbi
  License: MIT
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <math.h>

// ============================== Configuration ==============================

// DEMO_MODE 1 runs the clock 60x faster (1 real second = 1 minute) so a whole
// day can be demonstrated in the simulator in 24 minutes. 0 = real time.
#define DEMO_MODE 1
#if DEMO_MODE
const uint32_t TIME_SCALE = 60;
#else
const uint32_t TIME_SCALE = 1;
#endif

const int      DOSE_HOUR          = 21;   // daily dose due at 21:00 (e.g. basal insulin)
const int      DOSE_MINUTE        = 0;
const uint32_t EARLY_WINDOW_MIN   = 180;  // a dose up to 3 h before due counts for that day
const uint32_t GRACE_MIN          = 60;   // reminder starts 60 min after due
const uint32_t MISSED_AFTER_MIN   = 720;  // after 12 h the dose is logged as missed
const uint32_t GUARD_WINDOW_MIN   = 180;  // warn if cap opened within 3 h of last dose
const uint32_t CAP_OFF_REMIND_MIN = 10;   // remind to put the cap back
const uint32_t MIN_INJECTION_MS   = 6000; // real time the cap must be off to count as a dose

const float    HEAT_LIMIT_C       = 30.0; // in-use insulin: keep below 30 °C
const float    FREEZE_LIMIT_C     = 0.0;  // frozen insulin must be discarded
const uint32_t HEAT_BUDGET_MIN    = 120;  // warn after this many minutes above the limit
const uint32_t PEN_LIFE_DAYS      = 28;   // typical in-use life of an opened pen

const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";
const char* MQTT_HOST   = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* DEVICE_ID   = "pen-01";
const char* TOPIC_ROOT  = "doseguard";      // doseguard/pen-01/{telemetry,events,log}
const char* TIMEZONE    = "<+03>-3";        // Riyadh, UTC+3

// Pins
const int PIN_CAP    = 14;  // cap switch to GND: LOW = cap on
const int PIN_BUTTON = 13;  // push button to GND
const int PIN_NTC    = 34;  // NTC thermistor divider (ADC)
const int PIN_BUZZER = 25;
const int PIN_LED_R  = 26;
const int PIN_LED_G  = 27;
const int PIN_LED_B  = 33;

// ============================== Types & globals ==============================

enum DoseFlag : uint8_t { F_GUARD = 1, F_LATE = 2 };
struct Dose { uint32_t t; uint16_t holdS; uint8_t flags; uint8_t pad; };

const int MAX_DOSES  = 30;
const int MAX_MISSED = 10;

struct Store {
  Dose doses[MAX_DOSES];
  uint8_t doseCount, doseHead;
  uint32_t missed[MAX_MISSED];
  uint8_t missedCount, missedHead;
  uint32_t penStart;
  uint32_t heatMin;
  float maxTemp;
  uint8_t frozen;
} st;

enum State { S_OK, S_INJECTING, S_GUARD, S_CAP_OFF_LONG, S_FROZEN, S_MISSED,
             S_HOT, S_HEAT_BUDGET, S_PEN_EXPIRED, S_NO_DOSE_YET };
const char* STATE_NAMES[] = { "OK", "INJECTING", "GUARD", "CAP_LEFT_OFF", "FROZEN", "MISSED",
                              "HOT", "HEAT_EXPOSED", "PEN_EXPIRED", "NO_DOSE_YET" };

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

// virtual clock
uint32_t baseEpoch = 1793113200UL;  // 27 Oct 2026 18:00 (+03), replaced by NTP
uint32_t baseMillis = 0;
bool clockSynced = false;

bool capOn = true;
uint32_t capOffAtMs = 0;
uint32_t capOffAtEpoch = 0;
bool guardTriggered = false;
bool silenced = false;           // button pressed during the current alert
uint32_t missedAckedDue = 0;     // due time the user acknowledged
uint32_t lastMissedLogged = 0;

float tempC = NAN;
uint32_t lastHeatTick = 0;
bool hotNow = false;

bool btnDown = false, longDone = false;
uint32_t btnDownMs = 0, btnChangeMs = 0;

State state = S_OK;
State lastPublishedState = S_OK;
uint32_t lastSampleMs = 0, lastDrawMs = 0, lastTelemetryMs = 0;
uint32_t lastWifiTry = 0, lastMqttTry = 0;
bool logDirty = true;

char tTelemetry[48], tEvents[48], tLog[48];

// ============================== Time helpers ==============================

uint32_t nowEpoch() {
  return baseEpoch + (uint32_t)((uint64_t)(millis() - baseMillis) * TIME_SCALE / 1000ULL);
}

uint32_t minutesBetween(uint32_t a, uint32_t b) { return b > a ? (b - a) / 60 : 0; }

// Epoch of the most recent DOSE_HOUR:DOSE_MINUTE at or before `t`
uint32_t lastDueAt(uint32_t t) {
  time_t tt = t;
  struct tm lt;
  localtime_r(&tt, &lt);
  lt.tm_hour = DOSE_HOUR; lt.tm_min = DOSE_MINUTE; lt.tm_sec = 0;
  uint32_t due = (uint32_t)mktime(&lt);
  if (due > t) due -= 86400;
  return due;
}

void formatClock(uint32_t t, char* out, size_t n) {
  time_t tt = t;
  struct tm lt;
  localtime_r(&tt, &lt);
  snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

void formatSpan(uint32_t minutes, char* out, size_t n) {
  if (minutes < 60) snprintf(out, n, "%lum", (unsigned long)minutes);
  else if (minutes < 48 * 60) snprintf(out, n, "%luh %02lum", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
  else snprintf(out, n, "%lud %luh", (unsigned long)(minutes / 1440), (unsigned long)((minutes % 1440) / 60));
}

// ============================== Storage ==============================

void saveStore() {
  prefs.putBytes("store", &st, sizeof(st));
  logDirty = true;
}

void loadStore() {
  if (prefs.getBytesLength("store") == sizeof(st)) {
    prefs.getBytes("store", &st, sizeof(st));
  } else {
    memset(&st, 0, sizeof(st));
    st.penStart = nowEpoch();
    st.maxTemp = -100;
    saveStore();
  }
}

const Dose* lastDose() {
  if (st.doseCount == 0) return nullptr;
  return &st.doses[(st.doseHead + MAX_DOSES - 1) % MAX_DOSES];
}

void shiftAllTimes(int32_t delta) {
  for (int i = 0; i < st.doseCount; i++) st.doses[(st.doseHead + MAX_DOSES - 1 - i) % MAX_DOSES].t += delta;
  for (int i = 0; i < st.missedCount; i++) st.missed[(st.missedHead + MAX_MISSED - 1 - i) % MAX_MISSED] += delta;
  st.penStart += delta;
  if (capOffAtEpoch) capOffAtEpoch += delta;
  if (missedAckedDue) missedAckedDue += delta;
  if (lastMissedLogged) lastMissedLogged += delta;
}

// ============================== Events ==============================

void publishEvent(const char* type, const char* extra = "") {
  char buf[220];
  snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"t\":%lu%s%s}", type,
           (unsigned long)nowEpoch(), extra[0] ? "," : "", extra);
  Serial.printf("EVENT %s\n", buf);
  if (mqtt.connected()) mqtt.publish(tEvents, buf, false);
}

// ============================== Dose logic ==============================

void recordDose(uint32_t heldMs) {
  uint32_t t = capOffAtEpoch;
  uint8_t flags = 0;
  if (guardTriggered) flags |= F_GUARD;
  uint32_t due = lastDueAt(t + EARLY_WINDOW_MIN * 60);   // the due time this dose belongs to
  if (t > due + GRACE_MIN * 60) flags |= F_LATE;

  Dose d = { t, (uint16_t)min<uint32_t>(heldMs / 1000, 65535), flags, 0 };
  st.doses[st.doseHead] = d;
  st.doseHead = (st.doseHead + 1) % MAX_DOSES;
  if (st.doseCount < MAX_DOSES) st.doseCount++;
  saveStore();

  char extra[96];
  snprintf(extra, sizeof(extra), "\"dose_t\":%lu,\"hold_s\":%u,\"guard\":%s,\"late\":%s",
           (unsigned long)t, d.holdS, (flags & F_GUARD) ? "true" : "false", (flags & F_LATE) ? "true" : "false");
  publishEvent("dose", extra);
}

void onCapRemoved(uint32_t nowMs) {
  capOn = false;
  capOffAtMs = nowMs;
  capOffAtEpoch = nowEpoch();
  silenced = false;
  const Dose* ld = lastDose();
  guardTriggered = ld && minutesBetween(ld->t, capOffAtEpoch) < GUARD_WINDOW_MIN;
  if (guardTriggered) {
    char extra[64];
    snprintf(extra, sizeof(extra), "\"since_min\":%lu", (unsigned long)minutesBetween(ld->t, capOffAtEpoch));
    publishEvent("guard_warning", extra);
  } else {
    publishEvent("cap_off");
  }
}

void onCapReplaced(uint32_t nowMs) {
  capOn = true;
  uint32_t held = nowMs - capOffAtMs;
  if (held >= MIN_INJECTION_MS) recordDose(held);
  else publishEvent("cap_peek");
  guardTriggered = false;
  silenced = false;
}

// Returns the due time whose dose is currently missing past the grace period, or 0
uint32_t overdueDue(uint32_t now) {
  uint32_t due = lastDueAt(now);
  if (now < due + GRACE_MIN * 60) return 0;
  if (st.penStart > due) return 0;                       // new pen / first day
  const Dose* ld = lastDose();
  if (ld && ld->t >= due - EARLY_WINDOW_MIN * 60) return 0;
  return due;
}

void checkMissed(uint32_t now) {
  uint32_t due = lastDueAt(now);
  if (now < due + MISSED_AFTER_MIN * 60) return;
  if (due == lastMissedLogged || st.penStart > due) return;
  const Dose* ld = lastDose();
  if (ld && ld->t >= due - EARLY_WINDOW_MIN * 60) { lastMissedLogged = due; return; }
  // already in the list?
  for (int i = 0; i < st.missedCount; i++)
    if (st.missed[(st.missedHead + MAX_MISSED - 1 - i) % MAX_MISSED] == due) { lastMissedLogged = due; return; }
  st.missed[st.missedHead] = due;
  st.missedHead = (st.missedHead + 1) % MAX_MISSED;
  if (st.missedCount < MAX_MISSED) st.missedCount++;
  lastMissedLogged = due;
  saveStore();
  char extra[40];
  snprintf(extra, sizeof(extra), "\"due\":%lu", (unsigned long)due);
  publishEvent("dose_missed", extra);
}

// ============================== Temperature ==============================

float readNtc() {
  // Wokwi NTC module: 10k NTC, beta 3950, in a divider on a 12-bit ADC
  const float BETA = 3950;
  int raw = analogRead(PIN_NTC);
  if (raw <= 0 || raw >= 4095) return NAN;
  return 1 / (log(1 / (4095.0 / raw - 1)) / BETA + 1.0 / 298.15) - 273.15;
}

void sampleTemperature(uint32_t now) {
  float t = readNtc();
  if (isnan(t)) return;
  tempC = t;
  if (t > st.maxTemp) st.maxTemp = t;

  bool wasHot = hotNow;
  hotNow = t > HEAT_LIMIT_C;
  if (hotNow && !wasHot) { lastHeatTick = now; publishEvent("heat_start"); }
  if (!hotNow && wasHot) publishEvent("heat_end");
  if (hotNow) {
    uint32_t mins = minutesBetween(lastHeatTick, now);
    if (mins) {
      bool before = st.heatMin >= HEAT_BUDGET_MIN;
      st.heatMin += mins;
      lastHeatTick += mins * 60;
      if (!before && st.heatMin >= HEAT_BUDGET_MIN) publishEvent("heat_budget_exceeded");
      saveStore();
    }
  }
  if (t <= FREEZE_LIMIT_C && !st.frozen) {
    st.frozen = 1;
    saveStore();
    publishEvent("frozen");
  }
}

// ============================== State ==============================

uint32_t penDay(uint32_t now) { return (now - st.penStart) / 86400 + 1; }
int penDaysLeft(uint32_t now) { return (int)PEN_LIFE_DAYS - (int)((now - st.penStart) / 86400); }

State computeState(uint32_t nowMs, uint32_t now) {
  if (!capOn) {
    if (guardTriggered) return S_GUARD;
    if (nowMs - capOffAtMs > CAP_OFF_REMIND_MIN * 60000UL / TIME_SCALE) return S_CAP_OFF_LONG;
    return S_INJECTING;
  }
  if (st.frozen) return S_FROZEN;
  uint32_t od = overdueDue(now);
  if (od && od != missedAckedDue && now < od + MISSED_AFTER_MIN * 60) return S_MISSED;
  if (hotNow) return S_HOT;
  if (st.heatMin >= HEAT_BUDGET_MIN) return S_HEAT_BUDGET;
  if (penDaysLeft(now) <= 0) return S_PEN_EXPIRED;
  if (!lastDose()) return S_NO_DOSE_YET;
  return S_OK;
}

bool isAlarm(State s)   { return s == S_GUARD || s == S_FROZEN || s == S_MISSED || s == S_CAP_OFF_LONG; }
bool isWarning(State s) { return s == S_HOT || s == S_HEAT_BUDGET || s == S_PEN_EXPIRED; }

// ============================== Outputs ==============================

void setLed(int r, int g, int b) {
  digitalWrite(PIN_LED_R, r); digitalWrite(PIN_LED_G, g); digitalWrite(PIN_LED_B, b);
}

bool buzzing = false;
void buzz(bool on, int freq = 2600) {
  if (on && !buzzing) { tone(PIN_BUZZER, freq); buzzing = true; }
  if (!on && buzzing) { noTone(PIN_BUZZER); buzzing = false; }
}

void updateOutputs(uint32_t nowMs) {
  uint32_t p;
  switch (state) {
    case S_GUARD:        // urgent: rapid triple beeps, red strobe
      p = nowMs % 1200;
      setLed((nowMs / 100) % 2, 0, 0);
      buzz(!silenced && (p < 120 || (p > 220 && p < 340) || (p > 440 && p < 560)), 3000);
      break;
    case S_FROZEN:
      setLed((nowMs / 400) % 2, 0, (nowMs / 400) % 2);
      buzz(!silenced && nowMs % 20000 < 200, 1800);
      break;
    case S_MISSED:       // gentle double chirp every 8 s, amber
      p = nowMs % 8000;
      setLed(1, (nowMs / 500) % 2, 0);
      buzz(!silenced && (p < 120 || (p > 250 && p < 370)), 2200);
      break;
    case S_CAP_OFF_LONG:
      setLed(1, 1, 0);
      buzz(!silenced && nowMs % 10000 < 150, 2000);
      break;
    case S_INJECTING:    // blue, steady; turns green once the hold time counts as a dose
      if (nowMs - capOffAtMs >= MIN_INJECTION_MS) setLed(0, 1, 0); else setLed(0, 0, 1);
      buzz(false);
      break;
    case S_HOT: case S_HEAT_BUDGET: case S_PEN_EXPIRED:
      setLed(1, 1, 0);
      buzz(false);
      break;
    default:
      setLed(0, (nowMs % 3000) < 80, 0);   // short green heartbeat = all good
      buzz(false);
  }
}

void centerText(const char* s, int y, int size) {
  int16_t x1, y1; uint16_t w, h;
  display.setTextSize(size);
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, y);
  display.print(s);
}

void drawScreen(uint32_t nowMs, uint32_t now) {
  char buf[32], span[16], clk[8];
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // header
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("DoseGuard");
  formatClock(now, clk, sizeof(clk));
  display.setCursor(70, 0);
  display.print(clk);
  display.setCursor(104, 0);
  display.print(mqtt.connected() ? "ON" : (WiFi.status() == WL_CONNECTED ? "W" : "--"));
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  const Dose* ld = lastDose();
  uint32_t since = ld ? minutesBetween(ld->t, now) : 0;

  switch (state) {
    case S_GUARD:
      display.fillRect(0, 12, 128, 20, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      centerText("STOP!", 14, 2);
      display.setTextColor(SSD1306_WHITE);
      formatSpan(since, span, sizeof(span));
      snprintf(buf, sizeof(buf), "Dose taken %s ago", span);
      centerText(buf, 38, 1);
      centerText("Do not inject again", 52, 1);
      break;

    case S_INJECTING: {
      uint32_t held = (nowMs - capOffAtMs) / 1000;
      centerText("CAP OFF", 14, 2);
      snprintf(buf, sizeof(buf), "Hold %lus", (unsigned long)held);
      centerText(buf, 36, 1);
      centerText(held * 1000 >= MIN_INJECTION_MS ? "Dose will be logged" : "Inject, then recap", 50, 1);
      break;
    }

    case S_CAP_OFF_LONG:
      centerText("RECAP", 14, 2);
      centerText("Put the cap back on", 38, 1);
      centerText("to protect insulin", 50, 1);
      break;

    case S_FROZEN:
      centerText("FROZEN", 14, 2);
      centerText("Insulin froze.", 38, 1);
      centerText("Do not use this pen", 50, 1);
      break;

    case S_MISSED:
      centerText("DOSE DUE", 14, 2);
      snprintf(buf, sizeof(buf), "%02d:%02d dose not taken", DOSE_HOUR, DOSE_MINUTE);
      centerText(buf, 38, 1);
      centerText("Press to snooze", 50, 1);
      break;

    default: {
      if (ld) {
        formatSpan(since, span, sizeof(span));
        centerText(span, 15, 2);
        bool clear = since >= GUARD_WINDOW_MIN;
        if (clear) centerText("since last dose", 34, 1);
        else {
          formatSpan(GUARD_WINDOW_MIN - since, span, sizeof(span));
          snprintf(buf, sizeof(buf), "ago - guard %s", span);
          centerText(buf, 34, 1);
        }
      } else {
        centerText("No doses", 15, 2);
        centerText("logged on this pen", 34, 1);
      }
      display.drawFastHLine(0, 45, 128, SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0, 50);
      if (state == S_HOT) snprintf(buf, sizeof(buf), "TOO HOT %.0fC", tempC);
      else if (state == S_HEAT_BUDGET) snprintf(buf, sizeof(buf), "HEAT %lum check", (unsigned long)st.heatMin);
      else if (state == S_PEN_EXPIRED) snprintf(buf, sizeof(buf), "REPLACE PEN");
      else snprintf(buf, sizeof(buf), "Pen %dd left", max(0, penDaysLeft(now)));
      display.print(buf);
      if (!isnan(tempC)) {
        snprintf(buf, sizeof(buf), "%.1fC", tempC);
        display.setCursor(128 - 6 * strlen(buf), 50);
        display.print(buf);
      }
    }
  }
  display.display();
}

// ============================== Button ==============================

void newPen() {
  st.penStart = nowEpoch();
  st.heatMin = 0;
  st.maxTemp = isnan(tempC) ? -100 : tempC;
  st.frozen = 0;
  saveStore();
  publishEvent("new_pen");
}

void handleButton(uint32_t nowMs, uint32_t now) {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down != btnDown && nowMs - btnChangeMs > 30) {
    btnChangeMs = nowMs;
    btnDown = down;
    if (down) { btnDownMs = nowMs; longDone = false; }
    else if (!longDone) {
      silenced = true;
      if (state == S_MISSED) { missedAckedDue = overdueDue(now); publishEvent("missed_snoozed"); }
    }
  }
  if (btnDown && !longDone && nowMs - btnDownMs >= 3000) { longDone = true; newPen(); }
}

// ============================== Network ==============================

void publishLog() {
  static char buf[1400];
  int n = snprintf(buf, sizeof(buf), "{\"doses\":[");
  for (int i = 0; i < st.doseCount; i++) {
    const Dose& d = st.doses[(st.doseHead + MAX_DOSES - st.doseCount + i) % MAX_DOSES];
    n += snprintf(buf + n, sizeof(buf) - n, "%s[%lu,%u,%u]", i ? "," : "", (unsigned long)d.t, d.holdS, d.flags);
  }
  n += snprintf(buf + n, sizeof(buf) - n, "],\"missed\":[");
  for (int i = 0; i < st.missedCount; i++) {
    uint32_t m = st.missed[(st.missedHead + MAX_MISSED - st.missedCount + i) % MAX_MISSED];
    n += snprintf(buf + n, sizeof(buf) - n, "%s%lu", i ? "," : "", (unsigned long)m);
  }
  snprintf(buf + n, sizeof(buf) - n, "]}");
  mqtt.publish(tLog, buf, true);
  logDirty = false;
}

void publishTelemetry(uint32_t nowMs, uint32_t now) {
  const Dose* ld = lastDose();
  uint32_t since = ld ? minutesBetween(ld->t, now) : 0;
  uint32_t guardLeft = (ld && since < GUARD_WINDOW_MIN) ? GUARD_WINDOW_MIN - since : 0;
  uint32_t due = lastDueAt(now) + 86400;
  char tbuf[12], mbuf[12];
  if (isnan(tempC)) strcpy(tbuf, "null"); else snprintf(tbuf, sizeof(tbuf), "%.1f", tempC);
  if (st.maxTemp < -50) strcpy(mbuf, "null"); else snprintf(mbuf, sizeof(mbuf), "%.1f", st.maxTemp);
  char ldbuf[14];
  if (ld) snprintf(ldbuf, sizeof(ldbuf), "%lu", (unsigned long)ld->t); else strcpy(ldbuf, "null");

  char buf[520];
  snprintf(buf, sizeof(buf),
    "{\"id\":\"%s\",\"now\":%lu,\"scale\":%lu,\"state\":\"%s\",\"cap_on\":%s,\"hold_s\":%lu,"
    "\"last_dose\":%s,\"since_min\":%lu,\"guard_left_min\":%lu,\"guard_min\":%lu,"
    "\"due_hour\":%d,\"due_min\":%d,\"next_due\":%lu,\"temp\":%s,\"max_temp\":%s,"
    "\"heat_min\":%lu,\"heat_budget\":%lu,\"heat_limit\":%.0f,\"frozen\":%s,"
    "\"pen_start\":%lu,\"pen_day\":%lu,\"pen_life\":%lu,\"silenced\":%s}",
    DEVICE_ID, (unsigned long)now, (unsigned long)TIME_SCALE, STATE_NAMES[state],
    capOn ? "true" : "false", capOn ? 0UL : (unsigned long)((nowMs - capOffAtMs) / 1000),
    ldbuf, (unsigned long)since, (unsigned long)guardLeft, (unsigned long)GUARD_WINDOW_MIN,
    DOSE_HOUR, DOSE_MINUTE, (unsigned long)due, tbuf, mbuf,
    (unsigned long)st.heatMin, (unsigned long)HEAT_BUDGET_MIN, HEAT_LIMIT_C, st.frozen ? "true" : "false",
    (unsigned long)st.penStart, (unsigned long)penDay(now), (unsigned long)PEN_LIFE_DAYS,
    silenced ? "true" : "false");
  mqtt.publish(tTelemetry, buf, true);
}

void handleNetwork(uint32_t nowMs, uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (nowMs - lastWifiTry > 15000) { lastWifiTry = nowMs; WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS, 6); }
    return;
  }
  // one-time NTP sync of the virtual clock
  if (!clockSynced) {
    time_t real = time(nullptr);
    if (real > 1700000000) {
      int32_t delta = (int32_t)((uint32_t)real - nowEpoch());
      shiftAllTimes(delta);
      baseEpoch = real; baseMillis = millis();
      clockSynced = true;
      saveStore();
      Serial.println("Clock synced via NTP");
    }
  }
  if (!mqtt.connected()) {
    if (nowMs - lastMqttTry > 5000) {
      lastMqttTry = nowMs;
      char cid[40];
      snprintf(cid, sizeof(cid), "doseguard-%s-%04lx", DEVICE_ID, (unsigned long)(esp_random() & 0xFFFF));
      if (mqtt.connect(cid)) { publishEvent("online"); logDirty = true; }
    }
    return;
  }
  mqtt.loop();
  if (logDirty) publishLog();
  if (nowMs - lastTelemetryMs >= 2000 || state != lastPublishedState) {
    lastTelemetryMs = nowMs;
    lastPublishedState = state;
    publishTelemetry(nowMs, now);
  }
}

// ============================== Setup / loop ==============================

void setup() {
  Serial.begin(115200);
  pinMode(PIN_CAP, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
  analogReadResolution(12);

  setenv("TZ", TIMEZONE, 1); tzset();
  baseMillis = millis();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED not found");
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  centerText("DoseGuard", 20, 2);
  centerText("smart insulin cap", 44, 1);
  display.display();

  prefs.begin("doseguard", false);
  loadStore();

  snprintf(tTelemetry, sizeof(tTelemetry), "%s/%s/telemetry", TOPIC_ROOT, DEVICE_ID);
  snprintf(tEvents, sizeof(tEvents), "%s/%s/events", TOPIC_ROOT, DEVICE_ID);
  snprintf(tLog, sizeof(tLog), "%s/%s/log", TOPIC_ROOT, DEVICE_ID);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1600);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  lastWifiTry = millis();
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");

  capOn = digitalRead(PIN_CAP) == LOW;
  if (!capOn) { capOffAtMs = millis(); capOffAtEpoch = nowEpoch(); }
  Serial.println("DoseGuard ready");
  delay(800);
}

void loop() {
  uint32_t nowMs = millis();
  uint32_t now = nowEpoch();

  bool capReading = digitalRead(PIN_CAP) == LOW;
  if (capReading != capOn) {
    if (capReading) onCapReplaced(nowMs); else onCapRemoved(nowMs);
  }

  handleButton(nowMs, now);

  if (nowMs - lastSampleMs >= 1000 || lastSampleMs == 0) {
    lastSampleMs = nowMs;
    sampleTemperature(now);
    checkMissed(now);
  }

  State s = computeState(nowMs, now);
  if (s != state) {
    if (isAlarm(s) || isWarning(s)) silenced = false;
    state = s;
    char extra[40];
    snprintf(extra, sizeof(extra), "\"state\":\"%s\"", STATE_NAMES[s]);
    publishEvent("state", extra);
  }

  updateOutputs(nowMs);
  if (nowMs - lastDrawMs >= 200) { lastDrawMs = nowMs; drawScreen(nowMs, now); }
  handleNetwork(nowMs, now);
}
