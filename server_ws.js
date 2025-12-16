const express = require('express');
const http = require('http');
const mysql = require('mysql2');
const { SerialPort } = require('serialport');
const path = require('path');
const { ReadlineParser } = require('@serialport/parser-readline');

const app = express();
const server = http.createServer(app);

// Middleware for json PARSE
app.use(express.json());

// CREATE TABLE gps_data (
//   node_id INT PRIMARY KEY,
//   latitude DOUBLE NOT NULL,
//   longitude DOUBLE NOT NULL,
//   timestamp DATETIME NOT NULL
// );

// database setup connection
const db = mysql.createConnection({
  host: 'localhost',
  user: 'root',
  password: '',
  database: 'gps_niw'
});

// Check database connection
db.connect((err) => {
  if (err) {
    console.error('Error connecting to the database: ', err.stack);
    return;
  }
  console.log('Connected to the database');
});

// ENDPOINT
app.post('/api/update', (req, res) => {
  const { node_id, latitude, longitude } = req.body;

  if (!node_id || latitude === undefined || longitude === undefined) {
    return res.status(400).json({ error: 'Missing required fields (node_id, latitude, longitude)' });
  }

  const query = `
    REPLACE INTO gps_data (node_id, latitude, longitude, timestamp) 
    VALUES (?, ?, ?, ?)`;

  const timestamp = new Date().toISOString();

  db.query(query, [node_id, latitude, longitude, timestamp], (err, result) => {
    if (err) {
      console.error('Error executing query: ', err.stack);
      return res.status(500).json({ error: 'Failed to update data' });
    }

    console.log(`Data updated for node ${node_id}: Latitude ${latitude}, Longitude ${longitude}`);
    res.json({ message: 'Data updated successfully', data: { node_id, latitude, longitude, timestamp } });
  });
});

app.get('/api/data/:node_id', (req, res) => {
  const node_id = req.params.node_id;

  const query = 'SELECT * FROM gps_data WHERE node_id = ?';

  db.query(query, [node_id], (err, results) => {
    if (err) {
      console.error('Error executing query: ', err.stack);
      return res.status(500).json({ error: 'Failed to retrieve data' });
    }

    if (results.length === 0) {
      return res.status(404).json({ error: `Data for node_id ${node_id} not found` });
    }

    const data = results[0];
    res.json({ node_id, latitude: data.latitude, longitude: data.longitude, timestamp: data.timestamp });
  });
});

app.get('/api/data', (req, res) => {
  const query = 'SELECT * FROM gps_data ORDER BY node_id';

  db.query(query, (err, results) => {
    if (err) {
      console.error('Error executing query: ', err.stack);
      return res.status(500).json({ error: 'Failed to retrieve data' });
    }

    res.json({ count: results.length, data: results });
  });
});

// Serve static files (HTML, CSS, JS)
app.use(express.static('public'));

// Start server
const PORT = 8080;
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Server started on http://localhost:${PORT}`);
});

// Serial Port Configuration
const port = new SerialPort({ path: 'COM5', baudRate: 115200 });

// Parser with newline delimiter
const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));

port.on('open', () => {
  console.log("Serial port opened successfully");
});

port.on('error', (err) => {
  console.error('Serial port error:', err);
});

// Process parsed lines
parser.on('data', (line) => {
  // Extract JSON from the line
  // Format: "Received from 1082953869 msg={"node":22,"latitude":33.210873833,"longitude":130.045915667}"
  const msgIndex = line.indexOf('msg=');
  
  if (msgIndex !== -1) {
    const jsonString = line.substring(msgIndex + 4).trim();
    
    try {
      const jsonData = JSON.parse(jsonString);
      
      // Check if it's GPS data (has node, latitude, longitude)
      if (jsonData.node && jsonData.latitude !== undefined && jsonData.longitude !== undefined) {
        console.log('GPS Data received:', jsonData);
        updateNodeData(jsonData.node, jsonData.latitude, jsonData.longitude);
      } else if (jsonData.status) {
        // This is status data from extender
        console.log('Status message:', jsonData.status);
      }
    } catch (err) {
      console.error('Error parsing JSON:', err);
      console.error('Problematic string:', jsonString);
    }
  }
});

// Update data based on ID function
function updateNodeData(node_id, latitude, longitude) {
  const query = `
    REPLACE INTO gps_data (node_id, latitude, longitude, timestamp) 
    VALUES (?, ?, ?, ?)`;

  const timestamp = new Date().toISOString();

  db.query(query, [node_id, latitude, longitude, timestamp], (err, result) => {
    if (err) {
      console.error('Error updating database:', err.stack);
      return;
    }

    console.log(`✓ Node ${node_id} updated - Lat: ${latitude}, Lng: ${longitude}`);
  });
}