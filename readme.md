# 🛰️ Real-Time GPS Tracker

A real-time GPS tracking system that receives location data from ESP32 devices via serial communication and displays them on an interactive web map.

## 📋 Features

- **Real-time GPS tracking** - Live location updates from multiple ESP32 nodes
- **Serial data parsing** - Receives and processes GPS data from COM port
- **Interactive web map** - Visualize node positions using Leaflet.js
- **Multi-node support** - Track multiple GPS nodes simultaneously with color-coded markers
- **Path tracking** - Display movement history for each node
- **MySQL database** - Store latest position for each node (no history, only current data)
- **REST API** - Access GPS data programmatically

## 🏗️ System Architecture

```
ESP32 (GPS Module) → Serial Port (COM5) → Node.js Server → MySQL Database
                                              ↓
                                         Web Interface (Map)
```

## 🚀 Prerequisites

- Node.js (v14 or higher)
- MySQL Server
- Serial port connection to ESP32
- ESP32 devices with GPS modules

## 📦 Installation

### 1. Clone the repository

```bash
git clone <your-repository-url>
cd gps-tracker
```

### 3. Upload code for each ESP

just open each file and upload to the each esp function.

### 2. Install dependencies

```bash
npm install express mysql2 serialport @serialport/parser-readline
```

### 3. Setup MySQL Database

Create a database and table:

```sql
CREATE DATABASE gps_niw;

USE gps_niw;

CREATE TABLE gps_data (
  node_id INT PRIMARY KEY,
  latitude DOUBLE NOT NULL,
  longitude DOUBLE NOT NULL,
  timestamp DATETIME NOT NULL
);
```

### 4. Configure the server

Edit `server.js` to match your setup:

```javascript
// Database configuration
const db = mysql.createConnection({
  host: 'localhost',
  user: 'root',           // Change to your MySQL username
  password: '',           // Change to your MySQL password
  database: 'gps_niw'
});

// Serial port configuration
const port = new SerialPort({ 
  path: 'COM5',           // Change to your esp central COM port
  baudRate: 115200 
});

// Add your node IDs
let nodeIdList = [22];    // Add your node IDs here
```

### 5. Configure the web interface

Edit `public/index.html` to add your node IDs:

```javascript
let nodeIdList = [22, 23, 24]; // Add your node IDs here
```

## 🎯 Usage

### Start the server

```bash
node server.js
```

Expected output:
```
Connected to the database
Server started on http://localhost:8080
Serial port opened successfully
```

### Access the web interface

Open your browser and navigate to:
```
http://localhost:8080
```

## 📡 ESP32 Data Format

The ESP32 should send data via serial in this format:

```
Received from 1082953869 msg={"node":22,"latitude":33.210873833,"longitude":130.045915667}
```

**JSON Structure:**
```json
{
  "node": 22,
  "latitude": 33.210873833,
  "longitude": 130.045915667
}
```

**Status messages** (from extenders):
```
Received from 1082950021 msg={"status":"Extender 2"}
```

## 🔌 API Endpoints

### Get data for specific node

```http
GET /api/data/:node_id
```

**Response:**
```json
{
  "node_id": 22,
  "latitude": 33.210873833,
  "longitude": 130.045915667,
  "timestamp": "2025-12-16T10:30:45.000Z"
}
```

### Get all nodes data

```http
GET /api/data
```

**Response:**
```json
{
  "count": 2,
  "data": [
    {
      "node_id": 22,
      "latitude": 33.210873833,
      "longitude": 130.045915667,
      "timestamp": "2025-12-16T10:30:45.000Z"
    },
    {
      "node_id": 23,
      "latitude": 33.211234567,
      "longitude": 130.046789012,
      "timestamp": "2025-12-16T10:30:46.000Z"
    }
  ]
}
```

### Update node data (external API)

```http
POST /api/update
Content-Type: application/json

{
  "node_id": 22,
  "latitude": 33.210873833,
  "longitude": 130.045915667
}
```

## 🗺️ Map Features

- **Colored markers** - Each node has a unique color with its ID displayed inside
- **Click markers** - Click any marker to see detailed information
- **Movement paths** - Dashed lines show the movement history of each node
- **Auto-refresh** - Data updates every 1 second
- **Responsive design** - Works on desktop and mobile devices

## 📂 Project Structure

```
gps-tracker/
├── server.js              # Main Node.js server
├── public/
│   └── index.html        # Web interface
├── es32 code/
│   └── mesh_central.ino        #code for esp central
│   └── mesh_extender.ino        #code for esp extender
│   └── mesh_node.ino        #code for esp node + gps code 
├── package.json          # Dependencies
└── README.md            # This file
```

## 🔧 Configuration

### Adding Multiple Nodes

1. In `server.js`, the system automatically handles any node ID received from serial
2. In `public/index.html`, add node IDs to display:

```javascript
let nodeIdList = [22, 23, 24, 25]; // Add as many as needed
```

### Changing Update Interval

In `public/index.html`:

```javascript
// Change from 1000ms (1 second) to desired interval
setInterval(fetchAllNodesData, 5000); // 5 seconds
```

### Customizing Node Colors

In `public/index.html`:

```javascript
const nodeColors = [
    '#667eea', // Node 1 color
    '#f59e0b', // Node 2 color
    '#10b981', // Node 3 color
    // Add more colors as needed
];
```

## 🐛 Troubleshooting

### Serial Port Issues

**Error: Port not found**
- Check COM port number in Device Manager (Windows) or `ls /dev/tty*` (Linux/Mac)
- Update `path: 'COM5'` in `server.js`

**Error: Access denied**
- Close other programs using the serial port (Arduino IDE, PuTTY, etc.)
- Run as administrator (Windows)

### Database Issues

**Error: Access denied**
- Check MySQL credentials in `server.js`
- Ensure MySQL server is running

**Error: Table doesn't exist**
- Run the SQL commands in the Installation section

### No Data on Map

1. Check server console for errors
2. Verify ESP32 is sending data (check server logs)
3. Ensure node IDs in `index.html` match ESP32 node IDs
4. Open browser console (F12) to check for JavaScript errors

## 🔐 Security Notes

- This is a development setup with default MySQL credentials
- For production, use environment variables for sensitive data
- Implement proper authentication for API endpoints
- Use HTTPS for web interface

## 📝 License

This project is open source and available under the MIT License.

## 🤝 Contributing

Contributions, issues, and feature requests are welcome!

## 📧 Contact

For questions or support, please open an issue in the repository.

## 🙏 Acknowledgments

- [Leaflet.js](https://leafletjs.com/) - Interactive maps
- [Node.js](https://nodejs.org/) - Server runtime
- [MySQL](https://www.mysql.com/) - Database
- [SerialPort](https://serialport.io/) - Serial communication

---

Made with ❤️ for GPS tracking