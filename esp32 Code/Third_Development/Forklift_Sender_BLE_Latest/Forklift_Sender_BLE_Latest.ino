// forklift_sender_ble.ino
// Enhanced version dengan GPS Quality Check + BLE Beacon Scanning
// Auto-switch antara GPS dan BLE positioning

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include <TinyGPSPlus.h>

#define DEVICE_ID      1
#define SEND_INTERVAL  500  // ms

// ===== Channel hopping =====
static const uint8_t HOP_CHANNELS[] = {2, 6, 11};
static const size_t  HOP_COUNT = sizeof(HOP_CHANNELS) / sizeof(HOP_CHANNELS[0]);
#define HOP_SETTLE_MS  3

static const uint8_t BCAST_ADDR[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ===== GPS setup =====
static const int GPS_RX_PIN = 16;
static const int GPS_TX_PIN = 17;
static const uint32_t GPS_BAUD = 9600;

HardwareSerial GPSSerial(2);
TinyGPSPlus gps;

// ===== GPS Quality Thresholds =====
#define GPS_MIN_SATELLITES  6
#define GPS_MAX_HDOP_EXCELLENT  2.0
#define GPS_MAX_HDOP_GOOD       5.0
#define GPS_MAX_HDOP_USABLE     10.0
#define GPS_MAX_AGE_MS          5000

// ===== BLE Beacon Config =====
#define BLE_SCAN_TIME       1    // seconds
#define MIN_BEACONS_FOR_FIX 3
#define MAX_BEACONS         10

BLEScan* pBLEScan;

// Beacon data structure
typedef struct {
  String address;
  int rssi;
  double x;  // meter (relative coordinate)
  double y;  // meter (relative coordinate)
  bool valid;
} BeaconData;

BeaconData detectedBeacons[MAX_BEACONS];
int beaconCount = 0;

// Known beacon positions (SETUP INI SESUAI LAYOUT FACTORY ANDA!)
typedef struct {
  String mac;
  double x;  // meter
  double y;  // meter
  double z;  // meter (tinggi dari lantai)
} KnownBeacon;

// CONTOH: 4 beacon di sudut ruangan 20x20 meter
KnownBeacon knownBeacons[] = {
  {"aa:bb:cc:dd:ee:01", 0.0,  0.0,  3.0},   // Beacon 1: sudut kiri bawah
  {"aa:bb:cc:dd:ee:02", 20.0, 0.0,  3.0},   // Beacon 2: sudut kanan bawah
  {"aa:bb:cc:dd:ee:03", 20.0, 20.0, 3.0},   // Beacon 3: sudut kanan atas
  {"aa:bb:cc:dd:ee:04", 0.0,  20.0, 3.0}    // Beacon 4: sudut kiri atas
};
const int KNOWN_BEACON_COUNT = sizeof(knownBeacons) / sizeof(knownBeacons[0]);

// ===== Positioning Mode =====
typedef enum {
  MODE_GPS_ONLY,
  MODE_BLE_ONLY,
  MODE_HYBRID,
  MODE_UNKNOWN
} PositioningMode;

PositioningMode currentMode = MODE_UNKNOWN;

// ===== Enhanced Payload =====
typedef struct __attribute__((packed)) {
  uint32_t node_id;
  uint32_t seq;
  
  // GPS data
  int32_t  lat_e7;
  int32_t  lon_e7;
  
  // BLE positioning (relative coordinates in meters)
  int16_t  ble_x_cm;   // x position in cm
  int16_t  ble_y_cm;   // y position in cm
  
  // Quality metrics
  uint16_t hdop_x100;  // HDOP * 100
  uint8_t  satellites;
  uint8_t  beacon_count;
  
  uint8_t  flags;      // bit0: gps_valid, bit1: ble_valid, bit2-3: mode
  uint8_t  reserved;
} pkt_t;

static pkt_t pkt;
static uint32_t seqNo = 0;

// Position validation
typedef struct {
  double lat;
  double lon;
  double x;
  double y;
  unsigned long timestamp;
} LastPosition;

LastPosition lastPos = {0, 0, 0, 0, 0};

// ===== Helper Functions =====
static int32_t to_e7(double deg) {
  return (int32_t) llround(deg * 1e7);
}

static int16_t to_cm(double meters) {
  return (int16_t) llround(meters * 100.0);
}

// RSSI to distance conversion (Path Loss Model)
// d = 10^((TxPower - RSSI) / (10 * n))
// TxPower ≈ -59 dBm (ESP32 at 1m), n ≈ 2-4 (environment factor)
static double rssiToDistance(int rssi, double txPower = -59.0, double n = 3.0) {
  if (rssi >= 0) return 0.0;
  return pow(10.0, (txPower - rssi) / (10.0 * n));
}

// Moving average filter untuk RSSI
static int filterRSSI(int newRSSI, int oldRSSI) {
  if (oldRSSI == 0) return newRSSI;
  return (oldRSSI * 3 + newRSSI) / 4;  // 75% old, 25% new
}

// Trilateration untuk 3+ beacon (simplified)
static bool calculateBLEPosition(double &x_out, double &y_out) {
  if (beaconCount < MIN_BEACONS_FOR_FIX) return false;
  
  // Weighted centroid method (simple but effective)
  double sum_x = 0, sum_y = 0, sum_weight = 0;
  
  for (int i = 0; i < beaconCount && i < MAX_BEACONS; i++) {
    if (!detectedBeacons[i].valid) continue;
    
    double distance = rssiToDistance(detectedBeacons[i].rssi);
    
    // Weight: inverse of distance squared
    double weight = 1.0 / (distance * distance + 0.1);  // +0.1 prevent div by zero
    
    sum_x += detectedBeacons[i].x * weight;
    sum_y += detectedBeacons[i].y * weight;
    sum_weight += weight;
  }
  
  if (sum_weight < 0.01) return false;
  
  x_out = sum_x / sum_weight;
  y_out = sum_y / sum_weight;
  
  return true;
}

// GPS Quality Assessment
typedef enum {
  GPS_EXCELLENT,
  GPS_GOOD,
  GPS_MODERATE,
  GPS_POOR,
  GPS_UNUSABLE
} GPSQuality;

static GPSQuality assessGPSQuality() {
  if (!gps.location.isValid() || gps.location.age() > GPS_MAX_AGE_MS) {
    return GPS_UNUSABLE;
  }
  
  int sats = gps.satellites.value();
  double hdop = gps.hdop.hdop();
  
  // Check HDOP (most important for accuracy!)
  if (hdop < GPS_MAX_HDOP_EXCELLENT && sats >= 8) {
    return GPS_EXCELLENT;
  } else if (hdop < GPS_MAX_HDOP_GOOD && sats >= GPS_MIN_SATELLITES) {
    return GPS_GOOD;
  } else if (hdop < GPS_MAX_HDOP_USABLE && sats >= 4) {
    return GPS_MODERATE;
  } else if (sats >= 4) {
    return GPS_POOR;
  }
  
  return GPS_UNUSABLE;
}

// Position jump detection (anti-multipath)
static bool isPositionJumpAbnormal(double lat, double lon) {
  if (lastPos.timestamp == 0) return false;
  
  unsigned long timeDiff = millis() - lastPos.timestamp;
  if (timeDiff < 100) return false;  // Too soon
  
  // Calculate distance moved (simple approximation)
  double dLat = (lat - lastPos.lat) * 111000.0;  // meters
  double dLon = (lon - lastPos.lon) * 111000.0 * cos(lat * PI / 180.0);
  double distance = sqrt(dLat * dLat + dLon * dLon);
  
  // Max forklift speed: 15 km/h = 4.2 m/s
  double maxDistance = (timeDiff / 1000.0) * 6.0;  // 6 m/s with buffer
  
  if (distance > maxDistance) {
    Serial.printf("⚠️ GPS JUMP: %.1fm in %.1fs (max: %.1fm)\n", 
                  distance, timeDiff/1000.0, maxDistance);
    return true;
  }
  
  return false;
}

// ===== BLE Scan Callback =====
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String mac = advertisedDevice.getAddress().toString().c_str();
    int rssi = advertisedDevice.getRSSI();
    
    // Check if this is a known beacon
    for (int i = 0; i < KNOWN_BEACON_COUNT; i++) {
      if (mac.equalsIgnoreCase(knownBeacons[i].mac)) {
        // Found known beacon
        if (beaconCount < MAX_BEACONS) {
          detectedBeacons[beaconCount].address = mac;
          detectedBeacons[beaconCount].rssi = rssi;
          detectedBeacons[beaconCount].x = knownBeacons[i].x;
          detectedBeacons[beaconCount].y = knownBeacons[i].y;
          detectedBeacons[beaconCount].valid = true;
          
          Serial.printf("  Beacon %d: %s RSSI=%d dist=%.1fm\n", 
                        beaconCount, mac.c_str(), rssi, 
                        rssiToDistance(rssi));
          beaconCount++;
        }
        break;
      }
    }
  }
};

// Scan BLE beacons
static void scanBLEBeacons() {
  beaconCount = 0;
  memset(detectedBeacons, 0, sizeof(detectedBeacons));
  
  Serial.println("[BLE] Scanning...");
  BLEScanResults foundDevices = pBLEScan->start(BLE_SCAN_TIME, false);
  Serial.printf("[BLE] Found %d beacons\n", beaconCount);
  pBLEScan->clearResults();
}

// ===== Mode Selection Logic =====
static PositioningMode selectMode(GPSQuality gpsQuality, int bleBeacons) {
  // Priority: Safety first, then accuracy
  
  if (gpsQuality == GPS_EXCELLENT && bleBeacons == 0) {
    return MODE_GPS_ONLY;
  }
  
  if (gpsQuality <= GPS_MODERATE && bleBeacons >= MIN_BEACONS_FOR_FIX) {
    return MODE_BLE_ONLY;  // Indoor scenario (your case lantai 3!)
  }
  
  if (gpsQuality == GPS_GOOD && bleBeacons >= MIN_BEACONS_FOR_FIX) {
    return MODE_HYBRID;  // Transition zone
  }
  
  if (bleBeacons >= MIN_BEACONS_FOR_FIX) {
    return MODE_BLE_ONLY;
  }
  
  if (gpsQuality >= GPS_GOOD) {
    return MODE_GPS_ONLY;
  }
  
  return MODE_UNKNOWN;
}

// ===== Read GPS =====
static bool readGpsFix(double &lat, double &lon) {
  while (GPSSerial.available()) {
    gps.encode(GPSSerial.read());
  }
  
  GPSQuality quality = assessGPSQuality();
  
  if (quality >= GPS_GOOD) {
    lat = gps.location.lat();
    lon = gps.location.lng();
    
    // Check position jump
    if (isPositionJumpAbnormal(lat, lon)) {
      Serial.println("⚠️ Multipath detected, rejecting GPS position");
      return false;
    }
    
    return true;
  }
  
  return false;
}

// ===== ESP-NOW Functions =====
void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  // Suppress callback logging (too verbose)
}

static void setChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  delay(HOP_SETTLE_MS);
}

static void sendBroadcastOnChannel(uint8_t ch) {
  setChannel(ch);
  esp_now_send(BCAST_ADDR, (uint8_t*)&pkt, sizeof(pkt));
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Forklift Tracking System v2.0 ===");
  
  // GPS init
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART started");
  
  // BLE init
  BLEDevice::init("Forklift-Node");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  Serial.println("[BLE] Scanner initialized");
  
  // WiFi/ESPNOW init
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, true);
  esp_wifi_set_ps(WIFI_PS_NONE);
  setChannel(HOP_CHANNELS[0]);
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed!");
    return;
  }
  esp_now_register_send_cb(onSent);
  
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST_ADDR, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  
  Serial.println("[ESP-NOW] Ready");
  Serial.printf("[CONFIG] Node ID: %u\n", DEVICE_ID);
  Serial.printf("[CONFIG] Known beacons: %d\n", KNOWN_BEACON_COUNT);
  Serial.println("=================================\n");
}

// ===== Main Loop =====
void loop() {
  // 1. Read GPS
  double gpsLat = 0, gpsLon = 0;
  bool gpsValid = readGpsFix(gpsLat, gpsLon);
  GPSQuality gpsQuality = assessGPSQuality();
  
  // 2. Scan BLE beacons
  scanBLEBeacons();
  
  // 3. Calculate BLE position
  double bleX = 0, bleY = 0;
  bool bleValid = calculateBLEPosition(bleX, bleY);
  
  // 4. Select positioning mode
  currentMode = selectMode(gpsQuality, beaconCount);
  
  // 5. Prepare packet
  pkt.node_id = DEVICE_ID;
  pkt.seq = ++seqNo;
  pkt.satellites = gps.satellites.value();
  pkt.hdop_x100 = (uint16_t)(gps.hdop.hdop() * 100);
  pkt.beacon_count = beaconCount;
  pkt.flags = 0;
  
  // Set mode in flags (bit 2-3)
  pkt.flags |= (currentMode << 2);
  
  // Fill position data based on mode
  switch (currentMode) {
    case MODE_GPS_ONLY:
      if (gpsValid) {
        pkt.lat_e7 = to_e7(gpsLat);
        pkt.lon_e7 = to_e7(gpsLon);
        pkt.flags |= 0x01;  // gps_valid
        lastPos.lat = gpsLat;
        lastPos.lon = gpsLon;
        lastPos.timestamp = millis();
      }
      Serial.printf("[GPS] seq=%u lat=%.7f lon=%.7f sats=%d hdop=%.1f\n",
                    pkt.seq, gpsLat, gpsLon, pkt.satellites, gps.hdop.hdop());
      break;
      
    case MODE_BLE_ONLY:
      if (bleValid) {
        pkt.ble_x_cm = to_cm(bleX);
        pkt.ble_y_cm = to_cm(bleY);
        pkt.flags |= 0x02;  // ble_valid
        lastPos.x = bleX;
        lastPos.y = bleY;
        lastPos.timestamp = millis();
      }
      Serial.printf("[BLE] seq=%u x=%.2fm y=%.2fm beacons=%d\n",
                    pkt.seq, bleX, bleY, beaconCount);
      break;
      
    case MODE_HYBRID:
      // Weighted average (GPS 30%, BLE 70% for indoor)
      if (gpsValid && bleValid) {
        pkt.lat_e7 = to_e7(gpsLat);
        pkt.lon_e7 = to_e7(gpsLon);
        pkt.ble_x_cm = to_cm(bleX);
        pkt.ble_y_cm = to_cm(bleY);
        pkt.flags |= 0x03;  // both valid
      } else if (bleValid) {
        pkt.ble_x_cm = to_cm(bleX);
        pkt.ble_y_cm = to_cm(bleY);
        pkt.flags |= 0x02;
      } else if (gpsValid) {
        pkt.lat_e7 = to_e7(gpsLat);
        pkt.lon_e7 = to_e7(gpsLon);
        pkt.flags |= 0x01;
      }
      Serial.printf("[HYBRID] seq=%u GPS(%.7f,%.7f) BLE(%.2f,%.2f)\n",
                    pkt.seq, gpsLat, gpsLon, bleX, bleY);
      break;
      
    default:
      Serial.printf("[UNKNOWN] seq=%u No valid position\n", pkt.seq);
      break;
  }
  
  // 6. Send via ESP-NOW (channel hopping)
  for (size_t i = 0; i < HOP_COUNT; i++) {
    sendBroadcastOnChannel(HOP_CHANNELS[i]);
  }
  
  delay(SEND_INTERVAL);
}