// gateway.ino
// latest update 1/15/2026

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <queue>
#include <ArduinoJson.h>

// * Gateway ID *
// Third Floor
// const char* GATEWAY_ID = "GW-01";
// const char* ssid = "Niw_3Fg";
// const char* pass = "niw041713f";

// Daisan
// const char* GATEWAY_ID = "GW-02";
// const char* ssid = "NiwSouko3";
// const char* pass = "niw04171s3";

// Front Factory
const char *GATEWAY_ID = "GW-03";
const char *ssid = "Niw2ndFactory";
const char *pass = "niw04171k2";

// * Configuration *
#define MAX_QUEUE_SIZE 50
#define BATCH_SIZE 5          
#define BATCH_TIMEOUT_MS 2000 
#define MIN_INTERVAL_MS 100   

// * Payload from sender *
typedef struct __attribute__((packed))
{
  uint32_t node_id;
  uint32_t seq;
  int32_t lat_e7;
  int32_t lon_e7;
  uint8_t flags;
} pkt_t;

static double from_e7(int32_t v)
{
  return ((double)v) / 1e7;
}

typedef struct
{
  pkt_t pkt;
  uint8_t mac[6];
  unsigned long timestamp;
} rx_t;

// ===== Queue & Stats =====
std::queue<rx_t> rxQueue;
volatile uint32_t droppedPackets = 0;
volatile uint32_t totalReceived = 0;
volatile uint32_t totalProcessed = 0;

// ===== WiFi Client (reuse connection) =====
WiFiClientSecure client;
HTTPClient http;

static void macToStr(const uint8_t mac[6], char out[18])
{
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ===== BATCH HTTP POST =====
// * API base *
const char *API_BASE = "https://projects.nakayamairon.com/ncs85283278/NIW_GPS/api/index.php";

static bool httpPostBatch(const String &endpoint, JsonDocument &doc)
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String url = String(API_BASE) + endpoint;
  String json;
  serializeJson(doc, json);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int code = http.POST(json);
  String resp = http.getString();
  http.end();

  if (code >= 200 && code < 300)
  {
    Serial.printf("[HTTP] Batch OK: %d items, code=%d\n", doc["data"].size(), code);
    return true;
  }
  else
  {
    Serial.printf("[HTTP] Batch FAIL: code=%d resp=%s\n", code, resp.c_str());
    return false;
  }
}

// ===== ESP-NOW Receive =====
#define MAX_NODES 16
struct NodeStats
{
  uint32_t last_seq;
  unsigned long last_recv_time;
  unsigned long last_sent_time;
  uint32_t throttled;
} nodeStats[MAX_NODES];

void onRecv(const uint8_t *mac, const uint8_t *data, int len)
{
  totalReceived++;

  if (len != sizeof(pkt_t))
    return;

  rx_t rx;
  memcpy(&rx.pkt, data, sizeof(pkt_t));
  memcpy(rx.mac, mac, 6);
  rx.timestamp = millis();

  uint32_t nid = rx.pkt.node_id;
  if (nid < MAX_NODES)
  {
    NodeStats *ns = &nodeStats[nid];
    unsigned long now = millis();

    if (now - ns->last_recv_time < MIN_INTERVAL_MS)
    {
      ns->throttled++;
      return;
    }

    ns->last_recv_time = now;
    ns->last_seq = rx.pkt.seq;
  }

  if (rxQueue.size() < MAX_QUEUE_SIZE)
  {
    rxQueue.push(rx);
  }
  else
  {
    droppedPackets++;
  }
}

// ===== Stats =====
unsigned long lastStatsReport = 0;
#define STATS_INTERVAL 15000

void printStats()
{
  Serial.println("\n========== STATS ==========");
  Serial.printf("Recv: %u | Proc: %u | Drop: %u | Queue: %d/%d\n",
                totalReceived, totalProcessed, droppedPackets,
                rxQueue.size(), MAX_QUEUE_SIZE);

  for (int i = 0; i < MAX_NODES; i++)
  {
    if (nodeStats[i].last_recv_time > 0)
    {
      Serial.printf("Node %d: seq=%u throttled=%u\n",
                    i, nodeStats[i].last_seq, nodeStats[i].throttled);
    }
  }
  Serial.println("===========================\n");
}

// ===== MAIN =====
void setup()
{
  Serial.begin(115200);

  delay(300);

  Serial.println("\n[BOOT] Real-time Gateway v3.0");
  Serial.printf("[CFG] Queue=%d Batch=%d MinInterval=%dms\n",
                MAX_QUEUE_SIZE, BATCH_SIZE, MIN_INTERVAL_MS);

  memset(nodeStats, 0, sizeof(nodeStats));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  delay(200);

  WiFi.begin(ssid, pass);

  Serial.print("[WIFI] Connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000)
  {
    Serial.print(".");
    delay(300);
  }

  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WIFI] Failed");
    ESP.restart();
  }

  Serial.printf("[WIFI] IP=%s CH=%d\n", WiFi.localIP().toString().c_str(), WiFi.channel());

  client.setInsecure();
  client.setTimeout(5000);

  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("[ESPNOW] Init failed");
    ESP.restart();
  }

  esp_now_register_recv_cb(onRecv);

  Serial.println("[GW] Ready - Real-time mode");
}

void loop()
{
  static unsigned long lastBatchTime = 0;
  static DynamicJsonDocument batchDoc(4096);
  static bool batchInit = false;

  // Initialize batch
  if (!batchInit)
  {
    batchDoc.clear();
    batchDoc["gateway_id"] = GATEWAY_ID;
    batchDoc["data"] = batchDoc.createNestedArray();
    batchInit = true;
    lastBatchTime = millis();
  }

  // Collect packets into batch
  while (!rxQueue.empty() && batchDoc["data"].size() < BATCH_SIZE)
  {
    rx_t r = rxQueue.front();
    rxQueue.pop();

    char macStr[18];
    macToStr(r.mac, macStr);

    JsonObject item = batchDoc["data"].createNestedObject();
    item["node_id"] = r.pkt.node_id;
    item["seq"] = r.pkt.seq;
    item["sender_mac"] = macStr;
    item["timestamp"] = r.timestamp;

    bool gpsValid = (r.pkt.flags & 0x01);
    if (gpsValid)
    {
      item["latitude"] = from_e7(r.pkt.lat_e7);
      item["longitude"] = from_e7(r.pkt.lon_e7);
      item["type"] = "update";
    }
    else
    {
      item["type"] = "heartbeat";
    }

    totalProcessed++;
  }

  // Send batch if ready
  bool shouldSend = false;
  if (batchDoc["data"].size() >= BATCH_SIZE)
  {
    shouldSend = true;
  }
  else if (batchDoc["data"].size() > 0 && (millis() - lastBatchTime) > BATCH_TIMEOUT_MS)
  {
    shouldSend = true;
  }

  if (shouldSend)
  {
    Serial.printf("[BATCH] Sending %d items...\n", batchDoc["data"].size());

    if (!httpPostBatch("/batch", batchDoc))
    {
      Serial.println("[BATCH] Retry once...");
      delay(100);
      httpPostBatch("/batch", batchDoc);
    }

    // Reset batch
    batchInit = false;
  }

  // Stats report
  if (millis() - lastStatsReport > STATS_INTERVAL)
  {
    printStats();
    lastStatsReport = millis();
  }

  // WiFi watchdog
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 5000)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("[WIFI] Reconnecting...");
      WiFi.reconnect();
    }
    lastWiFiCheck = millis();
  }

  // Minimal delay
  if (rxQueue.empty())
  {
    delay(1);
  }
}