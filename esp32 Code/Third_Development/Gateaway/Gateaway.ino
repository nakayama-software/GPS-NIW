// Gateaway_latest_cache.ino - "Always Latest" Gateway (no event queue)
// Strategy: keep ONLY the latest packet per node (GPS + heartbeat separately),
// overwrite old, and send dirty nodes in batches.
// - No std::vector
// - No heap ops inside ESP-NOW callback
// - Out-of-order protected by seq
// - "Always newest" by design

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

// ===== Set unik per gateway =====
const char* GATEWAY_ID = "GW-01";

// ===== Wi-Fi lock =====
const char* ssid = "Niw_3Fg";
const char* pass = "niw041713f";

#define ESPNOW_CH 11
uint8_t AP_BSSID[6] = {0xCC, 0xE1, 0xD5, 0xC5, 0x6D, 0x40};

// ===== API base =====
const char* API_BASE = "https://projects.nakayamairon.com/ncs85283278/NIW_GPS/api/index.php";

// ===== Adaptive Configuration =====
#define MIN_BATCH_SIZE 3
#define MAX_BATCH_SIZE 15
#define ADAPTIVE_BATCH_TIMEOUT_MS 500   // send partial batch after 500ms
#define FORCE_SEND_TIMEOUT_MS 1000      // force send whatever after 1s
#define MIN_INTERVAL_MS 50              // throttle at receiver (per node)
#define MAX_NODES 32

// ===== Payload dari sender =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  flags; // bit0: gpsValid
} pkt_t;

static double from_e7(int32_t v) { return ((double)v) / 1e7; }

// ===== Receive item (stored) =====
typedef struct {
  pkt_t pkt;
  uint8_t mac[6];
  unsigned long rx_ms; // millis() when received by gateway
} rx_t;

// ===== Node stats (kept from your code) =====
struct NodeStats {
  uint32_t last_seq_gps;
  uint32_t last_seq_hb;
  unsigned long last_recv_time;
  uint32_t throttled;
  uint32_t packets_received;
  uint32_t packets_sent;
} nodeStats[MAX_NODES];

// ===== Latest cache per node =====
struct NodeCache {
  // latest GPS update
  rx_t gps;
  uint8_t has_gps;
  uint8_t dirty_gps;

  // latest heartbeat (no GPS fix)
  rx_t hb;
  uint8_t has_hb;
  uint8_t dirty_hb;
};

NodeCache cache[MAX_NODES];

// Protect cache updates between callback and loop
static portMUX_TYPE cacheMux = portMUX_INITIALIZER_UNLOCKED;

// ===== Global counters =====
volatile uint32_t droppedPackets = 0;   // dropped by throttle/overflow rules
volatile uint32_t totalReceived = 0;
volatile uint32_t totalProcessed = 0;
volatile uint32_t totalSent = 0;
volatile uint32_t totalFailed = 0;

// ===== HTTPS client =====
WiFiClientSecure client;
HTTPClient http;
bool httpBusy = false;

// ===== Perf metrics =====
struct PerfMetrics {
  float avg_batch_size;
  float avg_processing_time;
  uint32_t total_batches;
  uint32_t failed_batches;
} perfMetrics = {0, 0, 0, 0};

static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Count dirty nodes (cheap bounded by MAX_NODES)
static uint16_t countDirtyTotal() {
  uint16_t c = 0;
  portENTER_CRITICAL(&cacheMux);
  for (int i = 0; i < MAX_NODES; i++) {
    if (cache[i].dirty_gps) c++;
    if (cache[i].dirty_hb)  c++;
  }
  portEXIT_CRITICAL(&cacheMux);
  return c;
}

// Adaptive batch size based on dirty backlog
static uint16_t getAdaptiveBatchSize() {
  uint16_t dirty = countDirtyTotal();
  if (dirty >= 25) return MAX_BATCH_SIZE;
  if (dirty >= 10) return (MIN_BATCH_SIZE + MAX_BATCH_SIZE) / 2;
  return MIN_BATCH_SIZE;
}

// ===== HTTP POST batch with retry =====
static bool httpPostBatch(const String &endpoint, JsonDocument& doc, int retries = 2) {
  if (WiFi.status() != WL_CONNECTED || httpBusy) return false;

  httpBusy = true;
  bool success = false;
  unsigned long start = millis();

  String url = String(API_BASE) + endpoint;

  String json;
  serializeJson(doc, json);

  for (int attempt = 0; attempt < retries; attempt++) {
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(3000);
    http.setReuse(true);

    int code = http.POST(json);
    String resp = http.getString(); // optional debug
    http.end();

    if (code >= 200 && code < 300) {
      unsigned long elapsed = millis() - start;

      perfMetrics.total_batches++;
      float alpha = 0.3f;
      perfMetrics.avg_batch_size = (1 - alpha) * perfMetrics.avg_batch_size +
                                   alpha * (float)doc["data"].size();
      perfMetrics.avg_processing_time = (1 - alpha) * perfMetrics.avg_processing_time +
                                        alpha * (float)elapsed;

      Serial.printf("[HTTP] ✓ Batch OK: %d items, %lums, code=%d\n",
                    (int)doc["data"].size(), elapsed, code);

      success = true;
      totalSent += doc["data"].size();
      break;
    } else {
      Serial.printf("[HTTP] ✗ Attempt %d failed: code=%d\n", attempt + 1, code);
      if (attempt < retries - 1) delay(100 * (attempt + 1));
    }
  }

  if (!success) {
    perfMetrics.failed_batches++;
    totalFailed += doc["data"].size();
  }

  httpBusy = false;
  return success;
}

// ===== ESP-NOW Receive (overwrite latest) =====
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  totalReceived++;

  if (len != (int)sizeof(pkt_t)) return;

  pkt_t p;
  memcpy(&p, data, sizeof(pkt_t));

  uint32_t nid = p.node_id;
  if (nid >= MAX_NODES) return;

  unsigned long now = millis();
  bool gpsValid = (p.flags & 0x01);

  // Throttle per node to avoid CPU overload (still keeps "latest" enough)
  // GPS allowed at MIN_INTERVAL_MS, heartbeat at 2x interval.
  uint32_t minInterval = gpsValid ? MIN_INTERVAL_MS : (MIN_INTERVAL_MS * 2);

  NodeStats *ns = &nodeStats[nid];
  if (now - ns->last_recv_time < minInterval) {
    ns->throttled++;
    droppedPackets++;
    return;
  }
  ns->last_recv_time = now;

  // Out-of-order protection (separate for GPS and HB)
  if (gpsValid) {
    if (p.seq <= ns->last_seq_gps) {
      droppedPackets++;
      return;
    }
    ns->last_seq_gps = p.seq;
  } else {
    if (p.seq <= ns->last_seq_hb) {
      droppedPackets++;
      return;
    }
    ns->last_seq_hb = p.seq;
  }

  ns->packets_received++;

  rx_t rx;
  rx.pkt = p;
  memcpy(rx.mac, mac, 6);
  rx.rx_ms = now;

  // Overwrite latest in cache (ALWAYS newest wins)
  portENTER_CRITICAL(&cacheMux);
  if (gpsValid) {
    cache[nid].gps = rx;
    cache[nid].has_gps = 1;
    cache[nid].dirty_gps = 1;
  } else {
    cache[nid].hb = rx;
    cache[nid].has_hb = 1;
    cache[nid].dirty_hb = 1;
  }
  portEXIT_CRITICAL(&cacheMux);
}

// ===== Stats reporting =====
unsigned long lastStatsReport = 0;
#define STATS_INTERVAL 10000

void printStats() {
  float successRate = (totalSent + totalFailed) > 0 ?
                      (float)totalSent / (totalSent + totalFailed) * 100.0f : 100.0f;

  uint16_t dirty = countDirtyTotal();

  Serial.println("\n╔════════════════ GATEWAY STATS ════════════════╗");
  Serial.printf("║ Total Recv: %-8u | Processed: %-8u      ║\n", totalReceived, totalProcessed);
  Serial.printf("║ Total Sent: %-8u | Failed: %-8u         ║\n", totalSent, totalFailed);
  Serial.printf("║ Dropped:    %-8u | Success Rate: %5.1f%%   ║\n", droppedPackets, successRate);
  Serial.printf("║ Dirty items (gps+hb): %-5u                  ║\n", dirty);
  Serial.println("╠═══════════════════════════════════════════════╣");
  Serial.printf("║ Avg Batch Size: %5.1f | Avg Time: %5.1fms   ║\n",
                perfMetrics.avg_batch_size, perfMetrics.avg_processing_time);
  Serial.printf("║ Total Batches: %-6u | Failed: %-6u       ║\n",
                perfMetrics.total_batches, perfMetrics.failed_batches);
  Serial.println("╠═══════════════════════════════════════════════╣");

  bool hasActiveNodes = false;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodeStats[i].packets_received > 0) {
      if (!hasActiveNodes) {
        Serial.println("║ Active Nodes:                                 ║");
        hasActiveNodes = true;
      }
      Serial.printf("║  Node %2d: recv=%-5u sent=%-5u thr=%-5u     ║\n",
                    i,
                    nodeStats[i].packets_received,
                    nodeStats[i].packets_sent,
                    nodeStats[i].throttled);
    }
  }

  if (!hasActiveNodes) {
    Serial.println("║ No active nodes                               ║");
  }

  Serial.println("╚═══════════════════════════════════════════════╝\n");
}

// Pick newest dirty GPS node index, excluding already picked
static int pickNewestDirtyGps(const bool picked[MAX_NODES]) {
  int best = -1;
  unsigned long bestTs = 0;

  portENTER_CRITICAL(&cacheMux);
  for (int i = 0; i < MAX_NODES; i++) {
    if (picked[i]) continue;
    if (cache[i].dirty_gps && cache[i].has_gps) {
      unsigned long ts = cache[i].gps.rx_ms;
      if (best == -1 || (long)(ts - bestTs) > 0) {
        best = i;
        bestTs = ts;
      }
    }
  }
  portEXIT_CRITICAL(&cacheMux);

  return best;
}

// Pick newest dirty HB node index, excluding already picked
static int pickNewestDirtyHb(const bool picked[MAX_NODES]) {
  int best = -1;
  unsigned long bestTs = 0;

  portENTER_CRITICAL(&cacheMux);
  for (int i = 0; i < MAX_NODES; i++) {
    if (picked[i]) continue;
    if (cache[i].dirty_hb && cache[i].has_hb) {
      unsigned long ts = cache[i].hb.rx_ms;
      if (best == -1 || (long)(ts - bestTs) > 0) {
        best = i;
        bestTs = ts;
      }
    }
  }
  portEXIT_CRITICAL(&cacheMux);

  return best;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   Real-time GPS Gateway v5.0 LATEST   ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("[CFG] MAX_NODES=%d Batch=%d-%d timeout=%dms\n",
                MAX_NODES, MIN_BATCH_SIZE, MAX_BATCH_SIZE, ADAPTIVE_BATCH_TIMEOUT_MS);

  memset(nodeStats, 0, sizeof(nodeStats));
  memset(cache, 0, sizeof(cache));

  // WiFi setup
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
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
    Serial.println("[WIFI] ✗ Failed - Restarting...");
    ESP.restart();
  }

  Serial.printf("[WIFI] ✓ IP=%s CH=%d RSSI=%d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.channel(),
                WiFi.RSSI());

  // HTTPS client
  client.setInsecure();
  client.setTimeout(3000);
  client.setHandshakeTimeout(3000);

  // Low latency
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ✗ Init failed");
    ESP.restart();
  }

  esp_now_register_recv_cb(onRecv);

  Serial.println("[GW] ✓ Ready - ALWAYS LATEST mode\n");
}

void loop() {
  static unsigned long lastBatchTime = 0;
  static DynamicJsonDocument batchDoc(8192);

  uint16_t targetBatchSize = getAdaptiveBatchSize();

  batchDoc.clear();
  batchDoc["gateway_id"] = GATEWAY_ID;
  JsonArray arr = batchDoc.createNestedArray("data");

  bool pickedGps[MAX_NODES] = {false};
  bool pickedHb[MAX_NODES]  = {false};

  int sentGpsNodes[MAX_NODES];
  uint32_t sentGpsSeq[MAX_NODES];
  int sentHbNodes[MAX_NODES];
  uint32_t sentHbSeq[MAX_NODES];
  int sentGpsCount = 0;
  int sentHbCount  = 0;

  // 1) Newest GPS first
  while ((int)arr.size() < targetBatchSize) {
    int idx = pickNewestDirtyGps(pickedGps);
    if (idx < 0) break;
    pickedGps[idx] = true;

    rx_t r;
    portENTER_CRITICAL(&cacheMux);
    r = cache[idx].gps;
    portEXIT_CRITICAL(&cacheMux);

    char macStr[18];
    macToStr(r.mac, macStr);

    JsonObject item = arr.createNestedObject();
    item["node_id"] = r.pkt.node_id;
    item["seq"] = r.pkt.seq;
    item["sender_mac"] = macStr;
    item["timestamp"] = r.rx_ms;
    item["latitude"] = from_e7(r.pkt.lat_e7);
    item["longitude"] = from_e7(r.pkt.lon_e7);
    item["type"] = "update";

    sentGpsNodes[sentGpsCount] = idx;
    sentGpsSeq[sentGpsCount] = r.pkt.seq;
    sentGpsCount++;

    totalProcessed++;
  }

  // 2) Then heartbeat
  while ((int)arr.size() < targetBatchSize) {
    int idx = pickNewestDirtyHb(pickedHb);
    if (idx < 0) break;
    pickedHb[idx] = true;

    // Avoid wasting slot if GPS for same node is also dirty (GPS already preferred)
    // Still allow HB if it's the only dirty type.
    portENTER_CRITICAL(&cacheMux);
    bool gpsStillDirty = cache[idx].dirty_gps;
    portEXIT_CRITICAL(&cacheMux);
    if (gpsStillDirty) continue;

    rx_t r;
    portENTER_CRITICAL(&cacheMux);
    r = cache[idx].hb;
    portEXIT_CRITICAL(&cacheMux);

    char macStr[18];
    macToStr(r.mac, macStr);

    JsonObject item = arr.createNestedObject();
    item["node_id"] = r.pkt.node_id;
    item["seq"] = r.pkt.seq;
    item["sender_mac"] = macStr;
    item["timestamp"] = r.rx_ms;
    item["type"] = "heartbeat";

    sentHbNodes[sentHbCount] = idx;
    sentHbSeq[sentHbCount] = r.pkt.seq;
    sentHbCount++;

    totalProcessed++;
  }

  unsigned long now = millis();
  unsigned long timeSinceLastBatch = now - lastBatchTime;
  uint32_t currentSize = arr.size();

  bool shouldSend = false;
  if (currentSize >= targetBatchSize) shouldSend = true;
  else if (currentSize >= MIN_BATCH_SIZE && timeSinceLastBatch > ADAPTIVE_BATCH_TIMEOUT_MS) shouldSend = true;
  else if (currentSize > 0 && timeSinceLastBatch > FORCE_SEND_TIMEOUT_MS) shouldSend = true;

  if (shouldSend && currentSize > 0) {
    Serial.printf("[BATCH] Sending %u items (target=%u, dirty=%u)...\n",
                  (unsigned)currentSize,
                  (unsigned)targetBatchSize,
                  (unsigned)countDirtyTotal());

    bool ok = httpPostBatch("/batch", batchDoc, 2);

    if (ok) {
      // Clear dirty only if cache still holds the same seq (not replaced by newer)
      portENTER_CRITICAL(&cacheMux);

      for (int i = 0; i < sentGpsCount; i++) {
        int idx = sentGpsNodes[i];
        uint32_t seq = sentGpsSeq[i];
        if (cache[idx].has_gps && cache[idx].dirty_gps && cache[idx].gps.pkt.seq == seq) {
          cache[idx].dirty_gps = 0;
        }
      }

      for (int i = 0; i < sentHbCount; i++) {
        int idx = sentHbNodes[i];
        uint32_t seq = sentHbSeq[i];
        if (cache[idx].has_hb && cache[idx].dirty_hb && cache[idx].hb.pkt.seq == seq) {
          cache[idx].dirty_hb = 0;
        }
      }

      portEXIT_CRITICAL(&cacheMux);

      // update sent stats
      for (int i = 0; i < sentGpsCount; i++) nodeStats[sentGpsNodes[i]].packets_sent++;
      for (int i = 0; i < sentHbCount; i++) nodeStats[sentHbNodes[i]].packets_sent++;
    }

    lastBatchTime = millis();
  }

  if (millis() - lastStatsReport > STATS_INTERVAL) {
    printStats();
    lastStatsReport = millis();
  }

  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 5000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] ⚠ Reconnecting...");
      WiFi.reconnect();
    }
    lastWiFiCheck = millis();
  }

  if (currentSize == 0) delay(1);
  else yield();
}
