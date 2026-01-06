// gateway_optimized.ino - Ultra-optimized for real-time performance
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <vector>
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
#define MAX_QUEUE_SIZE 100
#define MIN_BATCH_SIZE 3              // Minimum untuk efisiensi
#define MAX_BATCH_SIZE 15             // Maximum untuk keamanan
#define ADAPTIVE_BATCH_TIMEOUT_MS 500 // 500ms untuk real-time responsif
#define MIN_INTERVAL_MS 50            // 50ms = 20 updates/sec per node (lebih responsif)

// ===== Payload dari sender =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  flags;
} pkt_t;

static double from_e7(int32_t v) { return ((double)v) / 1e7; }

// ===== Priority Queue Item =====
typedef struct {
  pkt_t pkt;
  uint8_t mac[6];
  unsigned long timestamp;
  uint8_t priority; // 0=heartbeat, 1=GPS update
} rx_t;

// ===== Queue & Stats =====
std::vector<rx_t> rxQueue;
std::vector<rx_t> priorityQueue; // Untuk GPS updates

volatile uint32_t droppedPackets = 0;
volatile uint32_t totalReceived = 0;
volatile uint32_t totalProcessed = 0;
volatile uint32_t totalSent = 0;
volatile uint32_t totalFailed = 0;

#define MAX_NODES 32
struct NodeStats {
  uint32_t last_seq;
  unsigned long last_recv_time;
  unsigned long last_sent_time;
  uint32_t throttled;
  uint32_t packets_received;
  uint32_t packets_sent;
} nodeStats[MAX_NODES];

// ===== WiFi Client (persistent connection) =====
WiFiClientSecure client;
HTTPClient http;
bool httpBusy = false;

// ===== Performance metrics =====
struct PerfMetrics {
  unsigned long last_batch_time;
  float avg_batch_size;
  float avg_processing_time;
  uint32_t total_batches;
  uint32_t failed_batches;
} perfMetrics = {0, 0, 0, 0, 0};

static void macToStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ===== ADAPTIVE BATCH SIZE =====
uint16_t getAdaptiveBatchSize() {
  uint16_t queueSize = rxQueue.size() + priorityQueue.size();
  
  // Jika queue penuh > 70%, kirim batch besar untuk mengosongkan
  if (queueSize > (MAX_QUEUE_SIZE * 0.7)) {
    return MAX_BATCH_SIZE;
  }
  
  // Jika queue medium (30-70%), batch sedang
  if (queueSize > (MAX_QUEUE_SIZE * 0.3)) {
    return (MIN_BATCH_SIZE + MAX_BATCH_SIZE) / 2;
  }
  
  // Jika queue rendah, batch minimum
  return MIN_BATCH_SIZE;
}

// ===== OPTIMIZED HTTP POST with retry =====
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
    http.setTimeout(3000); // Shorter timeout untuk responsif
    http.setReuse(true);   // Reuse connection

    int code = http.POST(json);
    String resp = http.getString();
    http.end();

    if (code >= 200 && code < 300) {
      unsigned long elapsed = millis() - start;
      
      // Update performance metrics
      perfMetrics.total_batches++;
      float alpha = 0.3; // Smoothing factor
      perfMetrics.avg_batch_size = (1 - alpha) * perfMetrics.avg_batch_size + 
                                   alpha * doc["data"].size();
      perfMetrics.avg_processing_time = (1 - alpha) * perfMetrics.avg_processing_time + 
                                        alpha * elapsed;
      
      Serial.printf("[HTTP] ✓ Batch OK: %d items, %dms, code=%d\n", 
                    doc["data"].size(), elapsed, code);
      success = true;
      totalSent += doc["data"].size();
      break;
      
    } else {
      Serial.printf("[HTTP] ✗ Attempt %d failed: code=%d\n", attempt + 1, code);
      
      if (attempt < retries - 1) {
        delay(100 * (attempt + 1)); // Progressive backoff
      }
    }
  }
  
  if (!success) {
    perfMetrics.failed_batches++;
    totalFailed += doc["data"].size();
  }
  
  httpBusy = false;
  return success;
}

// ===== ESP-NOW Receive with Priority =====
void IRAM_ATTR onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  totalReceived++;
  
  if (len != sizeof(pkt_t)) return;

  rx_t rx;
  memcpy(&rx.pkt, data, sizeof(pkt_t));
  memcpy(rx.mac, mac, 6);
  rx.timestamp = millis();

  // Check GPS validity for priority
  bool gpsValid = (rx.pkt.flags & 0x01);
  rx.priority = gpsValid ? 1 : 0;

  // Throttling per node
  uint32_t nid = rx.pkt.node_id;
  if (nid < MAX_NODES) {
    NodeStats* ns = &nodeStats[nid];
    unsigned long now = millis();
    
    // Dynamic throttling: lebih ketat untuk heartbeat, lebih longgar untuk GPS
    uint32_t minInterval = gpsValid ? MIN_INTERVAL_MS : MIN_INTERVAL_MS * 2;
    
    if (now - ns->last_recv_time < minInterval) {
      ns->throttled++;
      return;
    }
    
    ns->last_recv_time = now;
    ns->last_seq = rx.pkt.seq;
    ns->packets_received++;
  }

  // Priority queue untuk GPS updates, normal queue untuk heartbeats
  if (gpsValid && priorityQueue.size() < MAX_QUEUE_SIZE / 2) {
    priorityQueue.push_back(rx);
  } else if (rxQueue.size() < MAX_QUEUE_SIZE) {
    rxQueue.push_back(rx);
  } else {
    droppedPackets++;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  Real-time GPS Gateway v4.0 OPTIMIZED ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("[CFG] Queue=%d Adaptive Batch=%d-%d Interval=%dms\n", 
                MAX_QUEUE_SIZE, MIN_BATCH_SIZE, MAX_BATCH_SIZE, MIN_INTERVAL_MS);

  memset(nodeStats, 0, sizeof(nodeStats));
  rxQueue.reserve(MAX_QUEUE_SIZE);
  priorityQueue.reserve(MAX_QUEUE_SIZE / 2);

  // WiFi setup
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Max power untuk stabilitas
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

  // Setup persistent HTTPS client with optimizations
  client.setInsecure();
  client.setTimeout(3000);
  client.setHandshakeTimeout(3000);

  // WiFi power save off untuk latency rendah
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESPNOW] ✗ Init failed");
    ESP.restart();
  }

  esp_now_register_recv_cb(onRecv);

  Serial.println("[GW] ✓ Ready - Real-time adaptive mode\n");
}

// ===== Enhanced Stats =====
unsigned long lastStatsReport = 0;
#define STATS_INTERVAL 10000

void printStats() {
  float successRate = (totalSent + totalFailed) > 0 ? 
                      (float)totalSent / (totalSent + totalFailed) * 100 : 100;
  
  Serial.println("\n╔════════════════ GATEWAY STATS ════════════════╗");
  Serial.printf("║ Total Recv: %-8u | Processed: %-8u      ║\n", totalReceived, totalProcessed);
  Serial.printf("║ Total Sent: %-8u | Failed: %-8u         ║\n", totalSent, totalFailed);
  Serial.printf("║ Dropped:    %-8u | Success Rate: %5.1f%%   ║\n", droppedPackets, successRate);
  Serial.printf("║ Queue: %3d/%3d | Priority: %3d/%3d         ║\n", 
                rxQueue.size(), MAX_QUEUE_SIZE,
                priorityQueue.size(), MAX_QUEUE_SIZE/2);
  Serial.println("╠═══════════════════════════════════════════════╣");
  Serial.printf("║ Avg Batch Size: %5.1f | Avg Time: %5.1fms   ║\n",
                perfMetrics.avg_batch_size, perfMetrics.avg_processing_time);
  Serial.printf("║ Total Batches: %-6u | Failed: %-6u       ║\n",
                perfMetrics.total_batches, perfMetrics.failed_batches);
  Serial.println("╠═══════════════════════════════════════════════╣");
  
  // Node details
  bool hasActiveNodes = false;
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodeStats[i].packets_received > 0) {
      if (!hasActiveNodes) {
        Serial.println("║ Active Nodes:                                 ║");
        hasActiveNodes = true;
      }
      Serial.printf("║  Node %2d: recv=%-5u sent=%-5u throttled=%-4u ║\n",
                    i, nodeStats[i].packets_received, 
                    nodeStats[i].packets_sent, nodeStats[i].throttled);
    }
  }
  
  if (!hasActiveNodes) {
    Serial.println("║ No active nodes                               ║");
  }
  
  Serial.println("╚═══════════════════════════════════════════════╝\n");
}

void loop() {
  static unsigned long lastBatchTime = 0;
  static DynamicJsonDocument batchDoc(8192); // Larger buffer untuk adaptive batching
  static bool batchInit = false;

  // Initialize batch
  if (!batchInit) {
    batchDoc.clear();
    batchDoc["gateway_id"] = GATEWAY_ID;
    batchDoc["data"] = batchDoc.createNestedArray();
    batchInit = true;
    lastBatchTime = millis();
  }

  uint16_t targetBatchSize = getAdaptiveBatchSize();

  // Process PRIORITY QUEUE first (GPS updates)
  while (!priorityQueue.empty() && batchDoc["data"].size() < targetBatchSize) {
    rx_t r = priorityQueue.front();
    priorityQueue.erase(priorityQueue.begin());

    char macStr[18];
    macToStr(r.mac, macStr);

    JsonObject item = batchDoc["data"].createNestedObject();
    item["node_id"] = r.pkt.node_id;
    item["seq"] = r.pkt.seq;
    item["sender_mac"] = macStr;
    item["timestamp"] = r.timestamp;
    item["latitude"] = from_e7(r.pkt.lat_e7);
    item["longitude"] = from_e7(r.pkt.lon_e7);
    item["type"] = "update";

    totalProcessed++;
    
    if (r.pkt.node_id < MAX_NODES) {
      nodeStats[r.pkt.node_id].packets_sent++;
    }
  }

  // Then process normal queue (heartbeats)
  while (!rxQueue.empty() && batchDoc["data"].size() < targetBatchSize) {
    rx_t r = rxQueue.front();
    rxQueue.erase(rxQueue.begin());

    char macStr[18];
    macToStr(r.mac, macStr);

    JsonObject item = batchDoc["data"].createNestedObject();
    item["node_id"] = r.pkt.node_id;
    item["seq"] = r.pkt.seq;
    item["sender_mac"] = macStr;
    item["timestamp"] = r.timestamp;

    bool gpsValid = (r.pkt.flags & 0x01);
    if (gpsValid) {
      item["latitude"] = from_e7(r.pkt.lat_e7);
      item["longitude"] = from_e7(r.pkt.lon_e7);
      item["type"] = "update";
    } else {
      item["type"] = "heartbeat";
    }

    totalProcessed++;
    
    if (r.pkt.node_id < MAX_NODES) {
      nodeStats[r.pkt.node_id].packets_sent++;
    }
  }

  // Send batch logic - adaptive timeout
  bool shouldSend = false;
  uint32_t currentSize = batchDoc["data"].size();
  unsigned long timeSinceLastBatch = millis() - lastBatchTime;
  
  if (currentSize >= targetBatchSize) {
    shouldSend = true;
  } else if (currentSize >= MIN_BATCH_SIZE && timeSinceLastBatch > ADAPTIVE_BATCH_TIMEOUT_MS) {
    shouldSend = true;
  } else if (currentSize > 0 && timeSinceLastBatch > (ADAPTIVE_BATCH_TIMEOUT_MS * 2)) {
    // Force send jika sudah terlalu lama (1 detik)
    shouldSend = true;
  }

  if (shouldSend && currentSize > 0) {
    Serial.printf("[BATCH] Sending %d items (target=%d)...\n", currentSize, targetBatchSize);
    
    httpPostBatch("/batch", batchDoc, 2);

    // Reset batch
    batchInit = false;
    perfMetrics.last_batch_time = millis();
  }

  // Stats report
  if (millis() - lastStatsReport > STATS_INTERVAL) {
    printStats();
    lastStatsReport = millis();
  }

  // WiFi watchdog
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 5000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] ⚠ Reconnecting...");
      WiFi.reconnect();
    }
    lastWiFiCheck = millis();
  }

  // Memory leak prevention: clear old stats
  static unsigned long lastMemClean = 0;
  if (millis() - lastMemClean > 60000) { // Every minute
    if (rxQueue.capacity() > MAX_QUEUE_SIZE * 2) {
      rxQueue.shrink_to_fit();
    }
    if (priorityQueue.capacity() > (MAX_QUEUE_SIZE / 2) * 2) {
      priorityQueue.shrink_to_fit();
    }
    lastMemClean = millis();
  }

  // Minimal delay - yield untuk watchdog
  if (rxQueue.empty() && priorityQueue.empty()) {
    delay(1);
  } else {
    yield(); // Lebih responsif
  }
}
