// forklift_sender_enhanced.ino
// Based on your original code + optimizations
// VERBOSE logging untuk debugging real-time

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <TinyGPSPlus.h>

#define DEVICE_ID      1

// ===== Adaptive intervals (optional - bisa diset sama semua) =====
#define SEND_INTERVAL_MOVING    500   // 500ms when moving
#define SEND_INTERVAL_STATIONARY 2000 // 2s when stationary  
#define SEND_INTERVAL_NO_GPS     1000 // 1s when no GPS (untuk monitoring)

// ===== Movement detection (optional) =====
#define MOVEMENT_THRESHOLD_METERS 2.0
#define SPEED_THRESHOLD_KMPH      1.0

// ===== Channel hopping =====
static const uint8_t HOP_CHANNELS[] = {1, 6, 11};
static const size_t  HOP_COUNT = sizeof(HOP_CHANNELS) / sizeof(HOP_CHANNELS[0]);
#define HOP_SETTLE_MS  5

static const uint8_t BCAST_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ===== GPS setup =====
static const int GPS_RX_PIN = 16;
static const int GPS_TX_PIN = 17;
static const uint32_t GPS_BAUD = 9600;

HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

// ===== Payload =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t  lat_e7;
  int32_t  lon_e7;
  uint8_t  flags;
} pkt_t;

static pkt_t pkt;
static uint32_t seqNo = 0;

// ===== Last position for movement detection =====
struct {
  double lat;
  double lon;
  bool valid;
} lastPos = {0, 0, false};

// ===== Channel statistics =====
struct {
  uint32_t success;
  uint32_t failed;
} channelStats[3] = {{0,0}, {0,0}, {0,0}};

uint32_t totalPackets = 0;
uint32_t totalSkipped = 0;

// ===== Helper functions =====
static int32_t to_e7(double deg) {
  return (int32_t) llround(deg * 1e7);
}

static double calculateDistance(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = (lat2 - lat1) * DEG_TO_RAD;
  double dLon = (lon2 - lon1) * DEG_TO_RAD;
  
  double a = sin(dLat/2) * sin(dLat/2) +
             cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
             sin(dLon/2) * sin(dLon/2);
  
  double c = 2 * atan2(sqrt(a), sqrt(1-a));
  return R * c;
}

static bool hasMovedSignificantly(double newLat, double newLon) {
  if (!lastPos.valid) return true;
  
  double distance = calculateDistance(lastPos.lat, lastPos.lon, newLat, newLon);
  return (distance >= MOVEMENT_THRESHOLD_METERS);
}

static bool readGpsFix(double &lat, double &lon) {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }

  if (gps.location.isValid() && gps.location.age() < 5000) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    return true;
  }
  return false;
}

// ===== ESP-NOW callback dengan channel tracking =====
uint8_t currentChannel = 0;

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  // Find channel index
  for (size_t i = 0; i < HOP_COUNT; i++) {
    if (HOP_CHANNELS[i] == currentChannel) {
      if (status == ESP_NOW_SEND_SUCCESS) {
        channelStats[i].success++;
        Serial.printf("SEND_CB: CH%u OK\n", currentChannel);
      } else {
        channelStats[i].failed++;
        Serial.printf("SEND_CB: CH%u FAIL\n", currentChannel);
      }
      break;
    }
  }
}

static void setChannel(uint8_t ch) {
  currentChannel = ch;
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  delay(HOP_SETTLE_MS);
}

static void sendBroadcastOnChannel(uint8_t ch) {
  setChannel(ch);
  
  esp_err_t s = esp_now_send(BCAST_ADDR, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[SENDER] CH=%u esp_now_send=%d (seq=%u len=%u)\n",
                ch, (int)s, pkt.seq, (unsigned)sizeof(pkt));
  
  delay(15); // Wait for callback
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n[SENDER] GPS Node Enhanced v1.0");
  
  // GPS init
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[SENDER] GPS UART started");

  // WiFi/ESPNOW init
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  esp_wifi_set_ps(WIFI_PS_NONE);
  setChannel(HOP_CHANNELS[0]);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[SENDER] ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onSent);

  // Add broadcast peer
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;
  peer.encrypt = false;

  esp_err_t e = esp_now_add_peer(&peer);
  Serial.printf("[SENDER] add_peer=%d\n", (int)e);

  pkt.node_id = DEVICE_ID;
  pkt.seq = 0;
  pkt.lat_e7 = 0;
  pkt.lon_e7 = 0;
  pkt.flags = 0;

  Serial.printf("[SENDER] Ready. node_id=%u hopping channels: ", pkt.node_id);
  for (size_t i = 0; i < HOP_COUNT; i++) {
    Serial.printf("%u%s", HOP_CHANNELS[i], (i + 1 < HOP_COUNT) ? "," : "\n");
  }
  
  Serial.println("\n--- Starting GPS tracking ---");
  Serial.println("Format: [GPS status] [SENDER CH=X] [SEND_CB result]");
  Serial.println("GPS OK = valid fix | HB ONLY = no fix/heartbeat");
  Serial.println("Movement detection: send only if moved >2m or speed >1km/h\n");
}

void loop() {
  double lat = 0, lon = 0;
  bool gpsValid = readGpsFix(lat, lon);
  
  pkt.seq = ++seqNo;

  bool shouldSend = true;  // Default: always send (like original)
  uint32_t interval = SEND_INTERVAL_NO_GPS;
  
  // Get GPS info for display
  uint8_t sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  double hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9;
  double speed = gps.speed.isValid() ? gps.speed.kmph() : 0.0;

  if (gpsValid) {
    // Check movement (optional - comment out untuk disable)
    bool moved = hasMovedSignificantly(lat, lon);
    bool moving = (speed > SPEED_THRESHOLD_KMPH);
    
    if (moved || moving) {
      shouldSend = true;
      interval = SEND_INTERVAL_MOVING;
      lastPos.lat = lat;
      lastPos.lon = lon;
      lastPos.valid = true;
    } else {
      // OPTIONAL: Uncomment line dibawah untuk skip non-moving packets
      // shouldSend = false;  
      // totalSkipped++;
      interval = SEND_INTERVAL_STATIONARY;
    }
    
    pkt.flags = 0x01;
    pkt.lat_e7 = to_e7(lat);
    pkt.lon_e7 = to_e7(lon);
    
    Serial.printf("[SENDER] GPS OK  seq=%u lat=%.7f lon=%.7f | sats=%u hdop=%.2f speed=%.1fkm/h %s\n", 
                  pkt.seq, lat, lon, sats, hdop, speed,
                  moving ? "[MOVING]" : moved ? "[MOVED]" : "[STATIONARY]");
  } else {
    pkt.flags = 0x00;
    pkt.lat_e7 = 0;
    pkt.lon_e7 = 0;
    interval = SEND_INTERVAL_NO_GPS;
    
    Serial.printf("[SENDER] HB ONLY seq=%u (gps invalid - sats=%u hdop=%.2f)\n", 
                  pkt.seq, sats, hdop);
  }

  // ===== Channel hopping send =====
  if (shouldSend) {
    for (size_t i = 0; i < HOP_COUNT; i++) {
      sendBroadcastOnChannel(HOP_CHANNELS[i]);
    }
    totalPackets++;
  } else {
    Serial.printf("[SENDER] SKIPPED seq=%u (no significant movement)\n", pkt.seq);
  }

  // Print summary stats every 10 packets
  if (totalPackets % 10 == 0 && totalPackets > 0) {
    Serial.println("\n--- Channel Statistics ---");
    for (size_t i = 0; i < HOP_COUNT; i++) {
      uint32_t total = channelStats[i].success + channelStats[i].failed;
      float rate = total > 0 ? (float)channelStats[i].success / total * 100.0 : 0;
      Serial.printf("CH %2u: OK=%-4u FAIL=%-4u Rate=%.1f%%\n",
                    HOP_CHANNELS[i], channelStats[i].success, 
                    channelStats[i].failed, rate);
    }
    Serial.printf("Total Sent: %u | Skipped: %u\n\n", totalPackets, totalSkipped);
  }

  delay(interval);
}