// ble_beacon.ino
// Simple BLE beacon broadcaster
// Set MAC address di knownBeacons[] di code forklift

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEBeacon.h>

#define BEACON_UUID "12345678-1234-1234-1234-123456789abc"

// GANTI INI untuk setiap beacon! (1, 2, 3, 4, ...)
#define BEACON_ID 1

BLEAdvertising *pAdvertising;

void setup() {
  Serial.begin(115200);
  Serial.printf("BLE Beacon %d starting...\n", BEACON_ID);
  
  BLEDevice::init("Forklift-Beacon");
  
  BLEServer *pServer = BLEDevice::createServer();
  pAdvertising = pServer->getAdvertising();
  
  BLEBeacon beacon;
  beacon.setManufacturerId(0x4C00); // Apple company ID
  beacon.setProximityUUID(BLEUUID(BEACON_UUID));
  beacon.setMajor(1000 + BEACON_ID);  // Unique ID
  beacon.setMinor(1);
  
  BLEAdvertisementData advData;
  advData.setFlags(0x06);
  std::string beaconData = beacon.getData();
  advData.setManufacturerData(beaconData);
  
  pAdvertising->setAdvertisementData(advData);
  pAdvertising->setAdvertisementType(ADV_TYPE_NONCONN_IND);
  
  // Set TX power untuk range prediction (optional)
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);  // +9dBm
  
  pAdvertising->start();
  
  Serial.println("Beacon started!");
  Serial.printf("MAC: %s\n", BLEDevice::getAddress().toString().c_str());
  Serial.println("Copy MAC address ini ke knownBeacons[] array!");
}

void loop() {
  delay(5000);
  Serial.println("Beacon active...");
}