<?php
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');

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

// ===== connect mysqli with optimizations =====
$conn = new mysqli($host, $user, $pass, $dbname);
if ($conn->connect_error) {
  respond(['error' => 'Database connection failed'], 500);
}
$conn->set_charset('utf8mb4');

// Performance optimizations for MySQL
$conn->query("SET SESSION sql_mode = 'NO_ENGINE_SUBSTITUTION'");
$conn->query("SET SESSION autocommit = 1");

// ===== path normalize =====
$reqPath  = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH) ?? '/';
$basePath = rtrim(dirname($_SERVER['SCRIPT_NAME']), '/');
$path     = substr($reqPath, strlen($basePath));
if ($path === false || $path === '') $path = '/';

if (strpos($path, '/index.php') === 0) {
  $path = substr($path, strlen('/index.php'));
  if ($path === '') $path = '/';
}

$method = $_SERVER['REQUEST_METHOD'];

// ===== health route =====
if ($method === 'GET' && ($path === '/' || $path === '')) {
  respond([
    'ok' => true,
    'version' => '2.0-optimized',
    'routes' => [
      'GET  /nodes?within=60',
      'GET  /online?within=60',
      'GET  /data',
      'GET  /data/{node_id}',
      'POST /heartbeat',
      'POST /update',
      'POST /batch (OPTIMIZED)',
    ]
  ]);
}

// ===== POST /batch (ULTRA-OPTIMIZED VERSION) =====
if ($method === 'POST' && $path === '/batch') {
  $body = readJsonBody();

  if (!isset($body['data']) || !is_array($body['data'])) {
    respond(['error' => 'Missing or invalid "data" array'], 400);
  }

  $gateway_id = isset($body['gateway_id']) ? (string)$body['gateway_id'] : null;
  $items = $body['data'];

  if (count($items) === 0) {
    respond(['ok' => true, 'processed' => 0, 'message' => 'Empty batch']);
  }

  $start_time = microtime(true);
  
  // Separate GPS updates and heartbeats for bulk processing
  $gps_updates = [];
  $heartbeat_updates = [];
  $errors = [];

  // Validate and categorize items
  foreach ($items as $idx => $item) {
    if (!isset($item['node_id'], $item['type'])) {
      $errors[] = "Item $idx: missing node_id or type";
      continue;
    }

    $node_id = (int)$item['node_id'];
    $type = (string)$item['type'];
    $sender_mac = isset($item['sender_mac']) ? (string)$item['sender_mac'] : null;

    if ($type === 'update') {
      if (!isset($item['latitude'], $item['longitude'])) {
        $errors[] = "Item $idx: missing latitude/longitude";
        continue;
      }

      $gps_updates[] = [
        'node_id' => $node_id,
        'lat' => (float)$item['latitude'],
        'lon' => (float)$item['longitude'],
        'sender_mac' => $sender_mac,
      ];
      
      // GPS update also counts as heartbeat
      $heartbeat_updates[$node_id] = $sender_mac;

    } elseif ($type === 'heartbeat') {
      $heartbeat_updates[$node_id] = $sender_mac;
    } else {
      $errors[] = "Item $idx: unknown type '$type'";
    }
  }

  // Start transaction
  $conn->begin_transaction();

  try {
    $processed = 0;

    // ===== BULK GPS UPDATE (single multi-row INSERT) =====
    if (count($gps_updates) > 0) {
      $values = [];
      $params = [];
      $types = '';

      foreach ($gps_updates as $gps) {
        $values[] = "(?, ?, ?, NOW())";
        $params[] = $gps['node_id'];
        $params[] = $gps['lat'];
        $params[] = $gps['lon'];
        $types .= 'idd';
      }

      $sql = "INSERT INTO gps_data (node_id, latitude, longitude, `timestamp`)
              VALUES " . implode(', ', $values) . "
              ON DUPLICATE KEY UPDATE
                latitude = VALUES(latitude),
                longitude = VALUES(longitude),
                `timestamp` = VALUES(`timestamp`)";

      $stmt = $conn->prepare($sql);
      if (!$stmt) {
        throw new Exception("GPS prepare failed: {$conn->error}");
      }

      // Bind all parameters at once
      $stmt->bind_param($types, ...$params);
      
      if (!$stmt->execute()) {
        throw new Exception("GPS execute failed: {$stmt->error}");
      }
      
      $processed += $stmt->affected_rows;
      $stmt->close();
    }

    // ===== BULK HEARTBEAT UPDATE (single multi-row INSERT) =====
    if (count($heartbeat_updates) > 0) {
      $values = [];
      $params = [];
      $types = '';

      foreach ($heartbeat_updates as $node_id => $sender_mac) {
        $values[] = "(?, NOW(), ?, ?)";
        $params[] = $node_id;
        $params[] = $gateway_id;
        $params[] = $sender_mac;
        $types .= 'iss';
      }

      $sql = "INSERT INTO node_status (node_id, last_seen, last_gateway, last_sender_mac)
              VALUES " . implode(', ', $values) . "
              ON DUPLICATE KEY UPDATE
                last_seen = NOW(),
                last_gateway = VALUES(last_gateway),
                last_sender_mac = VALUES(last_sender_mac)";

      $stmt = $conn->prepare($sql);
      if (!$stmt) {
        throw new Exception("Heartbeat prepare failed: {$conn->error}");
      }

      $stmt->bind_param($types, ...$params);
      
      if (!$stmt->execute()) {
        throw new Exception("Heartbeat execute failed: {$stmt->error}");
      }
      
      $stmt->close();
    }

    // Commit transaction
    $conn->commit();

    $processing_time = round((microtime(true) - $start_time) * 1000, 2);

    $response = [
      'ok' => true,
      'processed' => count($gps_updates) + count($heartbeat_updates),
      'gps_updates' => count($gps_updates),
      'heartbeats' => count($heartbeat_updates),
      'total_items' => count($items),
      'processing_time_ms' => $processing_time,
    ];

    if (count($errors) > 0) {
      $response['errors'] = $errors;
      $response['error_count'] = count($errors);
    }

    respond($response);

  } catch (Throwable $e) {
    $conn->rollback();
    respond([
      'error' => 'Batch processing failed',
      'message' => $e->getMessage(),
      'processed' => 0,
    ], 500);
  }
}

// ===== POST /update =====
if ($method === 'POST' && $path === '/update') {
  $body = readJsonBody();

  if (!isset($body['node_id'], $body['latitude'], $body['longitude'])) {
    respond(['error' => 'Missing required fields (node_id, latitude, longitude)'], 400);
  }

  $node_id = (int)$body['node_id'];
  $lat = (float)$body['latitude'];
  $lon = (float)$body['longitude'];
  $gateway_id = isset($body['gateway_id']) ? (string)$body['gateway_id'] : null;
  $sender_mac = isset($body['sender_mac']) ? (string)$body['sender_mac'] : null;

  if (!is_numeric($lat) || !is_numeric($lon)) {
    respond(['error' => 'latitude/longitude must be numeric'], 400);
  }

  $conn->begin_transaction();

  try {
    // GPS update
    $stmt = $conn->prepare(
      "INSERT INTO gps_data (node_id, latitude, longitude, `timestamp`)
       VALUES (?, ?, ?, NOW())
       ON DUPLICATE KEY UPDATE
         latitude = VALUES(latitude),
         longitude = VALUES(longitude),
         `timestamp` = VALUES(`timestamp`)"
    );
    $stmt->bind_param("idd", $node_id, $lat, $lon);
    $stmt->execute();
    $stmt->close();

    // Heartbeat update
    $stmt = $conn->prepare(
      "INSERT INTO node_status (node_id, last_seen, last_gateway, last_sender_mac)
       VALUES (?, NOW(), ?, ?)
       ON DUPLICATE KEY UPDATE
         last_seen = NOW(),
         last_gateway = VALUES(last_gateway),
         last_sender_mac = VALUES(last_sender_mac)"
    );
    $stmt->bind_param("iss", $node_id, $gateway_id, $sender_mac);
    $stmt->execute();
    $stmt->close();

    $conn->commit();
    respond(['ok' => true]);

  } catch (Throwable $e) {
    $conn->rollback();
    respond(['error' => 'Update failed', 'message' => $e->getMessage()], 500);
  }
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
    $stmt = $conn->prepare(
      "INSERT INTO node_status (node_id, last_seen, last_gateway, last_sender_mac)
       VALUES (?, NOW(), ?, ?)
       ON DUPLICATE KEY UPDATE
         last_seen = NOW(),
         last_gateway = VALUES(last_gateway),
         last_sender_mac = VALUES(last_sender_mac)"
    );
    $stmt->bind_param("iss", $node_id, $gateway_id, $sender_mac);
    $stmt->execute();
    $stmt->close();

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

  $stmt = $conn->prepare(
    "SELECT node_id, last_seen, last_gateway, last_sender_mac
     FROM node_status
     WHERE last_seen >= (NOW() - INTERVAL ? SECOND)
     ORDER BY node_id"
  );
  $stmt->bind_param("i", $within);
  $stmt->execute();
  
  $result = $stmt->get_result();
  $rows = $result->fetch_all(MYSQLI_ASSOC);
  $stmt->close();

  respond(['within_seconds' => $within, 'count' => count($rows), 'data' => $rows]);
}

// ===== GET /nodes?within=60 =====
if ($method === 'GET' && $path === '/nodes') {
  $within = isset($_GET['within']) ? (int)$_GET['within'] : 60;
  $within = max(1, min($within, 86400));

  $stmt = $conn->prepare(
    "SELECT
       ns.node_id,
       ns.last_seen,
       ns.last_gateway,
       ns.last_sender_mac,
       gd.latitude,
       gd.longitude,
       gd.`timestamp` AS gps_timestamp,
       (gd.node_id IS NOT NULL) AS has_gps,
       (ns.last_seen >= (NOW() - INTERVAL ? SECOND)) AS online
     FROM node_status ns
     LEFT JOIN gps_data gd ON gd.node_id = ns.node_id
     ORDER BY ns.node_id"
  );
  $stmt->bind_param("i", $within);
  $stmt->execute();
  
  $result = $stmt->get_result();
  $rows = $result->fetch_all(MYSQLI_ASSOC);
  $stmt->close();

  respond(['within_seconds' => $within, 'count' => count($rows), 'data' => $rows]);
}

// ===== GET /areas?id_area=123 =====
if ($method === 'GET' && $path === '/areas') {
  $id_area = isset($_GET['id_area']) ? (int)$_GET['id_area'] : 0;
  if ($id_area <= 0) respond(['error' => 'Missing/invalid id_area'], 400);

  $stmt = $conn->prepare(
    "SELECT id, id_area, name, color, points, created_at, updated_at
     FROM area_shape
     WHERE id_area = ?
     ORDER BY id DESC"
  );
  $stmt->bind_param("i", $id_area);
  $stmt->execute();
  $res = $stmt->get_result();

  $rows = [];
  while ($row = $res->fetch_assoc()) {
    $row['points'] = json_decode($row['points'], true);
    $rows[] = $row;
  }
  $stmt->close();

  respond(['count' => count($rows), 'data' => $rows]);
}

// ===== POST /areas =====
if ($method === 'POST' && $path === '/areas') {
  $body = readJsonBody();

  if (!isset($body['id_area'], $body['name'], $body['points'])) {
    respond(['error' => 'Missing required fields (id_area, name, points)'], 400);
  }

  $id_area = (int)$body['id_area'];
  $name = trim((string)$body['name']);
  $color = isset($body['color']) ? (string)$body['color'] : null;
  $points = $body['points'];

  if ($id_area <= 0) respond(['error' => 'id_area must be > 0'], 400);
  if ($name === '') respond(['error' => 'name is required'], 400);
  if (!is_array($points) || count($points) < 3) {
    respond(['error' => 'points must be an array with >= 3 items'], 400);
  }

  foreach ($points as $p) {
    if (!is_array($p) || count($p) < 2 || !is_numeric($p[0]) || !is_numeric($p[1])) {
      respond(['error' => 'Each point must be [lat, lng] numeric'], 400);
    }
  }

  $pointsJson = json_encode($points, JSON_UNESCAPED_UNICODE);
  if ($pointsJson === false) respond(['error' => 'Failed to encode points'], 400);

  $stmt = $conn->prepare(
    "INSERT INTO area_shape (id_area, name, color, points)
     VALUES (?, ?, ?, CAST(? AS JSON))"
  );
  $stmt->bind_param("isss", $id_area, $name, $color, $pointsJson);

  if (!$stmt->execute()) {
    $stmt->close();
    respond(['error' => 'Failed to insert area_shape'], 500);
  }

  $newId = $stmt->insert_id;
  $stmt->close();

  respond(['ok' => true, 'id' => $newId]);
}

// ===== DELETE /area_delete/{id} =====
if ($method === 'POST' && strpos($path, '/area_delete/') === 0) {
  $parts = explode('/', trim($path, '/'));
  $id_area = isset($parts[1]) ? (int)$parts[1] : 0;
  
  if ($id_area <= 0) respond(['error' => 'Missing/invalid id_area'], 400);

  $stmt = $conn->prepare("DELETE FROM area_shape WHERE id = ?");
  $stmt->bind_param("i", $id_area);
  $stmt->execute();
  $affected = $stmt->affected_rows;
  $stmt->close();

  if ($affected <= 0) respond(['error' => 'Area not found'], 404);
  respond(['ok' => true]);
}

respond(['error' => 'Not found', 'path' => $path], 404);