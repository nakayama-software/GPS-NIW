

// ===== Config =====
const ONLINE_WITHIN_SEC = 60;
const GPS_STALE_SEC = 30;
const POLYLINE_MAX_POINTS = 50;
const ID_AREA = 1;

const API_BASE = "https://projects.nakayamairon.com/ncs85283278/NIW_GPS/api/index.php";

// Initialize map
const map = L.map('map').setView([33.2109, 130.0459], 15);

L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    attribution: '© OpenStreetMap',
    maxZoom: 19
}).addTo(map);

const nodeColors = [
    '#2563eb', '#dc2626', '#059669', '#d97706', '#7c3aed',
    '#db2777', '#0891b2', '#ea580c', '#0284c7', '#65a30d'
];

const nodes = {};

// Area drawing state
let isDrawingArea = false;
let areaPoints = [];
let tempMarkers = [];
let tempPolyline = null;
let savedAreas = [];
let selectedColor = '#3b82f6';

// Toggle panel for mobile
const toggleBtn = document.getElementById('toggleBtn');
const bottomPanel = document.getElementById('bottomPanel');
const toggleText = document.getElementById('toggleText');
let panelVisible = true;

toggleBtn.addEventListener('click', () => {
    panelVisible = !panelVisible;
    bottomPanel.classList.toggle('collapsed');
    toggleText.textContent = panelVisible ? 'パネルを隠す' : 'パネルを表示';

    setTimeout(() => {
        map.invalidateSize();
    }, 300);
});

// Area creation functionality
const createAreaBtn = document.getElementById('createAreaBtn');
const finishAreaBtn = document.getElementById('finishAreaBtn');
const cancelAreaBtn = document.getElementById('cancelAreaBtn');
const areaModal = document.getElementById('areaModal');
const modalCancelBtn = document.getElementById('modalCancelBtn');
const modalSaveBtn = document.getElementById('modalSaveBtn');
const areaNameInput = document.getElementById('areaNameInput');
const colorPicker = document.getElementById('colorPicker');
const areaListEl = document.getElementById('areaList');

// Color picker
colorPicker.addEventListener('click', (e) => {
    if (e.target.classList.contains('area-color-option')) {
        document.querySelectorAll('.area-color-option').forEach(opt => opt.classList.remove('selected'));
        e.target.classList.add('selected');
        selectedColor = e.target.dataset.color;
    }
});

// Start area creation
createAreaBtn.addEventListener('click', () => {
    isDrawingArea = true;
    areaPoints = [];
    createAreaBtn.style.display = 'none';
    finishAreaBtn.style.display = 'block';
    cancelAreaBtn.style.display = 'block';
    finishAreaBtn.disabled = true;
    map.getContainer().style.cursor = 'crosshair';
});

// Cancel area creation
function cancelAreaDrawing() {
    isDrawingArea = false;
    areaPoints = [];
    clearTempMarkers();
    createAreaBtn.style.display = 'block';
    finishAreaBtn.style.display = 'none';
    cancelAreaBtn.style.display = 'none';
    map.getContainer().style.cursor = '';
}

cancelAreaBtn.addEventListener('click', cancelAreaDrawing);

// Clear temporary markers
function clearTempMarkers() {
    tempMarkers.forEach(marker => map.removeLayer(marker));
    tempMarkers = [];
    if (tempPolyline) {
        map.removeLayer(tempPolyline);
        tempPolyline = null;
    }
}

// Map click for area creation
map.on('click', (e) => {
    if (!isDrawingArea) return;

    const { lat, lng } = e.latlng;
    areaPoints.push([lat, lng]);

    // Add marker
    const marker = L.circleMarker([lat, lng], {
        radius: 6,
        fillColor: '#2c5282',
        color: 'white',
        weight: 2,
        fillOpacity: 1
    }).addTo(map);
    tempMarkers.push(marker);

    // Update polyline
    if (tempPolyline) map.removeLayer(tempPolyline);
    if (areaPoints.length > 1) {
        tempPolyline = L.polyline(areaPoints, {
            color: '#2c5282',
            weight: 3,
            opacity: 0.7,
            dashArray: '10, 5'
        }).addTo(map);
    }

    // Enable finish button after 3 points
    if (areaPoints.length >= 3) {
        finishAreaBtn.disabled = false;
    }
});

// Finish area creation
finishAreaBtn.addEventListener('click', () => {
    if (areaPoints.length < 3) return;
    areaModal.classList.add('show');
    areaNameInput.value = '';
    areaNameInput.focus();
});

// Modal cancel
modalCancelBtn.addEventListener('click', () => {
    areaModal.classList.remove('show');
});

// Modal save
modalSaveBtn.addEventListener('click', async () => {
    const name = areaNameInput.value.trim();
    if (!name) {
        alert('エリア名を入力してください');
        return;
    }

    const payload = {
        id_area: ID_AREA,
        name,
        color: selectedColor,
        points: [...areaPoints], // [[lat,lng],...]
    };

    try {
        const res = await fetch(`${API_BASE}/areas`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload),
        });
        if (!res.ok) throw new Error('Failed POST /areas');
        const out = await res.json();

        const area = {
            id: out.id,               // ID dari DB
            name,
            color: selectedColor,
            points: [...areaPoints],
        };

        savedAreas.push(area);

        const polygon = L.polygon(area.points, {
            color: area.color,
            fillColor: area.color,
            fillOpacity: 0.2,
            weight: 2
        }).addTo(map);

        polygon.bindPopup(`<b>${area.name}</b><br>${area.points.length} ポイント`);
        area.polygon = polygon;

        updateAreaList();
        clearTempMarkers();
        areaModal.classList.remove('show');
        cancelAreaDrawing();
    } catch (e) {
        console.log(e);
        alert('保存に失敗しました');
    }
});


async function loadAreas() {
    try {
        const res = await fetch(`${API_BASE}/areas?id_area=${ID_AREA}`);
        if (!res.ok) throw new Error('Failed GET /areas');
        const out = await res.json();

        // bersihkan polygon lama jika ada
        savedAreas.forEach(a => a.polygon && map.removeLayer(a.polygon));
        savedAreas = [];

        (out.data || []).forEach(row => {
            const area = {
                id: Number(row.id),
                name: row.name,
                color: row.color || '#3b82f6',
                points: row.points || [],
            };

            const polygon = L.polygon(area.points, {
                color: area.color,
                fillColor: area.color,
                fillOpacity: 0.2,
                weight: 2
            }).addTo(map);

            polygon.bindPopup(`<b>${area.name}</b><br>${area.points.length} ポイント`);
            area.polygon = polygon;

            savedAreas.push(area);
        });

        updateAreaList();
    } catch (e) {
        console.log('loadAreas error', e);
    }
}



// Update area list
function updateAreaList() {
    if (savedAreas.length === 0) {
        areaListEl.innerHTML = '<div style="font-size:11px;color:#94a3b8;text-align:center;padding:8px;">エリアがありません</div>';
        return;
    }

    areaListEl.innerHTML = savedAreas.map(area => `
        <div class="area-item">
          <div class="area-item-info">
            <div class="area-item-color" style="background:${area.color};"></div>
            <span class="area-item-name">${area.name}</span>
          </div>
          <button class="area-item-delete" onclick="deleteArea(${area.id})">削除</button>
        </div>
      `).join('');
}

function isPointInArea(lat, lon, areaId) {
    const area = savedAreas.find(a => a.id === areaId);
    if (!area || !area.polygon) return false;

    // Gunakan method containsLatLng dari Leaflet polygon
    const latLng = L.latLng(lat, lon);

    // Cek apakah point berada di dalam polygon
    const polyLatLngs = area.polygon.getLatLngs()[0]; // Array of LatLng objects
    let inside = false;

    for (let i = 0, j = polyLatLngs.length - 1; i < polyLatLngs.length; j = i++) {
        const xi = polyLatLngs[i].lat, yi = polyLatLngs[i].lng;
        const xj = polyLatLngs[j].lat, yj = polyLatLngs[j].lng;

        const intersect = ((yi > lon) !== (yj > lon))
            && (lat < (xj - xi) * (lon - yi) / (yj - yi) + xi);
        if (intersect) inside = !inside;
    }

    return inside;
}

function getAreaName(lat, lon) {
    for (const area of savedAreas) {
        if (isPointInArea(lat, lon, area.id)) {
            return area.name; 
        }
    }

    const currentLoc = L.latLng(lat, lon);
    
    const distances = savedAreas.map(area => {
        const center = getAreaCenter(area.points);
        if (!center) return { name: area.name, dist: 999999 };
        
        const dist = map.distance(currentLoc, L.latLng(center.lat, center.lng));
        return { name: area.name, dist: dist };
    });

    distances.sort((a, b) => a.dist - b.dist);

    const nearest = distances[0];
    const secondNearest = distances[1];

    const NEAR_LIMIT = 100;    
    const FAR_LIMIT = 600;    

    if (!nearest) return '-';

    if (nearest.dist <= NEAR_LIMIT) {
        return `${nearest.name} 付近`;
    }

    if (nearest.dist <= FAR_LIMIT && secondNearest && secondNearest.dist <= FAR_LIMIT) {
        return `${nearest.name} と ${secondNearest.name} の間`;
    }

    if (nearest.dist <= FAR_LIMIT) {
        return `${nearest.name} あたり`;
    }

    return 'Di luar area';
}

// Delete area
window.deleteArea = async function (id) {
    const index = savedAreas.findIndex(a => a.id === id);
    if (index === -1) return;

    try {
        const res = await fetch(`${API_BASE}/area_delete/${id}`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            }
        });

        if (!res.ok) throw new Error('Failed DELETE /areas/{id}');

        const area = savedAreas[index];
        if (area.polygon) map.removeLayer(area.polygon);

        savedAreas.splice(index, 1);
        updateAreaList();
    } catch (e) {
        console.log(e);
        alert('削除に失敗しました');
    }
};


function createCustomIcon(nodeId, color) {
    return L.divIcon({
        className: 'custom-marker',
        html: `<div style="
          background:${color};
          width:38px;height:38px;border-radius:6px;
          border:3px solid white;
          box-shadow:0 3px 12px rgba(0,0,0,0.25);
          display:flex;align-items:center;justify-content:center;
          color:white;font-weight:700;font-size:14px;
          font-family:'Noto Sans JP',sans-serif;
        ">${nodeId}</div>`,
        iconSize: [38, 38],
        iconAnchor: [19, 19]
    });
}

function ensureNode(nodeId) {
    if (!nodes[nodeId]) {
        const idx = Object.keys(nodes).length;
        nodes[nodeId] = {
            marker: null,
            polyline: null,
            coordinates: [],
            color: nodeColors[idx % nodeColors.length]
        };
    }
    return nodes[nodeId];
}

function updateNodeMarker(nodeId, lat, lon, popupHtml, isStale) {
    const node = ensureNode(nodeId);

    node.coordinates.push([lat, lon]);
    if (node.coordinates.length > POLYLINE_MAX_POINTS) node.coordinates.shift();

    const color = isStale ? '#94a3b8' : node.color;
    const icon = createCustomIcon(nodeId, color);

    if (node.marker) {
        node.marker.setLatLng([lat, lon]);
        node.marker.setIcon(icon);
    } else {
        node.marker = L.marker([lat, lon], { icon }).addTo(map);
        node.marker.bindPopup(popupHtml);

        const markerCount = Object.values(nodes).filter(n => n.marker).length;
        if (markerCount === 1) map.setView([lat, lon], 15);
    }

    if (node.polyline) {
        node.polyline.setLatLngs(node.coordinates);
        node.polyline.setStyle({ color: color });
    } else {
        node.polyline = L.polyline(node.coordinates, {
            color: color,
            weight: 3,
            opacity: 0.7,
            dashArray: '8, 6'
        }).addTo(map);
    }

    node.marker.getPopup().setContent(popupHtml);
}

function removeNodeMarker(nodeId) {
    const node = nodes[nodeId];
    if (!node) return;
    if (node.marker) { map.removeLayer(node.marker); node.marker = null; }
    if (node.polyline) { map.removeLayer(node.polyline); node.polyline = null; }
    node.coordinates = [];
}

function formatAgeSec(sec) {
    if (sec < 60) return `${sec}秒前`;
    const m = Math.floor(sec / 60);
    const s = sec % 60;
    if (m < 60) return `${m}分${s}秒前`;
    const h = Math.floor(m / 60);
    const min = m % 60;
    return `${h}時間${min}分前`;
}

// Menghitung titik tengah (center) dari array coordinates polygon
function getAreaCenter(points) {
    if (!points || points.length === 0) return null;
    
    let latSum = 0;
    let lngSum = 0;
    
    points.forEach(p => {
        latSum += p[0];
        lngSum += p[1];
    });
    
    return {
        lat: latSum / points.length,
        lng: lngSum / points.length
    };
}

async function fetchNodesAndRender() {
    try {
        const res = await fetch(`${API_BASE}/nodes?within=${ONLINE_WITHIN_SEC}`);
        if (!res.ok) throw new Error('Failed /nodes');
        const payload = await res.json();

        const listEl = document.getElementById('statusList');
        const emptyEl = document.getElementById('emptyState');

        listEl.innerHTML = '';

        const now = Date.now();
        const MAX_LAST_SEEN_MS = 5 * 60 * 1000; // 5 menit dalam milliseconds

        // Filter hanya node yang last_seen < 5 menit
        const filteredData = (payload.data || []).filter(row => {
            const lastSeenTs = row.last_seen ? new Date(row.last_seen).getTime() : null;
            if (!lastSeenTs) return false; // Skip jika tidak ada last_seen

            const lastSeenAge = now - lastSeenTs;
            return lastSeenAge < MAX_LAST_SEEN_MS; // Hanya tampilkan jika < 5 menit
        });

        let total = filteredData.length;
        let onlineCount = 0;
        let noGpsCount = 0;

        if (filteredData.length === 0) {
            emptyEl.style.display = 'block';
            listEl.style.display = 'none';
        } else {
            emptyEl.style.display = 'none';
            listEl.style.display = '';

            filteredData.forEach(row => {
                const nodeId = row.node_id;
                const online = !!row.online;
                if (online) onlineCount++;

                const hasGps = !!row.has_gps;
                const gpsTs = row.gps_timestamp ? new Date(row.gps_timestamp).getTime() : null;
                const lastSeenTs = row.last_seen ? new Date(row.last_seen).getTime() : null;

                const gpsAgeSec = gpsTs ? Math.max(0, Math.floor((now - gpsTs) / 1000)) : null;
                const lastSeenAgeSec = lastSeenTs ? Math.max(0, Math.floor((now - lastSeenTs) / 1000)) : null;

                const lastGateway = row.last_gateway ?? '-';
                const senderMac = row.last_sender_mac ?? '-';

                const gpsStale = hasGps && gpsAgeSec !== null && gpsAgeSec > GPS_STALE_SEC;
                const noGpsOrBad = (!hasGps) || (online && gpsStale);
                if (noGpsOrBad) noGpsCount++;

                const card = document.createElement('div');
                card.className = `card ${online ? 'online' : ''}`;

                // === TAMBAHKAN BAGIAN INI (MULAI) ===
                // Cek apakah node memiliki koordinat GPS
                if (hasGps) {
                    // Ubah kursor jadi telunjuk agar terlihat bisa diklik
                    card.style.cursor = 'pointer';

                    card.onclick = () => {
                        const lat = Number(row.latitude);
                        const lon = Number(row.longitude);

                        // 1. Gerakkan peta ke lokasi node (Zoom level 18)
                        map.flyTo([lat, lon], 19, {
                            animate: true,
                            duration: 1.5 // Durasi animasi dalam detik
                        });

                        // 2. Buka popup marker yang sesuai (Opsional, agar user langsung tahu lokasinya)
                        const nodeData = nodes[nodeId];
                        if (nodeData && nodeData.marker) {
                            nodeData.marker.openPopup();
                        }

                        // 3. Khusus Mobile: Tutup panel otomatis agar peta terlihat
                        if (window.innerWidth < 768 && panelVisible) {
                            toggleBtn.click();
                        }
                    };
                }
                // === TAMBAHKAN BAGIAN INI (SELESAI) ===


                let warningHtml = '';
                if (!hasGps) {
                    warningHtml = '<div class="warning-box"><span>⚠️</span><span class="warning-text">GPS信号を受信していません</span></div>';
                } else if (online && gpsStale) {
                    warningHtml = '<div class="warning-box"><span>⚠️</span><span class="warning-text">GPS情報が古い可能性があります</span></div>';
                }

                card.innerHTML = `
              <div class="card-header">
                <div class="card-title">ノード ${nodeId}</div>
                <div class="status-badge ${online ? 'status-online' : 'status-offline'}">
                  ${online ? '● オンライン' : '○ オフライン'}
                </div>
              </div>
              
              <div class="card-grid">
                <div class="card-row">
                  <span class="card-label">最終確認</span>
                  <span class="card-value time-ago">
                    ${lastSeenAgeSec !== null ? formatAgeSec(lastSeenAgeSec) : '-'}
                  </span>
                </div>
                
                <div class="card-row">
                  <span class="card-label">ゲートウェイ</span>
                  <span class="card-value">${lastGateway}</span>
                </div>
                
                <div class="card-row">
                  <span class="card-label">送信元MAC</span>
                  <span class="card-value mono">${senderMac}</span>
                </div>
                
${hasGps ? `
  <div class="card-row">
    <span class="card-label">GPS座標</span>
    <span class="card-value gps-coords">${Number(row.latitude).toFixed(5)}, ${Number(row.longitude).toFixed(5)}</span>
  </div>
  <div class="card-row">
    <span class="card-label">エリア</span>
    <span class="card-value" style="font-weight:600;color:#2563eb;">${getAreaName(Number(row.latitude), Number(row.longitude))}</span>
  </div>
  <div class="card-row">
    <span class="card-label">GPS更新</span>
    <span class="card-value time-ago">${gpsAgeSec !== null ? formatAgeSec(gpsAgeSec) : '-'}</span>
  </div>
` : ''}
              </div>
              
              ${warningHtml}
            `;

                listEl.appendChild(card);

                if (hasGps) {
                    const lat = Number(row.latitude);
                    const lon = Number(row.longitude);

                    const areaName = getAreaName(lat, lon);

                    const popupHtml = `
  <div style="font-size:13px;line-height:1.7;font-family:'Noto Sans JP',sans-serif;">
    <div style="font-weight:700;font-size:15px;margin-bottom:8px;color:#1a202c;">ノード ${nodeId}</div>
    <div style="margin-bottom:4px;">ステータス: <b>${online ? '● オンライン' : '○ オフライン'}</b></div>
    <div style="margin-bottom:4px;">エリア: <b style="color:#2563eb;">${areaName}</b></div>
    <div style="margin-bottom:4px;">ゲートウェイ: <b>${lastGateway}</b></div>
    <div style="margin-bottom:4px;">送信元MAC: <span class="mono">${senderMac}</span></div>
    <div style="margin-bottom:2px;">緯度: ${lat.toFixed(6)}</div>
    <div style="margin-bottom:8px;">経度: ${lon.toFixed(6)}</div>
    <div style="font-size:11px;color:#64748b;padding-top:6px;border-top:1px solid #e2e8f0;">
      GPS更新: ${row.gps_timestamp ? new Date(row.gps_timestamp).toLocaleString('ja-JP') : '-'}
    </div>
  </div>
`;

                    updateNodeMarker(nodeId, lat, lon, popupHtml, (online && gpsStale));
                } else {
                    removeNodeMarker(nodeId);
                }
            });
        }

        document.getElementById('badgeTotal').textContent = `合計: ${total}`;
        document.getElementById('badgeOnline').textContent = `オンライン: ${onlineCount}`;
        document.getElementById('badgeNoGps').textContent = `GPS無効: ${noGpsCount}`;

    } catch (err) {
        console.log('Fetch nodes error:', err);
        document.getElementById('emptyState').innerHTML = '<div class="empty-state">データの読み込みに失敗しました。再試行中...</div>';
        document.getElementById('emptyState').style.display = 'block';
    }
}

loadAreas();
fetchNodesAndRender();
setInterval(fetchNodesAndRender, 500);


window.addEventListener('resize', () => {
    setTimeout(() => map.invalidateSize(), 150);
});

setTimeout(() => map.invalidateSize(), 300);