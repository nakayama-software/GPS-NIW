// app.js (UPDATED) - supports new format + smooth rendering
const socket = io();

// ===== Configuration =====
let roomWidth = 16.2;
let roomHeight = 9.0;

let currentPosition = { x: 0, y: 0 };
let renderPos = { x: 0, y: 0 };
const RENDER_ALPHA = 0.18; // visual smoothing (0.12-0.25)

let lastOk = false;
let lastBeaconCount = 0;
let beaconData = []; // from debug only

// ===== Canvas Setup =====
const canvas = document.getElementById('map-canvas');
const ctx = canvas.getContext('2d');

function resizeCanvas() {
  const container = canvas.parentElement;
  canvas.width = container.clientWidth - 50;
  canvas.height = 500;
}
resizeCanvas();
window.addEventListener('resize', resizeCanvas);

// ===== Statistics =====
let totalUpdates = 0;
let successfulUpdates = 0;
let lastUpdateTime = 0;
let updateTimes = [];

// ===== Socket Events =====
socket.on('connect', () => {
  document.getElementById('connection-status').textContent = 'Connected';
  document.getElementById('connection-status').className = 'status-badge connected';
});

socket.on('disconnect', () => {
  document.getElementById('connection-status').textContent = 'Disconnected';
  document.getElementById('connection-status').className = 'status-badge disconnected';
});

// Raw: config/ready/debug
socket.on('position-update', (data) => {
  if (!data) return;

  if (data.type === 'config') {
    roomWidth = data.roomWidth ?? roomWidth;
    roomHeight = data.roomHeight ?? roomHeight;
    return;
  }

  if (data.type === 'ready') return;

  // debug beacon data: {type:"debug", b:[...]}
  if (data.type === 'debug' && Array.isArray(data.b)) {
    beaconData = data.b.map((b, idx) => ({
      id: b.id ?? (idx + 1),
      detected: !!b.fresh,
      rssi: b.r,
      distance: b.d,
      age: b.age,
      x: b.x,
      y: b.y
    }));

    updateBeaconList();
    drawMap();
    updateStatistics();
  }
});

// Normalized position frames
socket.on('position-update-normalized', (data) => {
  if (!data || typeof data.ok !== 'boolean') return;

  totalUpdates++;
  lastOk = data.ok;
  lastBeaconCount = data.n ?? 0;

  if (data.ok) {
    successfulUpdates++;
    currentPosition = { x: data.x ?? 0, y: data.y ?? 0 };

    // Visual smoothing (LERP)
    renderPos.x = renderPos.x + (currentPosition.x - renderPos.x) * RENDER_ALPHA;
    renderPos.y = renderPos.y + (currentPosition.y - renderPos.y) * RENDER_ALPHA;
  }

  // Update rate
  const now = Date.now();
  if (lastUpdateTime > 0) {
    const interval = now - lastUpdateTime;
    updateTimes.push(interval);
    if (updateTimes.length > 20) updateTimes.shift();
  }
  lastUpdateTime = now;

  updateDisplay();
  drawMap();
  updateStatistics();
});

function updateDisplay() {
  const now = new Date();
  document.getElementById('last-update').textContent = `Last update: ${now.toLocaleTimeString()}`;

  if (lastOk) {
    document.getElementById('position-display').textContent =
      `X: ${currentPosition.x.toFixed(2)}m, Y: ${currentPosition.y.toFixed(2)}m`;
    document.getElementById('position-display').style.color = '#10B981';
  } else {
    document.getElementById('position-display').textContent = 'No position fix';
    document.getElementById('position-display').style.color = '#EF4444';
  }

  // If debug not present, we still show n/4 from position frames
  document.getElementById('beacons-detected').textContent = `${lastBeaconCount}/4`;
}

function drawMap() {
  const padding = 50;
  const mapWidth = canvas.width - padding * 2;
  const mapHeight = canvas.height - padding * 2;

  const scaleX = mapWidth / roomWidth;
  const scaleY = mapHeight / roomHeight;
  const scale = Math.min(scaleX, scaleY);

  ctx.clearRect(0, 0, canvas.width, canvas.height);

  // Room
  ctx.strokeStyle = '#667EEA';
  ctx.lineWidth = 3;
  ctx.strokeRect(padding, padding, roomWidth * scale, roomHeight * scale);

  // Grid
  ctx.strokeStyle = '#E5E7EB';
  ctx.lineWidth = 1;
  for (let i = 0; i <= roomWidth; i += 2) {
    const x = padding + i * scale;
    ctx.beginPath();
    ctx.moveTo(x, padding);
    ctx.lineTo(x, padding + roomHeight * scale);
    ctx.stroke();
  }
  for (let i = 0; i <= roomHeight; i += 2) {
    const y = padding + i * scale;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(padding + roomWidth * scale, y);
    ctx.stroke();
  }

  // Beacons
  beaconData.forEach((beacon) => {
    const x = padding + beacon.x * scale;
    const y = padding + beacon.y * scale;

    ctx.beginPath();
    ctx.arc(x, y, 8, 0, Math.PI * 2);
    ctx.fillStyle = beacon.detected ? '#10B981' : '#EF4444';
    ctx.fill();
    ctx.strokeStyle = 'white';
    ctx.lineWidth = 2;
    ctx.stroke();

    ctx.fillStyle = '#333';
    ctx.font = 'bold 12px Arial';
    ctx.textAlign = 'center';
    ctx.fillText(`B${beacon.id}`, x, y - 15);

    if (beacon.detected && beacon.distance) {
      ctx.beginPath();
      ctx.arc(x, y, beacon.distance * scale, 0, Math.PI * 2);
      ctx.strokeStyle = 'rgba(16, 185, 129, 0.3)';
      ctx.lineWidth = 1;
      ctx.stroke();
    }
  });

  // Forklift (use renderPos for smoother)
  const px = renderPos.x;
  const py = renderPos.y;

  if (px > 0 || py > 0) {
    const x = padding + px * scale;
    const y = padding + py * scale;

    ctx.save();
    ctx.translate(x, y);
    ctx.shadowColor = 'rgba(0, 0, 0, 0.3)';
    ctx.shadowBlur = 10;
    ctx.shadowOffsetX = 2;
    ctx.shadowOffsetY = 2;

    ctx.beginPath();
    ctx.moveTo(0, -15);
    ctx.lineTo(-10, 10);
    ctx.lineTo(10, 10);
    ctx.closePath();
    ctx.fillStyle = '#667EEA';
    ctx.fill();
    ctx.restore();

    ctx.fillStyle = '#667EEA';
    ctx.font = 'bold 14px Arial';
    ctx.textAlign = 'center';
    ctx.fillText(`(${px.toFixed(1)}, ${py.toFixed(1)})`, x, y + 30);
  }

  // Labels
  ctx.fillStyle = '#666';
  ctx.font = '12px Arial';
  ctx.textAlign = 'center';
  ctx.fillText('0,0', padding - 20, padding + roomHeight * scale + 20);
  ctx.fillText(`${roomWidth},${roomHeight}`, padding + roomWidth * scale + 20, padding - 10);
}

function updateBeaconList() {
  const container = document.getElementById('beacon-list');
  container.innerHTML = '';

  beaconData.forEach((beacon) => {
    const item = document.createElement('div');
    item.className = `beacon-item ${beacon.detected ? 'active' : 'inactive'}`;

    item.innerHTML = `
      <div class="beacon-header">
        <span class="beacon-id">Beacon ${beacon.id}</span>
        <span class="beacon-status ${beacon.detected ? 'active' : 'inactive'}">
          ${beacon.detected ? 'ACTIVE' : 'OFFLINE'}
        </span>
      </div>
      <div class="beacon-details">
        <div>Position: (${beacon.x}m, ${beacon.y}m)</div>
        ${beacon.detected ? `
          <div>RSSI: ${beacon.rssi} dBm</div>
          <div>Distance: ${Number(beacon.distance).toFixed(2)}m</div>
          <div>Age: ${beacon.age} ms</div>
        ` : '<div>Not detected</div>'}
      </div>
    `;
    container.appendChild(item);
  });
}

function updateStatistics() {
  const successRate = totalUpdates > 0
    ? ((successfulUpdates / totalUpdates) * 100).toFixed(1)
    : '0.0';
  document.getElementById('success-rate').textContent = `${successRate}%`;

  if (updateTimes.length > 0) {
    const avgInterval = updateTimes.reduce((a, b) => a + b, 0) / updateTimes.length;
    document.getElementById('update-rate').textContent = `${(1000 / avgInterval).toFixed(1)} Hz`;
  }

  const detectedCount = lastBeaconCount || beaconData.filter(b => b.detected).length;
  let accuracy = 'Poor';
  if (detectedCount >= 4) accuracy = 'Excellent';
  else if (detectedCount >= 3) accuracy = 'Good';
  else if (detectedCount >= 2) accuracy = 'Fair';
  document.getElementById('accuracy-value').textContent = accuracy;
}

// ===== Initial draw =====
drawMap();



