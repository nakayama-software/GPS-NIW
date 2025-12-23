<?php
header('Content-Type: application/json; charset=utf-8');

function respond($data, int $status = 200): void {
  http_response_code($status);
  echo json_encode($data, JSON_UNESCAPED_UNICODE);
  exit;
}

function readJsonBody(): array {
  $raw = file_get_contents('php://input');
  if (!$raw) return [];
  $data = json_decode($raw, true);
  return is_array($data) ? $data : [];
}

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
  http_response_code(204);
  exit;
}

// ===== DB config =====
$host = 'mysqldb.nakayamairon.com';
$user = 'ncs85283278';
$pass = 'CuMdjlWs';
$dbname = 'ncs85283278_NIW_GPS';

// ===== connect mysqli =====
$conn = new mysqli($host, $user, $pass, $dbname);
if ($conn->connect_error) {
  respond(['error' => 'Database connection failed'], 500);
}
$conn->set_charset('utf8mb4');

// ===== path normalize (support /api/index.php/nodes) =====
$reqPath  = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH) ?? '/';
$basePath = rtrim(dirname($_SERVER['SCRIPT_NAME']), '/'); // e.g. /.../NIW_GPS/api
$path     = substr($reqPath, strlen($basePath));
if ($path === false || $path === '') $path = '/';

// IMPORTANT PATCH: when accessed via /index.php or /index.php/xxx
if (strpos($path, '/index.php') === 0) {
  $path = substr($path, strlen('/index.php'));
  if ($path === '') $path = '/';
}

$method = $_SERVER['REQUEST_METHOD'];

// ===== helper: upsert heartbeat =====
function upsertHeartbeat(mysqli $conn, int $node_id, ?string $gateway_id, ?string $sender_mac): void {
  $q = "INSERT INTO node_status (node_id, last_seen, last_gateway, last_sender_mac)
        VALUES (?, NOW(), ?, ?)
        ON DUPLICATE KEY UPDATE
          last_seen = NOW(),
          last_gateway = VALUES(last_gateway),
          last_sender_mac = VALUES(last_sender_mac)";
  $stmt = $conn->prepare($q);
  if (!$stmt) throw new Exception("Prepare failed: {$conn->error}");

  // null safety: kalau kosong, set null
  $gw = ($gateway_id !== null && $gateway_id !== '') ? $gateway_id : null;
  $sm = ($sender_mac !== null && $sender_mac !== '') ? $sender_mac : null;

  $stmt->bind_param("iss", $node_id, $gw, $sm);
  $stmt->execute();
  $stmt->close();
}

// ===== health route =====
if ($method === 'GET' && ($path === '/' || $path === '')) {
  respond([
    'ok' => true,
    'routes' => [
      'GET  /nodes?within=60',
      'GET  /online?within=60',
      'GET  /data',
      'GET  /data/{node_id}',
      'POST /heartbeat',
      'POST /update',
    ]
  ]);
}

// ===== POST /update =====
if ($method === 'POST' && $path === '/update') {
  $body = readJsonBody();

  if (!isset($body['node_id'], $body['latitude'], $body['longitude'])) {
    respond(['error' => 'Missing required fields (node_id, latitude, longitude)'], 400);
  }

  $node_id = (int)$body['node_id'];
  $lat = $body['latitude'];
  $lon = $body['longitude'];

  // optional fields
  $gateway_id = isset($body['gateway_id']) ? (string)$body['gateway_id'] : null;
  $sender_mac = isset($body['sender_mac']) ? (string)$body['sender_mac'] : null;

  if (!is_numeric($lat) || !is_numeric($lon)) {
    respond(['error' => 'latitude/longitude must be numeric'], 400);
  }

  $q = "INSERT INTO gps_data (node_id, latitude, longitude, `timestamp`)
        VALUES (?, ?, ?, NOW())
        ON DUPLICATE KEY UPDATE
          latitude = VALUES(latitude),
          longitude = VALUES(longitude),
          `timestamp` = VALUES(`timestamp`)";

  $stmt = $conn->prepare($q);
  if (!$stmt) respond(['error' => 'Prepare failed (gps_data)'], 500);

  $latF = (float)$lat;
  $lonF = (float)$lon;
  $stmt->bind_param("idd", $node_id, $latF, $lonF);

  if (!$stmt->execute()) {
    $stmt->close();
    respond(['error' => 'Failed to update gps_data'], 500);
  }
  $stmt->close();

  // GPS update juga dianggap heartbeat + simpan gateway_id
  try {
    upsertHeartbeat($conn, $node_id, $gateway_id, $sender_mac);
  } catch (Throwable $e) {
    respond(['ok' => true, 'warning' => 'gps updated but heartbeat failed']);
  }

  respond(['ok' => true]);
}

// ===== POST /heartbeat =====
if ($method === 'POST' && $path === '/heartbeat') {
  $body = readJsonBody();

  if (!isset($body['node_id'])) {
    respond(['error' => 'Missing required field (node_id)'], 400);
  }

  $node_id = (int)$body['node_id'];
  $gateway_id = isset($body['gateway_id']) ? (string)$body['gateway_id'] : null;
  $sender_mac = isset($body['sender_mac']) ? (string)$body['sender_mac'] : null;

  try {
    upsertHeartbeat($conn, $node_id, $gateway_id, $sender_mac);
    respond(['ok' => true]);
  } catch (Throwable $e) {
    respond(['error' => 'Failed to update node_status'], 500);
  }
}

// ===== GET /data =====
if ($method === 'GET' && $path === '/data') {
  $res = $conn->query("SELECT * FROM gps_data ORDER BY node_id");
  if (!$res) respond(['error' => 'Failed to retrieve data'], 500);

  $rows = $res->fetch_all(MYSQLI_ASSOC);
  $res->free();

  respond(['count' => count($rows), 'data' => $rows]);
}

// ===== GET /data/{node_id} =====
if ($method === 'GET' && preg_match('#^/data/(\d+)$#', $path, $m)) {
  $node_id = (int)$m[1];

  $stmt = $conn->prepare("SELECT * FROM gps_data WHERE node_id = ?");
  if (!$stmt) respond(['error' => 'Prepare failed'], 500);

  $stmt->bind_param("i", $node_id);
  $stmt->execute();

  $result = $stmt->get_result();
  $row = $result ? $result->fetch_assoc() : null;
  $stmt->close();

  if (!$row) respond(['error' => "node_id {$node_id} not found"], 404);
  respond($row);
}

// ===== GET /online?within=60 =====
if ($method === 'GET' && $path === '/online') {
  $within = isset($_GET['within']) ? (int)$_GET['within'] : 60;
  $within = max(1, min($within, 86400));

  $q = "SELECT node_id, last_seen, last_gateway, last_sender_mac
        FROM node_status
        WHERE last_seen >= (NOW() - INTERVAL {$within} SECOND)
        ORDER BY node_id";

  $res = $conn->query($q);
  if (!$res) respond(['error' => 'Failed to retrieve online nodes'], 500);

  $rows = $res->fetch_all(MYSQLI_ASSOC);
  $res->free();

  respond(['within_seconds' => $within, 'count' => count($rows), 'data' => $rows]);
}

// ===== GET /nodes?within=60 =====
if ($method === 'GET' && $path === '/nodes') {
  $within = isset($_GET['within']) ? (int)$_GET['within'] : 60;
  $within = max(1, min($within, 86400));

  $q = "SELECT
          ns.node_id,
          ns.last_seen,
          ns.last_gateway,
          ns.last_sender_mac,
          gd.latitude,
          gd.longitude,
          gd.`timestamp` AS gps_timestamp,
          (gd.node_id IS NOT NULL) AS has_gps,
          (ns.last_seen >= (NOW() - INTERVAL {$within} SECOND)) AS online
        FROM node_status ns
        LEFT JOIN gps_data gd ON gd.node_id = ns.node_id
        ORDER BY ns.node_id";

  $res = $conn->query($q);
  if (!$res) respond(['error' => 'Failed to retrieve nodes'], 500);

  $rows = $res->fetch_all(MYSQLI_ASSOC);
  $res->free();

  respond(['within_seconds' => $within, 'count' => count($rows), 'data' => $rows]);
}

respond(['error' => 'Not found', 'path' => $path], 404);
