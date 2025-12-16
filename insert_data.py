import serial
import json
import requests
import time

# Konfigurasi port COM dan server
SERIAL_PORT = 'COM5'  # Ganti dengan port yang sesuai di sistem Anda
BAUD_RATE = 115200
API_URL = 'http://192.168.103.174:8080/api/update'  # URL API server

# Fungsi untuk mengirim data ke server
def send_data_to_server(latitude, longitude):
    # Format data yang akan dikirim
    data = {
        "node_id": 1,
        "latitude": latitude,
        "longitude": longitude
    }

    # Kirim data ke server menggunakan HTTP POST
    try:
        response = requests.post(API_URL, json=data)
        if response.status_code == 200:
            print("Data berhasil dikirim ke server:", data)
        else:
            print(f"Error mengirim data ke server. Status code: {response.status_code}")
    except requests.exceptions.RequestException as e:
        print(f"Error saat menghubungi server: {e}")

# Fungsi untuk membaca data GPS dari port serial
def read_serial_data():
    # Inisialisasi koneksi serial
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE)

    # Membaca data terus-menerus
    while True:
        if ser.in_waiting > 0:
            # Membaca data dari serial port
            line = ser.readline().decode('utf-8').strip()

            # Menampilkan data yang diterima
            print(f"Data diterima: {line}")

            try:
                # Parsing JSON
                data = json.loads(line)

                # Jika data berisi latitude dan longitude
                if "latitude" in data and "longitude" in data:
                    latitude = data["latitude"]
                    longitude = data["longitude"]

                    # Kirim data GPS ke server
                    send_data_to_server(latitude, longitude)
            except json.JSONDecodeError:
                print("Data yang diterima bukan JSON yang valid")
        
        time.sleep(1)  # Delay 1 detik agar tidak membanjiri CPU

if __name__ == '__main__':
    read_serial_data()
