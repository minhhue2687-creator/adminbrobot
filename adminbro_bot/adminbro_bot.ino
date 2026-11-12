/*
 * ============================================================
 *  AdminBro Bot  -  ESP32-S3 AI Chatbot (Gemini) + Dasai Mochi style eyes
 *  Màn hình: TFT 1.54" SPI ST7789 240x240
 * ============================================================
 *
 *  THƯ VIỆN CẦN CÀI (Library Manager):
 *    - Adafruit GFX Library
 *    - Adafruit ST7735 and ST7789 Library
 *    - ArduinoJson (v7.x)
 *
 *  BOARD (Arduino IDE):
 *    - Board: "ESP32S3 Dev Module"
 *    - USB CDC On Boot: Enabled   (để dùng Serial Monitor)
 *    - PSRAM: tuỳ board (không bắt buộc)
 *
 *  ĐI DÂY THEO SƠ ĐỒ XIAOZHI ESP32-S3 (ảnh s3nxtft.jpg):
 *    TFT 1.54 ST7789 ->  ESP32-S3
 *      VCC -> 3V3, GND -> GND
 *      SCL -> GPIO 42 (SCK)     SDA -> GPIO 41 (MOSI)
 *      DC  -> GPIO 1            RST -> GPIO 2
 *      BL  -> GPIO 17           CS  -> nối thẳng GND (không dùng chân)
 *    Nút: Wake -> GPIO 47, Vol+ -> GPIO 40, Vol- -> GPIO 39
 *    Mic INMP441: WS -> GPIO 4, SCK -> GPIO 5, SD -> GPIO 6
 *    Loa MAX98357A: DIN -> GPIO 7, BCLK -> GPIO 15, LRC -> GPIO 16
 *    (Mic / loa / nút chưa dùng trong bản này, chỉ khai báo sẵn chân.)
 *
 *  CÁCH DÙNG:
 *    1. Điền WIFI_SSID, WIFI_PASS, GEMINI_API_KEY bên dưới.
 *    2. Nạp code, mở Serial Monitor (115200) rồi gõ câu hỏi, HOẶC
 *       mở trình duyệt vào địa chỉ IP in ra trên Serial để chat qua web.
 *    3. Mắt robot sẽ chớp, nhìn quanh, "suy nghĩ" khi chờ AI và cười khi trả lời.
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ===================== CẤU HÌNH NGƯỜI DÙNG =====================
#define BOT_NAME        "AdminBro Bot"
const char* WIFI_SSID      = "TEN_WIFI";
const char* WIFI_PASS      = "MAT_KHAU_WIFI";
const char* GEMINI_API_KEY = "DAN_API_KEY_GEMINI_VAO_DAY";

// Tên model Gemini. Đổi tại đây nếu bạn dùng model khác
// (vd: "gemini-2.5-flash", "gemini-2.0-flash", ...).
const char* GEMINI_MODEL   = "gemini-2.5-flash";

const char* SYSTEM_PROMPT =
  "Ban la AdminBro, mot robot AI de thuong, than thien tren man hinh nho. "
  "Tra loi that ngan gon (toi da 2-3 cau), cung ngon ngu voi nguoi dung. "
  "Khong dung markdown, khong dung emoji, khong dung danh sach.";

// ===================== CHÂN KẾT NỐI (theo sơ đồ Xiaozhi) =====================
// TFT 1.54" ST7789
#define PIN_SCK   42
#define PIN_MOSI  41
#define PIN_DC     1
#define PIN_RST    2
#define PIN_CS    -1     // CS đã nối GND trên sơ đồ
#define PIN_BL    17

// Khai báo sẵn cho các bản sau (chưa dùng)
#define PIN_BTN_WAKE  47
#define PIN_BTN_VOLUP 40
#define PIN_BTN_VOLDN 39
#define PIN_MIC_WS     4
#define PIN_MIC_SCK    5
#define PIN_MIC_SD     6
#define PIN_SPK_DIN    7
#define PIN_SPK_BCLK  15
#define PIN_SPK_LRC   16

// ===================== MÀN HÌNH / MẮT =====================
#define SCR_W          240
#define SCR_H          240
#define EYE_AREA_H     130          // vùng vẽ mắt (canvas) phía trên
#define EYE_CY          55          // tâm dọc của mắt trong canvas
#define EYE_W           58
#define EYE_H_NORMAL    78
#define EYE_COLOR     0xFFFF        // trắng giống Dasai Mochi (thử 0x07FF cho xanh cyan)

#define TEXT_Y        134
#define MAXC           19           // số ký tự mỗi dòng (size 2)
#define LINES_PER_PAGE  6

Adafruit_ST7789 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16* canvas = nullptr;
WebServer server(80);

enum Mood { M_IDLE = 0, M_THINKING, M_HAPPY, M_ERROR };

// ===================== TRẠNG THÁI CHIA SẺ GIỮA 2 CORE =====================
SemaphoreHandle_t mtx;
volatile int      mood = M_IDLE;
volatile bool     busy = false;
volatile bool     hasQ = false;
volatile bool     replyReady = false;
volatile uint32_t replySeq = 0;
String pendingQ;
String replyFull;

// ===================== HOẠT ẢNH MẮT =====================
float eyeOpen = 1.0f, eyeH = EYE_H_NORMAL, lookX = 0, lookY = 0;
float lookTX = 0, lookTY = 0;
uint32_t nextBlink = 2000, blinkUntil = 0, nextLook = 1000;
int lastMood = -1;
uint32_t moodSince = 0;
uint32_t lastFrame = 0;

// ===================== VÙNG CHỮ =====================
String   lines[48];
int      nLines = 0, page = 0;
uint32_t pageAt = 0;
bool     textDirty = true;

// ============================================================
//  XỬ LÝ CHỮ: UTF-8 tiếng Việt -> ASCII (font mặc định không có dấu)
// ============================================================
static uint32_t nextCp(const char*& p) {
  uint8_t c = (uint8_t)*p++;
  if (c < 0x80) return c;
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
  uint32_t cp = (n == 3) ? (c & 0x07) : (n == 2) ? (c & 0x0F) : (c & 0x1F);
  while (n-- && *p) cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
  return cp;
}

struct ViGroup { const char* chars; char base; };
static const ViGroup VI[] = {
  {"àáảãạăằắẳẵặâầấẩẫậ", 'a'}, {"ÀÁẢÃẠĂẰẮẲẴẶÂẦẤẨẪẬ", 'A'},
  {"èéẻẽẹêềếểễệ", 'e'},       {"ÈÉẺẼẸÊỀẾỂỄỆ", 'E'},
  {"ìíỉĩị", 'i'},             {"ÌÍỈĨỊ", 'I'},
  {"òóỏõọôồốổỗộơờớởỡợ", 'o'}, {"ÒÓỎÕỌÔỒỐỔỖỘƠỜỚỞỠỢ", 'O'},
  {"ùúủũụưừứửữự", 'u'},       {"ÙÚỦŨỤƯỪỨỬỮỰ", 'U'},
  {"ỳýỷỹỵ", 'y'},             {"ỲÝỶỸỴ", 'Y'},
  {"đ", 'd'},                 {"Đ", 'D'},
};

String foldAscii(const String& s) {
  String out;
  const char* p = s.c_str();
  while (*p) {
    uint32_t cp = nextCp(p);
    if (cp < 128) {
      if (cp == '*' || cp == '#' || cp == '`' || cp == '\r') continue;
      if (cp == '\t') cp = ' ';
      out += (char)cp;
      continue;
    }
    bool done = false;
    for (const auto& g : VI) {
      const char* q = g.chars;
      while (*q) {
        if (nextCp(q) == cp) { out += g.base; done = true; break; }
      }
      if (done) break;
    }
    if (done) continue;
    if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0x2026) out += "...";
    // các ký tự khác (emoji...) bị bỏ qua
  }
  return out;
}

static void pushLine(const String& l) { if (nLines < 48) lines[nLines++] = l; }

void wrapText(const String& s) {
  nLines = 0;
  String line, word;
  for (size_t i = 0; i <= s.length(); i++) {
    char c = (i < s.length()) ? s[i] : ' ';
    if (c == '\n' || c == ' ') {
      if (word.length()) {
        while ((int)word.length() > MAXC) {
          if (line.length()) { pushLine(line); line = ""; }
          pushLine(word.substring(0, MAXC));
          word = word.substring(MAXC);
        }
        int add = word.length() + (line.length() ? 1 : 0);
        if ((int)line.length() + add > MAXC) { pushLine(line); line = word; }
        else line += (line.length() ? " " : "") + word;
        word = "";
      }
      if (c == '\n' && line.length()) { pushLine(line); line = ""; }
    } else word += c;
  }
  if (line.length()) pushLine(line);
}

void setText(const String& utf8) {
  wrapText(foldAscii(utf8));
  page = 0;
  pageAt = millis();
  textDirty = true;
}

void drawText() {
  tft.fillRect(0, TEXT_Y, SCR_W, SCR_H - TEXT_Y, 0x0000);
  tft.setTextColor(0xFFFF);
  tft.setTextSize(2);
  for (int i = 0; i < LINES_PER_PAGE; i++) {
    int idx = page * LINES_PER_PAGE + i;
    if (idx >= nLines) break;
    tft.setCursor(4, TEXT_Y + 4 + i * 16);
    tft.print(lines[idx]);
  }
  textDirty = false;
}

// ============================================================
//  VẼ MẮT
// ============================================================
static void drawEye(int cx, int cy, int w, int h) {
  if (h < 4) h = 4;
  int r = min(min(w / 2, h / 2), 20);
  canvas->fillRoundRect(cx - w / 2, cy - h / 2, w, h, r, EYE_COLOR);
}

static void happyEye(int cx, int cy) {            // mắt cười ^
  canvas->fillCircle(cx, cy + 15, 32, EYE_COLOR);
  canvas->fillCircle(cx, cy + 27, 26, 0x0000);
  canvas->fillRect(cx - 40, cy + 27, 80, 40, 0x0000);
}

static void xEye(int cx, int cy) {                // mắt X khi lỗi
  for (int d = -3; d <= 3; d++) {
    canvas->drawLine(cx - 25 + d, cy - 25, cx + 25 + d, cy + 25, EYE_COLOR);
    canvas->drawLine(cx + 25 + d, cy - 25, cx - 25 + d, cy + 25, EYE_COLOR);
  }
}

void renderEyes() {
  canvas->fillScreen(0x0000);
  int lx = 68 + (int)lookX, rx = 172 + (int)lookX, cy = EYE_CY + (int)lookY;
  int h = (int)(eyeH * eyeOpen);
  switch (mood) {
    case M_HAPPY: happyEye(lx, EYE_CY); happyEye(rx, EYE_CY); break;
    case M_ERROR: xEye(lx, EYE_CY);     xEye(rx, EYE_CY);     break;
    default:      drawEye(lx, cy, EYE_W, h); drawEye(rx, cy, EYE_W, h); break;
  }
  if (mood == M_THINKING) {                        // ba chấm nhảy
    int ph = (millis() / 300) % 3;
    for (int i = 0; i < 3; i++)
      canvas->fillCircle(100 + i * 20, 118, (i == ph) ? 6 : 3, EYE_COLOR);
  }
  tft.drawRGBBitmap(0, 0, canvas->getBuffer(), SCR_W, EYE_AREA_H);
}

void updateAnim() {
  uint32_t now = millis();
  float tH = 1.0f, tx = 0, ty = 0, tEyeH = EYE_H_NORMAL;

  if (mood == M_THINKING) {
    tx = 16; ty = -12; tEyeH = 58;
  } else if (mood == M_IDLE) {
    if (now > nextLook) {
      lookTX = random(-18, 19);
      lookTY = random(-8, 9);
      nextLook = now + random(1500, 4000);
    }
    tx = lookTX; ty = lookTY;
  }

  if (now > nextBlink) {
    blinkUntil = now + 130;
    nextBlink = now + random(2500, 6000);
  }
  if (now < blinkUntil) tH = 0.08f;

  eyeOpen += (tH - eyeOpen) * 0.5f;
  eyeH    += (tEyeH - eyeH) * 0.25f;
  lookX   += (tx - lookX) * 0.25f;
  lookY   += (ty - lookY) * 0.25f;
}

void frame() {
  uint32_t now = millis();
  if (now - lastFrame < 25) return;                // ~40 FPS tối đa
  lastFrame = now;

  if (mood != lastMood) { lastMood = mood; moodSince = now; }
  if ((mood == M_HAPPY || mood == M_ERROR) && now - moodSince > 6000) mood = M_IDLE;

  updateAnim();
  renderEyes();

  if (nLines > LINES_PER_PAGE && now - pageAt > 4000) {
    int pages = (nLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
    page = (page + 1) % pages;
    pageAt = now;
    textDirty = true;
  }
  if (textDirty) drawText();
}

// ============================================================
//  GEMINI (chạy trên core 0 để không làm đứng hoạt ảnh)
// ============================================================
struct Turn { String role; String text; };
Turn hist[6];
int  histN = 0;

static void histPush(const char* role, const String& text) {
  if (histN == 6) {
    for (int i = 0; i < 5; i++) hist[i] = hist[i + 1];
    histN = 5;
  }
  hist[histN++] = { role, text };
}

bool askGemini(const String& q, String& out) {
  if (WiFi.status() != WL_CONNECTED) { out = "Loi: mat ket noi WiFi"; return false; }

  JsonDocument doc;
  doc["system_instruction"]["parts"][0]["text"] = SYSTEM_PROMPT;
  JsonArray contents = doc["contents"].to<JsonArray>();
  for (int i = 0; i < histN; i++) {
    JsonObject o = contents.add<JsonObject>();
    o["role"] = hist[i].role;
    o["parts"][0]["text"] = hist[i].text;
  }
  JsonObject u = contents.add<JsonObject>();
  u["role"] = "user";
  u["parts"][0]["text"] = q;
  doc["generationConfig"]["maxOutputTokens"] = 512;
  doc["generationConfig"]["temperature"] = 0.8;

  String body;
  serializeJson(doc, body);

  String url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
               GEMINI_MODEL + ":generateContent?key=" + GEMINI_API_KEY;

  WiFiClientSecure client;
  client.setInsecure();                            // bỏ kiểm tra chứng chỉ cho đơn giản
  HTTPClient http;
  http.setTimeout(25000);
  if (!http.begin(client, url)) { out = "Loi: khong mo duoc ket noi"; return false; }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  String payload = http.getString();
  http.end();

  if (code <= 0) { out = "Loi mang: " + String(code); return false; }

  JsonDocument resp;
  if (deserializeJson(resp, payload)) { out = "Loi: phan hoi khong hop le"; return false; }

  const char* t = resp["candidates"][0]["content"]["parts"][0]["text"];
  if (code != 200 || !t) {
    const char* m = resp["error"]["message"];
    out = String("Loi ") + code + (m ? String(": ") + m : String(""));
    return false;
  }
  out = String(t);
  out.trim();
  histPush("user", q);
  histPush("model", out);
  return true;
}

void geminiTask(void*) {
  for (;;) {
    if (hasQ) {
      String q;
      xSemaphoreTake(mtx, portMAX_DELAY);
      q = pendingQ; hasQ = false;
      xSemaphoreGive(mtx);

      String ans;
      bool ok = askGemini(q, ans);

      xSemaphoreTake(mtx, portMAX_DELAY);
      replyFull = ans;
      replySeq++;
      xSemaphoreGive(mtx);

      mood = ok ? M_HAPPY : M_ERROR;
      replyReady = true;
      busy = false;
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

bool submit(String q) {
  q.trim();
  if (!q.length() || busy) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  pendingQ = q; hasQ = true; busy = true;
  xSemaphoreGive(mtx);
  mood = M_THINKING;
  setText("Dang nghi...");
  Serial.println("[Ban] " + q);
  return true;
}

// ============================================================
//  WEB UI
// ============================================================
static const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AdminBro Bot</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b0b0f;color:#eee;margin:0;display:flex;flex-direction:column;height:100vh}
h1{font-size:18px;padding:12px 16px;margin:0;background:#15151c}
#log{flex:1;overflow:auto;padding:12px 16px}
.m{margin:8px 0;padding:8px 12px;border-radius:12px;max-width:85%;white-space:pre-wrap}
.u{background:#2d5bff;margin-left:auto}.b{background:#22222c}
form{display:flex;gap:8px;padding:12px;background:#15151c}
input{flex:1;padding:10px;border-radius:8px;border:0;background:#22222c;color:#eee;font-size:16px}
button{padding:10px 16px;border:0;border-radius:8px;background:#2d5bff;color:#fff;font-size:16px}
</style></head><body>
<h1>&#129302; AdminBro Bot</h1><div id="log"></div>
<form id="f"><input id="q" placeholder="Nhap cau hoi..." autocomplete="off"><button>Gui</button></form>
<script>
const log=document.getElementById('log'),q=document.getElementById('q');
function add(c,t){const d=document.createElement('div');d.className='m '+c;d.textContent=t;log.appendChild(d);log.scrollTop=log.scrollHeight;return d}
document.getElementById('f').onsubmit=async e=>{
 e.preventDefault();const v=q.value.trim();if(!v)return;q.value='';add('u',v);
 const r=await fetch('/ask?q='+encodeURIComponent(v));
 if(!r.ok){add('b','Bot dang ban, thu lai sau nhe.');return}
 const w=add('b','...');
 const t=setInterval(async()=>{
  const p=await (await fetch('/poll')).json();
  if(!p.busy){clearInterval(t);w.textContent=p.reply}
 },700)};
</script></body></html>
)rawliteral";

static String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') { }
    else o += c;
  }
  return o;
}

void setupWeb() {
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/ask", []() {
    if (submit(server.arg("q"))) server.send(200, "text/plain", "ok");
    else server.send(429, "text/plain", "busy");
  });
  server.on("/poll", []() {
    xSemaphoreTake(mtx, portMAX_DELAY);
    String r = replyFull;
    xSemaphoreGive(mtx);
    String j = String("{\"busy\":") + (busy ? "true" : "false") +
               ",\"reply\":\"" + jsonEscape(r) + "\"}";
    server.send(200, "application/json; charset=utf-8", j);
  });
  server.begin();
}

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  randomSeed(esp_random());
  mtx = xSemaphoreCreateMutex();

  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  tft.init(SCR_W, SCR_H, SPI_MODE3);
  tft.setSPISpeed(40000000);
  tft.setRotation(0);
  tft.fillScreen(0x0000);

  canvas = new GFXcanvas16(SCR_W, EYE_AREA_H);
  if (!canvas || !canvas->getBuffer()) {
    tft.setTextColor(0xF800); tft.setTextSize(2);
    tft.println("Het RAM cho canvas!");
    while (true) delay(1000);
  }

  // Kết nối WiFi (mắt vẫn chuyển động)
  mood = M_THINKING;
  setText("Dang ket noi WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    frame();
    delay(5);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(String("WiFi OK. IP: ") + WiFi.localIP().toString());
    setupWeb();
    mood = M_HAPPY;
    setText(String("Xin chao! Toi la ") + BOT_NAME + ".\nIP: " + WiFi.localIP().toString());
  } else {
    Serial.println("Khong ket noi duoc WiFi");
    mood = M_ERROR;
    setText("Khong co WiFi. Kiem tra SSID/mat khau.");
  }

  xTaskCreatePinnedToCore(geminiTask, "gemini", 16384, nullptr, 1, nullptr, 0);
  Serial.println(String("San sang! Go cau hoi vao day, hoac mo web. (") + BOT_NAME + ")");
}

void loop() {
  server.handleClient();

  // Nhập từ Serial Monitor
  static String sbuf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (sbuf.length()) {
        if (!submit(sbuf)) Serial.println("(bot dang ban hoac cau hoi rong)");
        sbuf = "";
      }
    } else sbuf += c;
  }

  // Có câu trả lời mới -> hiển thị lên màn hình
  if (replyReady) {
    replyReady = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    String r = replyFull;
    xSemaphoreGive(mtx);
    Serial.println("[AdminBro] " + r);
    setText(r);
  }

  frame();
  delay(1);
}
