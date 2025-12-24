// gateway.ino (ESP32 Gateway: ESP-NOW RX + HTTPS POST)
// Tested concept for Arduino-ESP32 2.0.x

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ================== CONFIG ==================

// Unik per gateway
static const char* GATEWAY_ID = "GW-05 NEAR APATO";

// Wi-Fi (PASTIKAN ini SSID 2.4 GHz)
static const char* WIFI_SSID = "PicoCELA_G";
static const char* WIFI_PASS = "niw04171pc";

// ESP-NOW channel yang diharapkan (biasanya sender fixed di sini)
#define ESPNOW_CH 11

// Jika 1: gateway hanya memilih AP (BSSID) yang berada di channel ESPNOW_CH.
// Ini paling aman agar ESP-NOW tidak putus karena beda channel.
#define FORCE_AP_ON_ESPNOW_CH  1

// Jika 1: kalau tidak ada AP SSID di channel ESPNOW_CH, fallback connect ke channel lain.
// WARNING: sender ESP-NOW harus ikut channel hasil koneksi Wi-Fi (akan dicetak di Serial).
#define ALLOW_FALLBACK_OTHER_CH 1

// API base (hosting)
static const char* API_BASE =
  "https://projects.nakayamairon.com/ncs85283278/NIW_GPS/api/index.php";

// Timeout connect Wi-Fi (ms)
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

// HTTP timeout (ms)
static const uint32_t HTTP_TIMEOUT_MS = 7000;

// Retry HTTP
static const uint8_t HTTP_RETRY = 1; // total 2 kali (1 retry)

// ================== PACKET DEFINITIONS ==================

typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  flags;  // bit0 gps_valid
} pkt_t;

static double from_e7(int32_t v) { return ((double)v) / 1e7; }

typedef struct {
  pkt_t   pkt;
  uint8_t mac[6];
} rx_t;

// RX shared state
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool hasRx = false;
static rx_t lastRx;

// ================== UTILS ==================

static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void printWiFiStatusLine() {
  Serial.printf("[WIFI] status=%d ip=%s ch=%d bssid=%s rssi=%d\n",
                (int)WiFi.status(),
                WiFi.localIP().toString().c_str(),
                WiFi.channel(),
                WiFi.BSSIDstr().c_str(),
                WiFi.RSSI());
}

static void scanAndPrintOnce() {
  Serial.println("[SCAN] start...");
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("[SCAN] found=%d\n", n);

  for (int i = 0; i < n; i++) {
    Serial.printf("  %2d) SSID='%s' RSSI=%d CH=%d BSSID=%s ENC=%d\n",
                  i,
                  WiFi.SSID(i).c_str(),
                  WiFi.RSSI(i),
                  WiFi.channel(i),
                  WiFi.BSSIDstr(i).c_str(),
                  (int)WiFi.encryptionType(i));
  }
  WiFi.scanDelete();
}

// pilih AP terbaik untuk SSID target; optional: preferChannelOnly
static bool findBestAP(const char* targetSsid,
                       int preferCh,
                       bool preferChannelOnly,
                       uint8_t outBssid[6],
                       int* outCh,
                       int* outRssi) {
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    WiFi.scanDelete();
    return false;
  }

  int bestIdx = -1;
  int bestRssi = -9999;
  int bestCh = 0;
  uint8_t bestBssid[6] = {0};

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) != targetSsid) continue;

    int ch = WiFi.channel(i);
    if (preferChannelOnly && ch != preferCh) continue;

    int rssi = WiFi.RSSI(i);
    if (rssi > bestRssi) {
      bestRssi = rssi;
      bestIdx = i;
      bestCh = ch;
      const uint8_t* b = WiFi.BSSID(i);
      memcpy(bestBssid, b, 6);
    }
  }

  WiFi.scanDelete();

  if (bestIdx < 0) return false;

  memcpy(outBssid, bestBssid, 6);
  *outCh = bestCh;
  *outRssi = bestRssi;
  return true;
}

static bool connectWiFiLocked(const char* ssid, const char* pass) {
  uint8_t bssid[6];
  int ch = 0, rssi = -9999;

#if FORCE_AP_ON_ESPNOW_CH
  bool ok = findBestAP(ssid, ESPNOW_CH, true, bssid, &ch, &rssi);
#if ALLOW_FALLBACK_OTHER_CH
  if (!ok) {
    Serial.printf("[WIFI] No AP for '%s' on CH=%d. Fallback scanning any channel...\n", ssid, ESPNOW_CH);
    ok = findBestAP(ssid, ESPNOW_CH, false, bssid, &ch, &rssi);
  }
#endif
#else
  bool ok = findBestAP(ssid, ESPNOW_CH, false, bssid, &ch, &rssi);
#endif

  if (!ok) {
    Serial.printf("[WIFI] SSID '%s' not found in scan\n", ssid);
    return false;
  }

  Serial.printf("[WIFI] Best AP SSID='%s' RSSI=%d CH=%d BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n",
                ssid, rssi, ch,
                bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

  // lock to that BSSID + channel
  WiFi.begin(ssid, pass, ch, bssid, true);

  Serial.print("[WIFI] Connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WIFI] Failed to connect (status=%d)\n", (int)WiFi.status());
    return false;
  }

  Serial.printf("[WIFI] Connected. IP=%s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Channel=%d BSSID=%s RSSI=%d\n",
                WiFi.channel(), WiFi.BSSIDstr().c_str(), WiFi.RSSI());

  if (WiFi.channel() != ESPNOW_CH) {
    Serial.printf("[WARN] Wi-Fi connected on CH=%d, not ESPNOW_CH=%d.\n", WiFi.channel(), ESPNOW_CH);
    Serial.println("[WARN] Sender ESP-NOW MUST use the same channel as gateway Wi-Fi, or ESP-NOW will fail.");
  }

  return true;
}

static void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("[WIFI] Disconnected -> reconnecting...");
  WiFi.disconnect(false, true);
  delay(200);
  connectWiFiLocked(WIFI_SSID, WIFI_PASS);
}

// ================== HTTPS POST ==================

static bool httpPostJsonOnce(const String &url, const String &json) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // PoC: skip cert validation

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);

  if (!http.begin(client, url)) {
    Serial.println("[HTTP] begin() failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t*)json.c_str(), json.length());
  String resp = http.getString();
  http.end();

  Serial.printf("[HTTP] code=%d url=%s body=%s resp=%s\n",
                code, url.c_str(), json.c_str(), resp.c_str());

  return (code >= 200 && code < 300);
}

static bool httpPostJsonRetry(const String &url, const String &json) {
  ensureWiFiConnected();

  if (httpPostJsonOnce(url, json)) return true;
  for (uint8_t i = 0; i < HTTP_RETRY; i++) {
    delay(200);
    ensureWiFiConnected();
    if (httpPostJsonOnce(url, json)) return true;
  }
  return false;
}

static void postHeartbeat(uint32_t node_id, const char* sender_mac, uint32_t seq) {
  String url  = String(API_BASE) + "/heartbeat";

  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"node_id\":%u,\"gateway_id\":\"%s\",\"sender_mac\":\"%s\",\"seq\":%u}",
           node_id, GATEWAY_ID, sender_mac, seq);

  httpPostJsonRetry(url, String(buf));
}

static void postUpdate(uint32_t node_id, double lat, double lon, const char* sender_mac, uint32_t seq) {
  String url = String(API_BASE) + "/update";

  char buf[260];
  snprintf(buf, sizeof(buf),
           "{\"node_id\":%u,\"latitude\":%.7f,\"longitude\":%.7f,\"gateway_id\":\"%s\",\"sender_mac\":\"%s\",\"seq\":%u}",
           node_id, lat, lon, GATEWAY_ID, sender_mac, seq);

  httpPostJsonRetry(url, String(buf));
}

// ================== ESP-NOW RX ==================

// recv cb (Arduino-ESP32 2.0.x)
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(pkt_t)) return;

  portENTER_CRITICAL(&rxMux);
  memcpy((void*)&lastRx.pkt, data, sizeof(pkt_t));
  memcpy((void*)lastRx.mac, mac, 6);
  hasRx = true;
  portEXIT_CRITICAL(&rxMux);
}

// ================== SETUP/LOOP ==================

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n[BOOT] Gateway start");
  Serial.printf("[GW] ID=%s\n", GATEWAY_ID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, true);
  delay(200);

  // Optional: kalau kamu ingin lihat SSID yang terlihat
  // scanAndPrintOnce();

  // Connect Wi-Fi dengan lock (scan -> best BSSID)
  if (!connectWiFiLocked(WIFI_SSID, WIFI_PASS)) {
    Serial.println("[BOOT] Wi-Fi connect failed. Check SSID 2.4GHz / channel / password.");
    return;
  }

  // Matikan power save agar ESP-NOW stabil
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Pastikan channel mengikuti Wi-Fi (harus se-channel untuk ESP-NOW)
  esp_wifi_set_channel(WiFi.channel(), WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init failed");
    return;
  }

  esp_now_register_recv_cb(onRecv);

  Serial.println("[GW] Ready (ESP-NOW RX + HTTPS POST)");
  Serial.printf("[GW] API_BASE=%s\n", API_BASE);

  printWiFiStatusLine();
}

void loop() {
  // jaga koneksi Wi-Fi untuk HTTP
  if (WiFi.status() != WL_CONNECTED) {
    ensureWiFiConnected();
  }

  // ambil paket RX bila ada
  rx_t r;
  bool got = false;

  portENTER_CRITICAL(&rxMux);
  if (hasRx) {
    memcpy(&r, (const void*)&lastRx, sizeof(rx_t));
    hasRx = false;
    got = true;
  }
  portEXIT_CRITICAL(&rxMux);

  if (!got) { delay(1); return; }

  char senderMacStr[18];
  macToStr(r.mac, senderMacStr);

  bool gpsValid = (r.pkt.flags & 0x01);

  if (gpsValid) {
    double lat = from_e7(r.pkt.lat_e7);
    double lon = from_e7(r.pkt.lon_e7);

    Serial.printf("[GW] UPDATE node=%u seq=%u lat=%.7f lon=%.7f from=%s\n",
                  r.pkt.node_id, r.pkt.seq, lat, lon, senderMacStr);

    postUpdate(r.pkt.node_id, lat, lon, senderMacStr, r.pkt.seq);
  } else {
    Serial.printf("[GW] HEARTBEAT node=%u seq=%u from=%s\n",
                  r.pkt.node_id, r.pkt.seq, senderMacStr);

    postHeartbeat(r.pkt.node_id, senderMacStr, r.pkt.seq);
  }
}
