// 3D-scan rotation platform — phone-controlled over Wi-Fi
//
// Hardware: ESP32-S2-WROOM + MKS-SERVO42C (closed-loop NEMA17 driver) in STEP/DIR mode.
//
// Wiring (ESP32 -> MKS-SERVO42C):
//   GPIO 40 -> STP   (step pulse)
//   GPIO 39 -> DIR   (direction)
//   GPIO 41 -> ENA   (enable, active LOW on this driver)
//   GND     -> GND   (common ground between ESP32 and driver logic)
//
// Wi-Fi: the board hosts a soft-AP. Connect your phone to it and open the IP
// in a browser. iOS may complain about "no internet"; choose "Use without internet".

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------- pins ----------
constexpr int PIN_STEP = 40;
constexpr int PIN_DIR  = 39;
constexpr int PIN_EN   = 41;

// On-board WS2812 RGB. Common defaults: LOLIN S2 Mini = 15, TinyS2 = 17,
// DFRobot FireBeetle 2 = 45. Saola-1 has no RGB LED. Change if no light shows.
constexpr int PIN_LED  = 15;

// ---------- Wi-Fi AP ----------
constexpr const char* AP_SSID = "RotationPlatform";
constexpr const char* AP_PASS = "rotate1234";   // must be >= 8 chars for WPA2

// ---------- motion config ----------
constexpr int  MOTOR_STEPS   = 200;
constexpr int  MICROSTEPS    = 16;
constexpr long STEPS_PER_REV = (long)MOTOR_STEPS * MICROSTEPS;  // 3200

constexpr unsigned long PULSE_MAX_US = 400;   // slow start (snap-startable)
constexpr unsigned long PULSE_MIN_US = 25;    // cruise (~20 kHz, ~375 RPM)
constexpr long          ACCEL_STEPS  = 300;

constexpr int           AUTO_STEP_DEG  = 10;     // auto-mode move size
constexpr unsigned long AUTO_PERIOD_MS = 2000;   // auto-mode interval

// ---------- state ----------
WebServer server(80);
long g_total_steps = 0;        // signed, cumulative position
long g_accumulator = 0;        // fractional-step accumulator for drift-free deg moves
bool g_auto_on    = false;
unsigned long g_auto_last_ms = 0;
int  g_auto_swept_deg = 0;     // degrees rotated since this auto run started

unsigned long g_led_celebrate_until = 0;
uint32_t      g_led_current = 0xFFFFFFFFu;   // sentinel so first write always runs

// ---------- web UI ----------
constexpr const char* INDEX_HTML = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Rotation Platform</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: -apple-system, system-ui, sans-serif; background:#111; color:#eee;
         margin:0; padding:24px; text-align:center; }
  h1 { font-weight:500; font-size:20px; color:#888; margin:0 0 24px; }
  .row { display:flex; gap:14px; justify-content:center; margin-bottom:18px; }
  button { font-size:28px; padding:28px 0; flex:1; max-width:200px;
           border:0; border-radius:14px; background:#2a6; color:white; touch-action:manipulation; }
  button:active { background:#185; transform: scale(0.98); }
  .small { font-size:18px; padding:18px 0; background:#444; }
  .small:active { background:#333; }
  .auto { max-width:none; background:#26a; font-size:22px; }
  .auto:active { background:#159; }
  .auto.running { background:#c33; }
  .auto.running:active { background:#911; }
  .pos { font-size:42px; font-weight:300; margin:32px 0 8px; }
  .lab { color:#666; font-size:13px; letter-spacing:1px; text-transform:uppercase; }
</style>
</head>
<body>
<h1>Rotation Platform</h1>

<div class="pos" id="pos">0.0&deg;</div>
<div class="lab">cumulative</div>

<div class="row" style="margin-top:28px;">
  <button class="auto" id="autoBtn" onclick="toggleAuto()">Start Auto (10&deg; / 2s)</button>
</div>
<div class="row">
  <button onclick="go(-10)">&minus;10&deg;</button>
  <button onclick="go(10)">+10&deg;</button>
</div>
<div class="row">
  <button class="small" onclick="go(-45)">&minus;45&deg;</button>
  <button class="small" onclick="go(45)">+45&deg;</button>
</div>
<div class="row">
  <button class="small" onclick="go(-1)">&minus;1&deg;</button>
  <button class="small" onclick="go(1)">+1&deg;</button>
</div>
<div class="row">
  <button class="small" onclick="zero()">Zero</button>
</div>

<script>
let pollTimer = null;
async function go(d) {
  const r = await fetch('/step?deg=' + d, { method: 'POST' });
  setAngle(await r.text());
}
async function zero() {
  await fetch('/zero', { method: 'POST' });
  setAngle('0.0');
}
async function toggleAuto() {
  const r = await fetch('/auto', { method: 'POST' });
  applyState(await r.text());
}
async function refreshAngle() {
  // poll /state instead of /angle so the button flips back when the
  // firmware ends auto mode (e.g. after a full revolution)
  const r = await fetch('/state');
  const [s, a] = (await r.text()).split('|');
  setAngle(a);
  if (s === 'off') applyState('off');
}
function setAngle(t) {
  document.getElementById('pos').innerHTML = t + '&deg;';
}
function applyState(s) {
  const btn = document.getElementById('autoBtn');
  if (s === 'on') {
    btn.textContent = 'Stop Auto (10° / 2s)';
    btn.classList.add('running');
    if (!pollTimer) pollTimer = setInterval(refreshAngle, 600);
  } else {
    btn.textContent = 'Start Auto (10° / 2s)';
    btn.classList.remove('running');
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  }
}
// fetch initial state in case the page is reloaded while auto is running
fetch('/state').then(r => r.text()).then(t => {
  const [s, a] = t.split('|');
  setAngle(a);
  applyState(s);
});
</script>
</body>
</html>
)HTML";

// ---------- LED ----------
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (c == g_led_current) return;          // skip writes that wouldn't change anything
  g_led_current = c;
  neopixelWrite(PIN_LED, r, g, b);
}

// Color the LED to reflect the current ambient state (no motion in progress).
void setAmbientLED() {
  if (millis() < g_led_celebrate_until) setLED(0, 40, 0);      // green: just finished a sweep
  else if (g_auto_on)                   setLED(0, 0, 12);      // dim blue: auto armed
  else                                  setLED(0, 0, 0);       // off: idle
}

// ---------- motor ----------
void enableDriver(bool on) { digitalWrite(PIN_EN, on ? LOW : HIGH); }

void stepN(long n) {
  long accel = (n / 2 < ACCEL_STEPS) ? n / 2 : ACCEL_STEPS;
  if (accel < 1) accel = 1;
  for (long i = 0; i < n; ++i) {
    long ramp_pos;
    if (i < accel)             ramp_pos = i;
    else if (i >= n - accel)   ramp_pos = n - 1 - i;
    else                       ramp_pos = accel;
    unsigned long pulse = PULSE_MAX_US -
        ((PULSE_MAX_US - PULSE_MIN_US) * ramp_pos) / accel;
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(pulse);
    digitalWrite(PIN_STEP, LOW);
    delayMicroseconds(pulse);

    // Briefly yield to FreeRTOS every ~100 steps so USB CDC and Wi-Fi keep
    // getting serviced during long moves. Cooperative — no extra delay.
    if ((i & 0x7F) == 0x7F) yield();
  }
}

void rotateDegrees(int deg) {
  g_accumulator += (long)deg * STEPS_PER_REV;
  long steps = g_accumulator / 360;
  g_accumulator -= steps * 360;
  if (steps == 0) return;

  digitalWrite(PIN_DIR, steps > 0 ? HIGH : LOW);
  delayMicroseconds(5);
  setLED(30, 25, 0);                       // yellow while moving
  stepN(steps > 0 ? steps : -steps);
  g_total_steps += steps;
  setAmbientLED();
}

float currentAngle() { return (g_total_steps * 360.0f) / STEPS_PER_REV; }

// ---------- HTTP handlers ----------
void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

void handleStep() {
  if (!server.hasArg("deg")) { server.send(400, "text/plain", "missing deg"); return; }
  int deg = server.arg("deg").toInt();
  rotateDegrees(deg);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", currentAngle());
  server.send(200, "text/plain", buf);
}

void handleZero() {
  g_total_steps = 0;
  g_accumulator = 0;
  server.send(200, "text/plain", "0.0");
}

void handleAuto() {
  g_auto_on = !g_auto_on;
  if (g_auto_on) {
    g_auto_swept_deg = 0;
    g_auto_last_ms = millis() - AUTO_PERIOD_MS;  // step immediately
  }
  server.send(200, "text/plain", g_auto_on ? "on" : "off");
}

void handleAngle() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f", currentAngle());
  server.send(200, "text/plain", buf);
}

void handleState() {
  char buf[32];
  snprintf(buf, sizeof(buf), "%s|%.1f",
           g_auto_on ? "on" : "off", currentAngle());
  server.send(200, "text/plain", buf);
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n3D rotation platform — Wi-Fi control");

  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR,  OUTPUT);
  pinMode(PIN_EN,   OUTPUT);
  digitalWrite(PIN_STEP, LOW);
  digitalWrite(PIN_DIR,  HIGH);
  enableDriver(true);
  delay(100);

  setLED(0, 0, 0);                         // start dark

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP SSID: %s  pass: %s\n", AP_SSID, AP_PASS);
  Serial.printf("Open http://%s in your phone's browser\n", ip.toString().c_str());

  server.on("/",      HTTP_GET,  handleRoot);
  server.on("/step",  HTTP_POST, handleStep);
  server.on("/zero",  HTTP_POST, handleZero);
  server.on("/auto",  HTTP_POST, handleAuto);
  server.on("/angle", HTTP_GET,  handleAngle);
  server.on("/state", HTTP_GET,  handleState);
  server.begin();
}

void loop() {
  server.handleClient();

  if (g_auto_on && (millis() - g_auto_last_ms) >= AUTO_PERIOD_MS) {
    g_auto_last_ms = millis();
    rotateDegrees(AUTO_STEP_DEG);
    g_auto_swept_deg += AUTO_STEP_DEG;
    if (g_auto_swept_deg >= 360) {
      g_auto_on = false;
      g_led_celebrate_until = millis() + 1500;        // green flash on completion
    }
  }

  setAmbientLED();  // expires the green celebration window without needing extra timers
}
