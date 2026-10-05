// ESP32 + ST7789 (240x320): Spotify-style "Now Playing" screen with album art.
// Libraries: Adafruit GFX, Adafruit ST7735 and ST7789, ArduinoJson (v7+), TJpg_Decoder (by Bodmer).

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>

// ================== USER SETTINGS ==================
const char* WIFI_SSID     = "Ariuka";
const char* WIFI_PASS     = "1zurgaa9";

const char* CLIENT_ID     = "d9864d69c1e74583a6c282df07f11b6d";
const char* CLIENT_SECRET = "68a2650a0c9e4f91bb14cfe7766738df";
const char* REFRESH_TOKEN = "AQADj3tX_FWCOy3BkrCAxQEikTEqiTJMyepFJjH7iYHvzzXxIDEDRVDA6_hRzLOL-NpFLHdS1LkqWBhysfpkApkHEWX5geKt7vigmyD40lYWfIkgpzwSzNUyvSOo8c6VO0s";

// ===================================================

// Display pins (ST7789 240x320; SCL->GPIO18, SDA->GPIO23)
#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
#define SCREEN_W 320
#define SCREEN_H 240

// ---------- Color tuning for your panel ----------
#define INVERT_DISPLAY 0   // Your panel showed inverted colors with 1, so 0 is the fix. Try 1 if it ever looks negative.
#define JPG_SWAP_BYTES 0   // Album cover scrambled/rainbow edges? Flip this between 0 and 1.
#define SCREEN_ROTATION 1  // 1 = landscape. 3 = landscape upside down (turn the screen 180 degrees).
#define SWAP_RB        0   // If red and blue are swapped (green looks blue/red), set 1.
#define COLOR_TEST     1   // 1 = show red/green/blue/white bars for 3 s at startup. Set 0 when done.

// Swaps red and blue channels of an RGB565 constant when SWAP_RB is 1
#define FIX(c) (SWAP_RB ? (uint16_t)((((c) & 0x1F) << 11) | ((c) & 0x07E0) | (((c) >> 11) & 0x1F)) : (uint16_t)(c))

// Spotify-like colors (RGB565)
#define C_BG     FIX(0x1082)   // #121212
#define C_GREEN  FIX(0x1DCA)   // #1DB954
#define C_GRAY   FIX(0xB596)   // #B3B3B3
#define C_BAR    FIX(0x528A)   // #535353
#define C_RED    FIX(0xF800)
#define C_WHITE  0xFFFF
#define C_BLACK  0x0000

// ---------- State ----------
String accessToken = "";
unsigned long tokenExpiresAt = 0;

String lastTrackId = "";
String trackName = "", artistName = "", artUrl = "";
int artScale = 2;                 // 300px image / 2 = 150px on screen
long progressMs = 0, durationMs = 0;
unsigned long pollMillis = 0;     // millis() when progressMs was read
bool isPlaying = false;
bool newTrack = false;
bool haveTrack = false;

int  lastFill = -1;
String lastCur = "";
bool drawnPlaying = false;

unsigned long lastPoll = 0;
const unsigned long POLL_INTERVAL = 3000;
unsigned long lastTick = 0;

// ---------- Text helpers ----------
// The built-in display font is ASCII only: keep ASCII, map curly quotes/dashes, drop the rest.
String toAscii(const String& s) {
  String o;
  size_t n = s.length();
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c < 0x80) {
      if (c != '\r') o += (char)c;
    } else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80) {
      unsigned char b = s[i + 2];
      if (b == 0x98 || b == 0x99) o += '\'';
      else if (b == 0x9C || b == 0x9D) o += '"';
      else if (b == 0x93 || b == 0x94) o += '-';
      i += 2;
    }
  }
  return o;
}

String fmtTime(long ms) {
  long s = ms / 1000;
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld:%02ld", s / 60, s % 60);
  return String(buf);
}

int drawWrapped(const String& text, int x, int y, int size, uint16_t color, int maxChars, int maxLines) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  int lineH = 8 * size + 4;
  int lines = 0;
  String rest = text;
  while (rest.length() > 0 && lines < maxLines) {
    String chunk;
    if ((int)rest.length() <= maxChars) {
      chunk = rest;
      rest = "";
    } else {
      int cut = rest.lastIndexOf(' ', maxChars);
      if (cut <= 0) cut = maxChars;
      chunk = rest.substring(0, cut);
      rest = rest.substring(cut);
      rest.trim();
    }
    tft.setCursor(x, y + lines * lineH);
    tft.print(chunk);
    lines++;
  }
  return lines;
}

void showMessage(const char* msg, uint16_t color = C_GREEN) {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.setCursor(10, 105);
  tft.print(msg);
  haveTrack = false;
}

// ---------- JPEG (album art) ----------
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= SCREEN_H) return 0;
#if SWAP_RB
  for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
    uint16_t c = bitmap[i];
    bitmap[i] = ((c & 0x1F) << 11) | (c & 0x07E0) | ((c >> 11) & 0x1F);
  }
#endif
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

void drawArtPlaceholder() {
  tft.fillRect(10, 14, 150, 150, C_BAR);
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(52, 86);
  tft.print("No cover");
}

bool drawAlbumArt() {
  if (artUrl == "") return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  http.begin(client, artUrl);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Album art HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0 || len > 60000) {
    Serial.printf("Album art size not usable: %d\n", len);
    http.end();
    return false;
  }

  uint8_t* buf = (uint8_t*)malloc(len);
  if (!buf) {
    Serial.println("Album art: out of memory");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int got = 0;
  unsigned long t0 = millis();
  while (got < len && millis() - t0 < 8000) {
    int avail = stream->available();
    if (avail > 0) {
      got += stream->readBytes(buf + got, min(avail, len - got));
    } else if (!http.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();

  bool ok = false;
  if (got == len) {
    TJpgDec.setJpgScale(artScale);
    ok = (TJpgDec.drawJpg(10, 14, buf, len) == 0);
  }
  free(buf);
  if (!ok) Serial.println("Album art decode failed");
  return ok;
}

// ---------- Now-playing UI ----------
void drawText() {
  tft.fillRect(172, 0, 148, 172, C_BG);
  tft.setTextWrap(false);

  tft.setTextSize(1);
  tft.setTextColor(C_GREEN);
  tft.setCursor(176, 14);
  tft.print("NOW PLAYING");

  String title = toAscii(trackName);
  if (title.length() == 0) title = "Unknown title";
  int used = drawWrapped(title, 176, 32, 2, C_WHITE, 11, 3);

  int y = 32 + used * 20 + 6;
  drawWrapped(toAscii(artistName), 176, y, 1, C_GRAY, 23, 2);
}

void drawPlayIcon() {
  tft.fillCircle(160, 218, 15, C_WHITE);
  if (isPlaying) {
    tft.fillRect(154, 211, 4, 14, C_BLACK);
    tft.fillRect(162, 211, 4, 14, C_BLACK);
  } else {
    tft.fillTriangle(156, 210, 156, 226, 170, 218, C_BLACK);
  }
  drawnPlaying = isPlaying;
}

void drawControls() {
  // previous
  tft.fillRect(103, 210, 3, 16, C_WHITE);
  tft.fillTriangle(121, 210, 121, 226, 107, 218, C_WHITE);
  // next
  tft.fillTriangle(199, 210, 199, 226, 213, 218, C_WHITE);
  tft.fillRect(214, 210, 3, 16, C_WHITE);
  // shuffle (decorative)
  tft.drawLine(46, 226, 66, 210, C_GRAY);
  tft.drawLine(46, 210, 66, 226, C_GRAY);
  tft.fillTriangle(71, 210, 63, 206, 63, 214, C_GRAY);
  tft.fillTriangle(71, 226, 63, 222, 63, 230, C_GRAY);
  // repeat (decorative)
  tft.drawRoundRect(254, 210, 20, 16, 4, C_GRAY);
  tft.fillTriangle(270, 205, 270, 215, 277, 210, C_GRAY);

  drawPlayIcon();
}

void drawProgress(bool force = false) {
  long est = progressMs + (isPlaying ? (long)(millis() - pollMillis) : 0);
  if (est > durationMs) est = durationMs;
  if (est < 0) est = 0;

  const int x = 14, y = 180, w = 292, h = 4;
  int fill = durationMs > 0 ? (int)((long long)w * est / durationMs) : 0;

  if (force || fill != lastFill) {
    tft.fillRect(x - 7, y - 6, w + 14, 16, C_BG);          // clear bar + knob area
    tft.fillRoundRect(x, y, w, h, 2, C_BAR);
    if (fill > 0) tft.fillRoundRect(x, y, fill < 4 ? 4 : fill, h, 2, C_WHITE);
    tft.fillCircle(x + fill, y + 2, 5, C_WHITE);
    lastFill = fill;
  }

  String cur = fmtTime(est);
  if (force || cur != lastCur) {
    tft.fillRect(14, 192, 50, 10, C_BG);
    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    tft.setCursor(14, 193);
    tft.print(cur);
    lastCur = cur;
  }

  if (force) {
    String total = fmtTime(durationMs);
    tft.fillRect(240, 192, 70, 10, C_BG);
    tft.setTextSize(1);
    tft.setTextColor(C_GRAY);
    tft.setCursor(306 - 6 * total.length(), 193);
    tft.print(total);
  }
}

void drawNowPlaying() {
  tft.fillScreen(C_BG);
  tft.setTextWrap(false);
  drawText();
  drawControls();
  lastFill = -1;
  lastCur = "";
  drawProgress(true);
  drawArtPlaceholder();
  if (!drawAlbumArt()) drawArtPlaceholder();
  haveTrack = true;
}

// ---------- Spotify auth ----------
bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");

  String auth = base64::encode(String(CLIENT_ID) + ":" + String(CLIENT_SECRET));
  http.addHeader("Authorization", "Basic " + auth);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token&refresh_token=" + String(REFRESH_TOKEN);
  int code = http.POST(body);

  if (code != 200) {
    Serial.printf("Token refresh failed: %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return false;

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAt = millis() + (unsigned long)(expiresIn - 60) * 1000UL;
  Serial.println("Got new access token");
  return true;
}

// ---------- Spotify: currently playing ----------
// returns: 1 = track found, 0 = nothing playing, -1 = error
int fetchCurrentlyPlaying() {
  if (accessToken == "" || millis() > tokenExpiresAt) {
    if (!refreshAccessToken()) return -1;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing");
  http.addHeader("Authorization", "Bearer " + accessToken);

  int code = http.GET();

  if (code == 204) { http.end(); return 0; }
  if (code == 401) { http.end(); accessToken = ""; return -1; }
  if (code != 200) {
    Serial.printf("Spotify error: %d\n", code);
    http.end();
    return -1;
  }

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.println("JSON parse error");
    return -1;
  }

  isPlaying  = doc["is_playing"] | false;
  progressMs = doc["progress_ms"] | 0;
  pollMillis = millis();
  durationMs = doc["item"]["duration_ms"] | 0;
  String id  = doc["item"]["id"] | "";
  trackName  = doc["item"]["name"] | "Unknown";
  artistName = doc["item"]["artists"][0]["name"] | "Unknown";

  // images[0]=640px, images[1]=300px, images[2]=64px. Prefer 300px (shown at half size = 150px).
  JsonArray imgs = doc["item"]["album"]["images"];
  artUrl = "";
  if (imgs.size() > 1) { artUrl = imgs[1]["url"].as<String>(); artScale = 2; }
  else if (imgs.size() > 0) { artUrl = imgs[0]["url"].as<String>(); artScale = 4; }

  if (id != lastTrackId) {
    lastTrackId = id;
    newTrack = true;
  }
  return 1;
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);

  tft.init(240, 320);               // if blank/garbled try: tft.init(240, 320, SPI_MODE3);
  tft.invertDisplay(INVERT_DISPLAY);
  tft.setRotation(SCREEN_ROTATION); // landscape: 320x240

#if COLOR_TEST
  // Raw colors (not corrected). You should see RED, GREEN, BLUE, WHITE in this order.
  tft.fillRect(0,   0, 80, 240, 0xF800);
  tft.fillRect(80,  0, 80, 240, 0x07E0);
  tft.fillRect(160, 0, 80, 240, 0x001F);
  tft.fillRect(240, 0, 80, 240, 0xFFFF);
  tft.setTextSize(2);
  tft.setTextColor(0x0000);
  tft.setCursor(8, 110);   tft.print("RED");
  tft.setCursor(88, 110);  tft.print("GREEN");
  tft.setCursor(168, 110); tft.print("BLUE");
  tft.setCursor(248, 110); tft.print("WHITE");
  delay(3000);
#endif

  tft.fillScreen(C_BG);

  TJpgDec.setSwapBytes(JPG_SWAP_BYTES);
  TJpgDec.setCallback(tft_output);

  showMessage("Connecting WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  showMessage("Connected");
  delay(500);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    showMessage("WiFi lost...", C_RED);
    WiFi.reconnect();
    delay(2000);
    return;
  }

  unsigned long now = millis();

  // ----- Poll Spotify -----
  if (now - lastPoll >= POLL_INTERVAL) {
    lastPoll = now;
    int result = fetchCurrentlyPlaying();

    if (result == 1) {
      if (newTrack || !haveTrack) {
        newTrack = false;
        drawNowPlaying();
      } else if (isPlaying != drawnPlaying) {
        drawPlayIcon();
      }
    } else if (result == 0) {
      lastTrackId = "";
      showMessage("Nothing playing");
    } else if (!haveTrack) {
      showMessage("Spotify error", C_RED);
    }
    // on a transient error while a song is shown, keep the screen and retry
  }

  // ----- Smooth progress bar (estimated between polls) -----
  if (haveTrack && now - lastTick >= 250) {
    lastTick = now;
    drawProgress();
    if (isPlaying != drawnPlaying) drawPlayIcon();
  }
}
