// Gateway_latest_cache_fast.ino
// ALWAYS-LATEST + GPS IMMEDIATE (FAST REALTIME MODE)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

// ===== Gateway ID =====
const char* GATEWAY_ID = "GW-01";

// ===== WiFi =====
const char* ssid = "Niw_3Fg";
const char* pass = "niw041713f";

#define ESPNOW_CH 11
uint8_t AP_BSSID[6] = {0xCC, 0xE1, 0xD5, 0xC5, 0x6D, 0x40};

// ===== API =====
const char* API_BASE =
  "https://projects.nakayamairon.com/ncs85283278/NIW_GPS/api/index.php";

// ===== CONFIG =====
#define MAX_NODES 32
#define MIN_BATCH_SIZE 1
#define MAX_BATCH_SIZE 10

#define ADAPTIVE_BATCH_TIMEOUT_MS 100
#define FORCE_SEND_TIMEOUT_MS    200
#define MIN_INTERVAL_MS          50

// ===== PAYLOAD =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  flags;   // bit0: gpsValid
} pkt_t;

static double from_e7(int32_t v) { return ((double)v) / 1e7; }

// ===== RX STORAGE =====
typedef struct {
  pkt_t pkt;
  uint8_t mac[6];
  unsigned long rx_ms;
} rx_t;

// ===== NODE STATS =====
struct NodeStats {
  uint32_t last_seq_gps;
  uint32_t last_seq_hb;
  unsigned long last_recv_time;
  uint32_t throttled;
  uint32_t packets_received;
  uint32_t packets_sent;
} nodeStats[MAX_NODES];

// ===== CACHE =====
struct NodeCache {
  rx_t gps;
  uint8_t has_gps;
  uint8_t dirty_gps;

  rx_t hb;
  uint8_t has_hb;
  uint8_t dirty_hb;
};

NodeCache cache[MAX_NODES];
static portMUX_TYPE cacheMux = portMUX_INITIALIZER_UNLOCKED;

// ===== GLOBAL =====
volatile uint32_t droppedPackets = 0;
volatile uint32_t totalReceived  = 0;
volatile uint32_t totalProcessed = 0;
volatile uint32_t totalSent      = 0;
volatile uint32_t totalFailed    = 0;

volatile bool gpsUrgent = false;

// ===== HTTP =====
WiFiClientSecure client;
HTTPClient http;
bool httpBusy = false;

// ===== PERF =====
struct PerfMetrics {
  float avg_batch_size;
  float avg_processing_time;
  uint32_t total_batches;
  uint32_t failed_batches;
} perf = {0,0,0,0};

// ===== UTIL =====
static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out,18,"%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

// ===== HTTP POST =====
static bool httpPostBatch(JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED || httpBusy) return false;
  httpBusy = true;

  unsigned long start = millis();
  String json;
  serializeJson(doc, json);

  String url = String(API_BASE) + "/batch";
  bool ok = false;

  for (int i=0;i<2;i++) {
    http.begin(client, url);
    http.addHeader("Content-Type","application/json");
    http.setTimeout(3000);
    http.setReuse(true);

    int code = http.POST(json);
    http.end();

    if (code >= 200 && code < 300) {
      unsigned long dt = millis() - start;
      float a = 0.3f;
      perf.avg_batch_size = (1-a)*perf.avg_batch_size + a*doc["data"].size();
      perf.avg_processing_time = (1-a)*perf.avg_processing_time + a*dt;
      perf.total_batches++;
      totalSent += doc["data"].size();
      ok = true;
      break;
    }
    delay(100*(i+1));
  }

  if (!ok) {
    perf.failed_batches++;
    totalFailed += doc["data"].size();
  }

  httpBusy = false;
  return ok;
}

// ===== ESP-NOW RX =====
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  totalReceived++;
  if (len != sizeof(pkt_t)) return;

  pkt_t p;
  memcpy(&p, data, sizeof(pkt_t));
  if (p.node_id >= MAX_NODES) return;

  bool gpsValid = (p.flags & 0x01);
  unsigned long now = millis();

  NodeStats *ns = &nodeStats[p.node_id];
  uint32_t minInt = gpsValid ? MIN_INTERVAL_MS : (MIN_INTERVAL_MS*2);

  if (now - ns->last_recv_time < minInt) {
    ns->throttled++;
    droppedPackets++;
    return;
  }
  ns->last_recv_time = now;

  if (gpsValid) {
    if (p.seq <= ns->last_seq_gps) return;
    ns->last_seq_gps = p.seq;
  } else {
    if (p.seq <= ns->last_seq_hb) return;
    ns->last_seq_hb = p.seq;
  }

  ns->packets_received++;

  rx_t rx;
  rx.pkt = p;
  memcpy(rx.mac, mac, 6);
  rx.rx_ms = now;

  portENTER_CRITICAL(&cacheMux);
  if (gpsValid) {
    cache[p.node_id].gps = rx;
    cache[p.node_id].has_gps = 1;
    cache[p.node_id].dirty_gps = 1;
    gpsUrgent = true;
  } else {
    cache[p.node_id].hb = rx;
    cache[p.node_id].has_hb = 1;
    cache[p.node_id].dirty_hb = 1;
  }
  portEXIT_CRITICAL(&cacheMux);
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  memset(cache,0,sizeof(cache));
  memset(nodeStats,0,sizeof(nodeStats));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(ssid, pass, ESPNOW_CH, AP_BSSID, true);

  while (WiFi.status()!=WL_CONNECTED) delay(200);

  client.setInsecure();
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_now_init();
  esp_now_register_recv_cb(onRecv);

  Serial.println("[GW] READY - FAST LATEST MODE");
}

// ===== LOOP =====
void loop() {
  static unsigned long lastSend = 0;
  static DynamicJsonDocument doc(4096);

  doc.clear();
  doc["gateway_id"] = GATEWAY_ID;
  JsonArray arr = doc.createNestedArray("data");

  bool usedGps[MAX_NODES] = {0};

  // GPS first (urgent)
  for (int i=0;i<MAX_NODES && arr.size()<MAX_BATCH_SIZE;i++) {
    portENTER_CRITICAL(&cacheMux);
    if (cache[i].dirty_gps && cache[i].has_gps) {
      rx_t r = cache[i].gps;
      cache[i].dirty_gps = 0;
      portEXIT_CRITICAL(&cacheMux);

      char macStr[18];
      macToStr(r.mac, macStr);

      JsonObject o = arr.createNestedObject();
      o["node_id"] = r.pkt.node_id;
      o["seq"] = r.pkt.seq;
      o["sender_mac"] = macStr;
      o["timestamp"] = r.rx_ms;
      o["latitude"] = from_e7(r.pkt.lat_e7);
      o["longitude"] = from_e7(r.pkt.lon_e7);
      o["type"] = "update";

      nodeStats[i].packets_sent++;
      totalProcessed++;
      usedGps[i]=1;
    } else {
      portEXIT_CRITICAL(&cacheMux);
    }
  }

  // Heartbeat
  for (int i=0;i<MAX_NODES && arr.size()<MAX_BATCH_SIZE;i++) {
    if (usedGps[i]) continue;

    portENTER_CRITICAL(&cacheMux);
    if (cache[i].dirty_hb && cache[i].has_hb) {
      rx_t r = cache[i].hb;
      cache[i].dirty_hb = 0;
      portEXIT_CRITICAL(&cacheMux);

      char macStr[18];
      macToStr(r.mac, macStr);

      JsonObject o = arr.createNestedObject();
      o["node_id"] = r.pkt.node_id;
      o["seq"] = r.pkt.seq;
      o["sender_mac"] = macStr;
      o["timestamp"] = r.rx_ms;
      o["type"] = "heartbeat";

      nodeStats[i].packets_sent++;
      totalProcessed++;
    } else {
      portEXIT_CRITICAL(&cacheMux);
    }
  }

  unsigned long now = millis();
  bool shouldSend =
    (gpsUrgent && arr.size()>0) ||
    (arr.size()>=MIN_BATCH_SIZE && now-lastSend>ADAPTIVE_BATCH_TIMEOUT_MS) ||
    (arr.size()>0 && now-lastSend>FORCE_SEND_TIMEOUT_MS);

  if (shouldSend && arr.size()>0) {
    if (httpPostBatch(doc)) gpsUrgent=false;
    lastSend = millis();
  }

  delay(1);
}
