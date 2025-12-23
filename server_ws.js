const express = require('express');
const http = require('http');
const mysql = require('mysql2');

const app = express();
const server = http.createServer(app);

app.use(express.json());

// DB connection
const db = mysql.createConnection({
  host: 'localhost',
  user: 'root',
  password: '',
  database: 'gps_niw'
});

db.connect((err) => {
  if (err) {
    console.error('DB connect error:', err.stack);
    return;
  }
  console.log('Connected to the database');
});

/**
 * Helper: upsert heartbeat (latest only)
 */
function upsertHeartbeat(node_id, cb) {
  const q = `
    INSERT INTO node_status (node_id, last_seen)
    VALUES (?, NOW())
    ON DUPLICATE KEY UPDATE last_seen = NOW();
  `;
  db.query(q, [node_id], cb);
}

/**
 * 1) GPS update (latest only) + update heartbeat
 */
app.post('/api/update', (req, res) => {
  const { node_id, latitude, longitude } = req.body;

  if (node_id === undefined || latitude === undefined || longitude === undefined) {
    return res.status(400).json({ error: 'Missing required fields (node_id, latitude, longitude)' });
  }

  const qGps = `
    INSERT INTO gps_data (node_id, latitude, longitude, \`timestamp\`)
    VALUES (?, ?, ?, NOW())
    ON DUPLICATE KEY UPDATE
      latitude = VALUES(latitude),
      longitude = VALUES(longitude),
      \`timestamp\` = VALUES(\`timestamp\`);
  `;

  db.query(qGps, [node_id, latitude, longitude], (err) => {
    if (err) {
      console.error('DB gps_data error:', err.stack);
      return res.status(500).json({ error: 'Failed to update gps_data' });
    }

    // Sekalian update last_seen (anggap GPS update = node alive)
    upsertHeartbeat(node_id, (err2) => {
      if (err2) {
        console.error('DB node_status error:', err2.stack);
        // GPS sudah tersimpan; heartbeat gagal -> tetap balas ok, tapi kasih warning
        return res.json({ ok: true, warning: 'gps updated but heartbeat failed' });
      }
      res.json({ ok: true });
    });
  });
});

/**
 * 2) Heartbeat only (no GPS)
 */
app.post('/api/heartbeat', (req, res) => {
  const { node_id } = req.body;

  if (node_id === undefined) {
    return res.status(400).json({ error: 'Missing required field (node_id)' });
  }

  upsertHeartbeat(node_id, (err) => {
    if (err) {
      console.error('DB node_status error:', err.stack);
      return res.status(500).json({ error: 'Failed to update node_status' });
    }
    res.json({ ok: true });
  });
});

/**
 * Get one GPS row
 */
app.get('/api/data/:node_id', (req, res) => {
  const node_id = req.params.node_id;
  db.query('SELECT * FROM gps_data WHERE node_id = ?', [node_id], (err, results) => {
    if (err) return res.status(500).json({ error: 'Failed to retrieve data' });
    if (!results.length) return res.status(404).json({ error: `node_id ${node_id} not found` });
    res.json(results[0]);
  });
});

/**
 * Get all GPS rows
 */
app.get('/api/data', (req, res) => {
  db.query('SELECT * FROM gps_data ORDER BY node_id', (err, results) => {
    if (err) return res.status(500).json({ error: 'Failed to retrieve data' });
    res.json({ count: results.length, data: results });
  });
});

/**
 * (Opsional) cek node yang "online" dalam N detik terakhir
 * contoh: /api/online?within=60
 */
app.get('/api/online', (req, res) => {
  const within = parseInt(req.query.within ?? '60', 10); // detik
  const q = `
    SELECT node_id, last_seen
    FROM node_status
    WHERE last_seen >= (NOW() - INTERVAL ? SECOND)
    ORDER BY node_id;
  `;
  db.query(q, [within], (err, results) => {
    if (err) return res.status(500).json({ error: 'Failed to retrieve online nodes' });
    res.json({ within_seconds: within, count: results.length, data: results });
  });
});

app.get('/api/nodes', (req, res) => {
  const within = parseInt(req.query.within ?? '60', 10); // detik online window

  const q = `
    SELECT
      ns.node_id,
      ns.last_seen,
      gd.latitude,
      gd.longitude,
      gd.\`timestamp\` AS gps_timestamp,
      (gd.node_id IS NOT NULL) AS has_gps,
      (ns.last_seen >= (NOW() - INTERVAL ? SECOND)) AS online
    FROM node_status ns
    LEFT JOIN gps_data gd ON gd.node_id = ns.node_id
    ORDER BY ns.node_id;
  `;

  db.query(q, [within], (err, rows) => {
    if (err) {
      console.error('DB /api/nodes error:', err.stack);
      return res.status(500).json({ error: 'Failed to retrieve nodes' });
    }
    res.json({ within_seconds: within, count: rows.length, data: rows });
  });
});

app.use(express.static('public'));

const PORT = 8080;
server.listen(PORT, '0.0.0.0', () => {
  console.log(`Server started on http://localhost:${PORT}`);
});
