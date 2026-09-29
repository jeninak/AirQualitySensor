<?php

header("Content-Type: application/json");


// Only allow POST requests.
if ($_SERVER["REQUEST_METHOD"] !== "POST") {
    http_response_code(405);
    echo json_encode([
        "success" => false,
        "error" => "Only POST requests are allowed."
    ]);
    exit;
}


// Read the raw request body.
$input = file_get_contents("php://input");

if ($input === false || trim($input) === "") {
    http_response_code(400);
    echo json_encode([
        "success" => false,
        "error" => "Request body is empty."
    ]);
    exit;
}


// Decode JSON.
$data = json_decode($input, true);

if (!is_array($data)) {
    http_response_code(400);
    echo json_encode([
        "success" => false,
        "error" => "Invalid JSON."
    ]);
    exit;
}


// Required fields from the Thingy:53.
$requiredFields = [
    "temperature",
    "humidity",
    "pressure",
    "iaq",
    "iaq_accuracy",
    "co2",
    "voc",
    "gas_stability"
];


// Check that all required fields exist.
foreach ($requiredFields as $field) {
    if (!array_key_exists($field, $data)) {
        http_response_code(400);
        echo json_encode([
            "success" => false,
            "error" => "Missing field: " . $field
        ]);
        exit;
    }
}


// Check that measurement values are numeric.
$numericFields = [
    "temperature",
    "humidity",
    "pressure",
    "iaq",
    "iaq_accuracy",
    "co2",
    "voc",
    "gas_stability"
];

foreach ($numericFields as $field) {
    if (!is_numeric($data[$field])) {
        http_response_code(400);
        echo json_encode([
            "success" => false,
            "error" => "Field must be numeric: " . $field
        ]);
        exit;
    }
}


// Use the Thingy's timestamp if provided.
// Otherwise, use the server's current Unix timestamp.
$timestamp = isset($data["timestamp"]) && is_numeric($data["timestamp"])
    ? (int)$data["timestamp"]
    : time();


// Create the data that will be stored.
$telemetry = [
    "temperature" => (float)$data["temperature"],
    "humidity" => (float)$data["humidity"],
    "pressure" => (float)$data["pressure"],
    "iaq" => (float)$data["iaq"],
    "iaq_accuracy" => (int)$data["iaq_accuracy"],
    "co2" => (float)$data["co2"],
    "voc" => (float)$data["voc"],
    "gas_stability" => (int)$data["gas_stability"],
    "timestamp" => $timestamp
];


// Write to latest.json.
$file = __DIR__ . "/latest.json";

$json = json_encode(
    $telemetry,
    JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES
);

if ($json === false) {
    http_response_code(500);
    echo json_encode([
        "success" => false,
        "error" => "Failed to encode telemetry data."
    ]);
    exit;
}


// Use a file lock so simultaneous requests do not corrupt the file.
$result = file_put_contents(
    $file,
    $json . PHP_EOL,
    LOCK_EX
);

if ($result === false) {
    http_response_code(500);
    echo json_encode([
        "success" => false,
        "error" => "Failed to write latest.json."
    ]);
    exit;
}


// Everything succeeded.
http_response_code(200);

echo json_encode([
    "success" => true,
    "message" => "Telemetry updated."
]);

?>
