#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <TinyGPSPlus.h>
#include <math.h>

// ===== CONFIG =====
#define DEVICE_ID 5
static const uint8_t HOP_CHANNELS[] = { 2, 6, 11 };
static const size_t HOP_COUNT = sizeof(HOP_CHANNELS) / sizeof(HOP_CHANNELS[0]);
static const uint8_t BCAST_ADDR[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ===== IMPROVED FILTERING PARAMS =====
#define ALPHA 0.3  // Exponential smoothing (0.1=lambat, 0.5=cepat)
float smoothedDist[3] = { 0, 0, 0 };

// ===== KALIBRASI PARAMETERS (SESUAIKAN!) =====
#define PATH_LOSS_N 2.5    // 2.0=outdoor terbuka, 3.0=indoor banyak obstacle
#define REF_POWER -59      // RSSI pada jarak 1 meter (kalibrasi!)
#define MIN_DIST 0.5       // Minimum jarak yang masuk akal
#define MAX_DIST 30.0      // Maximum jarak yang masuk akal
#define MAX_DEVIATION 100  // Batas koordinat lokal (meter)

HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  int32_t lat_e7;
  int32_t lon_e7;
  uint8_t flags;
} pkt_t;

static pkt_t pkt;
static uint32_t seqNo = 0;

struct BeaconConfig {
  String mac;
  double lat;
  double lng;
  float x;
  float y;
  float lastDistance;
  int lastRSSI;
  unsigned long lastSeen;
};

BeaconConfig myBeacons[] = {
  { "6c:c8:40:8c:81:86", 33.21111001, 130.04573232, 0, 0, 0, 0, 0 },
  { "78:21:84:e0:e5:6a", 33.21088167, 130.04566862, 0, 0, 0, 0, 0 },
  { "6c:c8:40:8c:90:8e", 33.21105952, 130.04546544, 0, 0, 0, 0, 0 }
};
∏

  // Exponential Moving Average - Lebih responsif dari SMA
  float
  filterDistance(int bIdx, float newDist) {
  // Clamp ke range yang masuk akal
  newDist = constrain(newDist, MIN_DIST, MAX_DIST);

  if (smoothedDist[bIdx] == 0) {
    smoothedDist[bIdx] = newDist;  // Inisialisasi pertama kali
  } else {
    // EMA: smooth = α*new + (1-α)*old
    smoothedDist[bIdx] = ALPHA * newDist + (1 - ALPHA) * smoothedDist[bIdx];
  }
  return smoothedDist[bIdx];
}

class MyCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    String addr = dev.getAddress().toString().c_str();
    for (int i = 0; i < 3; i++) {
      if (addr.equalsIgnoreCase(myBeacons[i].mac)) {
        int rssi = dev.getRSSI();
        myBeacons[i].lastRSSI = rssi;

        // Distance calculation dengan parameter yang bisa disesuaikan
        float rawDist = pow(10.0, (float)(REF_POWER - rssi) / (10.0 * PATH_LOSS_N));
        myBeacons[i].lastDistance = filterDistance(i, rawDist);
        myBeacons[i].lastSeen = millis();
      }
    }
  }
};

void setup() {
  Serial.begin(115200);
  GPSSerial.begin(9600, SERIAL_8N1, 16, 17);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != ESP_OK) ESP.restart();

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  // Hitung koordinat lokal XY dari lat/lng
  for (int i = 0; i < 3; i++) {
    myBeacons[i].y = (myBeacons[i].lat - myBeacons[0].lat) * 111111.0;
    myBeacons[i].x = (myBeacons[i].lng - myBeacons[0].lng) * (111111.0 * cos(myBeacons[0].lat * 0.01745329));
    smoothedDist[i] = 0;
  }

  BLEDevice::init("");

  Serial.println("\n=== HYBRID TRACKER READY ===");
  Serial.printf("Config: N=%.1f, RefPwr=%d, Alpha=%.1f\n", PATH_LOSS_N, REF_POWER, ALPHA);
  Serial.println("Beacon Layout (local XY meters):");
  for (int i = 0; i < 3; i++) {
    Serial.printf("  Beacon %d: (%.2f, %.2f)\n", i + 1, myBeacons[i].x, myBeacons[i].y);
  }
  Serial.println("================================\n");
}

void loop() {
  // BLE Scan
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyCallbacks(), false);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(1, false);
  pBLEScan->stop();

  // Check beacon timeout
  int beaconsFound = 0;
  for (int i = 0; i < 3; i++) {
    if (millis() - myBeacons[i].lastSeen > 4000) {
      myBeacons[i].lastDistance = 0;
      smoothedDist[i] = 0;
    }
    if (myBeacons[i].lastDistance > 0) beaconsFound++;
  }

  // GPS read
  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  bool gpsValid = (gps.location.isValid() && gps.location.age() < 3000);

  double finalLat = 0, finalLon = 0;
  bool locationValid = false;
  uint8_t modeBit = 0;

  // TRILATERATION (jika 3 beacon terdeteksi)
  if (beaconsFound == 3) {
    float x1 = myBeacons[0].x, y1 = myBeacons[0].y, r1 = myBeacons[0].lastDistance;
    float x2 = myBeacons[1].x, y2 = myBeacons[1].y, r2 = myBeacons[1].lastDistance;
    float x3 = myBeacons[2].x, y3 = myBeacons[2].y, r3 = myBeacons[2].lastDistance;

    float A = 2 * x2 - 2 * x1;
    float B = 2 * y2 - 2 * y1;
    float C = r1 * r1 - r2 * r2 - x1 * x1 + x2 * x2 - y1 * y1 + y2 * y2;
    float D = 2 * x3 - 2 * x2;
    float E = 2 * y3 - 2 * y2;
    float F = r2 * r2 - r3 * r3 - x2 * x2 + x3 * x3 - y2 * y2 + y3 * y3;
    float det = (A * E - D * B);

    if (abs(det) > 0.001) {
      float localX = (C * E - F * B) / det;
      float localY = (A * F - D * C) / det;

      // Validasi koordinat lokal
      if (abs(localX) < MAX_DEVIATION && abs(localY) < MAX_DEVIATION) {
        finalLat = myBeacons[0].lat + (localY / 111111.0);
        finalLon = myBeacons[0].lng + (localX / (111111.0 * cos(myBeacons[0].lat * 0.01745329)));
        locationValid = true;
        modeBit = 1;

        Serial.printf("[BLE] XY:(%.2f,%.2f) ", localX, localY);
      } else {
        Serial.printf("[BLE-REJECT] XY:(%.2f,%.2f) > limit ", localX, localY);
      }
    }
  }

  // GPS fallback
  if (!locationValid && gpsValid) {
    finalLat = gps.location.lat();
    finalLon = gps.location.lng();
    locationValid = true;
    modeBit = 0;
  }

  // Prepare packet
  pkt.seq = ++seqNo;
  pkt.node_id = DEVICE_ID;
  if (locationValid) {
    pkt.flags = 0x01 | (modeBit << 1);
    pkt.lat_e7 = (int32_t)round(finalLat * 1e7);
    pkt.lon_e7 = (int32_t)round(finalLon * 1e7);
    Serial.printf("D:%.1fm,%.1fm,%.1fm (RSSI:%d,%d,%d) | %.7f,%.7f\n",
                  myBeacons[0].lastDistance, myBeacons[1].lastDistance, myBeacons[2].lastDistance,
                  myBeacons[0].lastRSSI, myBeacons[1].lastRSSI, myBeacons[2].lastRSSI,
                  finalLat, finalLon);
  } else {
    pkt.flags = 0x00;
    pkt.lat_e7 = 0;
    pkt.lon_e7 = 0;
    Serial.println("[NO FIX]");
  }

  // Channel hopping broadcast
  for (size_t i = 0; i < HOP_COUNT; i++) {
    esp_wifi_set_channel(HOP_CHANNELS[i], WIFI_SECOND_CHAN_NONE);
    delay(10);
    for (int retry = 0; retry < 2; retry++) {
      esp_now_send(BCAST_ADDR, (uint8_t*)&pkt, sizeof(pkt));
      delay(5);
    }
  }

  pBLEScan->clearResults();
  delay(100);
}