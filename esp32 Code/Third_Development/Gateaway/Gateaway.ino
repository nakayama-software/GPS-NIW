// gateway.ino
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ===== Set unik per gateway =====
const char* GATEWAY_ID = "GW-01";

// ===== Wi-Fi lock =====
const char* ssid = "Niw_3Fg";
const char* pass = "niw041713f";

#define ESPNOW_CH 11
uint8_t AP_BSSID[6] = {0xCC, 0xE1, 0xD5, 0xC5, 0x6D, 0x40};

// ===== API base (hosting) =====
// Pakai index.php karena rewrite /api/update belum aktif
const char* API_BASE = "https://projects.nakayamairon.com/ncs85283278/NIW_GPS/api/index.php";

// ===== payload dari sender =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  flags;  // bit0 gps_valid
} pkt_t;

static double from_e7(int32_t v) { return ((double)v) / 1e7; }

// ===== RX buffer: simpan paket + mac sender =====
typedef struct {
  pkt_t pkt;
  uint8_t mac[6];
} rx_t;

volatile bool hasRx = false;
rx_t lastRx;

static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// HTTPS POST helper
static bool httpPostJson(const String &url, const String &json) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure(); // ⚠️ PoC: skip cert validation

  HTTPClient http;
  http.setTimeout(7000);

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

static void postHeartbeat(uint32_t node_id, const char* sender_mac, uint32_t seq) {
  String url  = String(API_BASE) + "/heartbeat";

  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"node_id\":%u,\"gateway_id\":\"%s\",\"sender_mac\":\"%s\",\"seq\":%u}",
           node_id, GATEWAY_ID, sender_mac, seq);

  String body(buf);

  if (!httpPostJson(url, body)) {
    delay(200);
    httpPostJson(url, body);
  }
}

static void postUpdate(uint32_t node_id, double lat, double lon, const char* sender_mac, uint32_t seq) {
  String url = String(API_BASE) + "/update";

  char buf[260];
  snprintf(buf, sizeof(buf),
           "{\"node_id\":%u,\"latitude\":%.7f,\"longitude\":%.7f,\"gateway_id\":\"%s\",\"sender_mac\":\"%s\",\"seq\":%u}",
           node_id, lat, lon, GATEWAY_ID, sender_mac, seq);

  String body(buf);

  if (!httpPostJson(url, body)) {
    delay(200);
    httpPostJson(url, body);
  }
}

// recv cb (Arduino-ESP32 2.0.x)
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(pkt_t)) return;

  memcpy((void*)&lastRx.pkt, data, sizeof(pkt_t));
  memcpy((void*)lastRx.mac, mac, 6);
  hasRx = true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[BOOT] Gateway start");
  Serial.printf("[GW] ID=%s\n", GATEWAY_ID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  delay(200);

  WiFi.begin(ssid, pass, ESPNOW_CH, AP_BSSID, true);

  Serial.print("[WIFI] Connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WIFI] Failed to connect (status=%d)\n", (int)WiFi.status());
    return;
  }

  Serial.printf("[WIFI] IP=%s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Channel=%d BSSID=%s\n", WiFi.channel(), WiFi.BSSIDstr().c_str());

  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] init failed");
    return;
  }

  esp_now_register_recv_cb(onRecv);

  Serial.println("[GW] Ready (ESP-NOW RX + HTTPS POST)");
  Serial.printf("[GW] API_BASE=%s\n", API_BASE);
}

void loop() {
  if (!hasRx) { delay(1); return; }

  rx_t r;
  memcpy(&r, (const void*)&lastRx, sizeof(rx_t));
  hasRx = false;

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
