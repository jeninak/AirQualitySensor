const TELEMETRY_URL = "latest.json";

const POLL_INTERVAL_MS = 5000;
const MAX_HISTORY_POINTS = 30;

const state = {
    current: null,

    history: {
        timestamps: [],
        temperature: [],
        humidity: [],
        iaq: [],
        co2: [],
        voc: []
    }
};


/*
 * Get sensor data from the server.
 */
async function getSensorData() {
    const response = await fetch(
        `${TELEMETRY_URL}?t=${Date.now()}`,
        {
            cache: "no-store"
        }
    );

    if (!response.ok) {
        throw new Error(
            `Server returned HTTP ${response.status}`
        );
    }

    const data = await response.json();

    /*
     * Validate the basic telemetry structure.
     */
    const requiredFields = [
        "temperature",
        "humidity",
        "pressure",
        "iaq",
        "iaq_accuracy",
        "co2",
        "voc",
        "gas_stability",
        "timestamp"
    ];

    for (const field of requiredFields) {
        if (!(field in data)) {
            throw new Error(
                `Telemetry is missing field: ${field}`
            );
        }
    }

    /*
     * Convert Unix timestamp to JavaScript Date.
     */
    data.timestamp = new Date(data.timestamp * 1000);

    return data;
}


/*
 * Convert IAQ value into a readable label.
 */
function getIaqLabel(iaq) {
    if (iaq <= 50) {
        return "Excellent";
    }

    if (iaq <= 100) {
        return "Good";
    }

    if (iaq <= 150) {
        return "Lightly polluted";
    }

    if (iaq <= 200) {
        return "Moderately polluted";
    }

    if (iaq <= 250) {
        return "Heavily polluted";
    }

    if (iaq <= 350) {
        return "Severely polluted";
    }

    return "Extremely polluted";
}


/*
 * Convert BSEC accuracy value into a readable label.
 */
function getAccuracyLabel(accuracy) {
    switch (accuracy) {
        case 0:
            return "Initial calibration";

        case 1:
            return "Low accuracy";

        case 2:
            return "Medium accuracy";

        case 3:
            return "High accuracy";

        default:
            return "Unknown";
    }
}


/*
 * Update the online/offline status.
 */
function updateConnectionStatus(online, timestamp = null) {
    const statusDot =
        document.getElementById("statusDot");

    const deviceStatus =
        document.getElementById("deviceStatus");

    const lastUpdate =
        document.getElementById("lastUpdate");

    const connectionText =
        document.getElementById("connectionText");

    const connectionDescription =
        document.getElementById("connectionDescription");

    if (online) {
        statusDot.style.background = "var(--success)";
        statusDot.style.boxShadow =
            "0 0 0 5px var(--success-soft)";

        deviceStatus.textContent = "Online";

        connectionText.textContent = "Connected";

        connectionDescription.textContent =
            "Receiving sensor measurements";

        if (timestamp) {
            lastUpdate.textContent =
                `Last update: ${timestamp.toLocaleTimeString()}`;
        }
    } else {
        statusDot.style.background = "var(--warning)";
        statusDot.style.boxShadow =
            "0 0 0 5px var(--warning-soft)";

        deviceStatus.textContent = "Offline";

        connectionText.textContent = "No data";

        connectionDescription.textContent =
            "Waiting for sensor measurements";

        lastUpdate.textContent =
            "Last update: unavailable";
    }
}


/*
 * Update the measurement cards.
 */
function updateValues(data) {
    document.getElementById("temperature").textContent =
        data.temperature.toFixed(2);

    document.getElementById("humidity").textContent =
        data.humidity.toFixed(2);

    document.getElementById("pressure").textContent =
        data.pressure.toFixed(2);

    document.getElementById("iaq").textContent =
        Math.round(data.iaq);

    document.getElementById("iaqLabel").textContent =
        getIaqLabel(data.iaq);

    document.getElementById("iaqAccuracy").textContent =
        data.iaq_accuracy;

    document.getElementById("accuracyLabel").textContent =
        getAccuracyLabel(data.iaq_accuracy);

    document.getElementById("co2").textContent =
        data.co2.toFixed(1);

    document.getElementById("voc").textContent =
        data.voc.toFixed(2);

    const gasStable =
        data.gas_stability === 1;

    document.getElementById("gasStability").textContent =
        gasStable ? "Stable" : "Not stable";

    document.getElementById("gasLabel").textContent =
        gasStable
            ? "BME688 stable"
            : "BME688 warming up";

    /*
     * Update chart header values.
     */
    document.getElementById("temperatureChartValue").textContent =
        `${data.temperature.toFixed(2)} °C`;

    document.getElementById("humidityChartValue").textContent =
        `${data.humidity.toFixed(2)} %`;

    document.getElementById("iaqChartValue").textContent =
        Math.round(data.iaq);

    document.getElementById("co2ChartValue").textContent =
        `${data.co2.toFixed(1)} ppm`;

    document.getElementById("vocChartValue").textContent =
        `${data.voc.toFixed(2)} ppm`;

    /*
     * Device status section.
     */
    document.getElementById("bsecStatus").textContent =
        getAccuracyLabel(data.iaq_accuracy);

    document.getElementById("bsecDescription").textContent =
        `IAQ accuracy: ${data.iaq_accuracy}`;

    document.getElementById("gasStatus").textContent =
        gasStable ? "Stable" : "Not stable";

    updateConnectionStatus(true, data.timestamp);
}


/*
 * Add a measurement to the history.
 */
function addHistoryPoint(data) {
    state.history.timestamps.push(data.timestamp);

    state.history.temperature.push(
        data.temperature
    );

    state.history.humidity.push(
        data.humidity
    );

    state.history.iaq.push(
        data.iaq
    );

    state.history.co2.push(
        data.co2
    );

    state.history.voc.push(
        data.voc
    );

    while (
        state.history.timestamps.length >
        MAX_HISTORY_POINTS
    ) {
        state.history.timestamps.shift();
        state.history.temperature.shift();
        state.history.humidity.shift();
        state.history.iaq.shift();
        state.history.co2.shift();
        state.history.voc.shift();
    }
}


/*
 * Draw a graph on a canvas.
 */
function drawChart(canvasId, values, label, unit) {
    const canvas =
        document.getElementById(canvasId);

    if (!canvas) {
        return;
    }

    const ctx = canvas.getContext("2d");

    const rect =
        canvas.getBoundingClientRect();

    const width = rect.width;
    const height = rect.height;

    const devicePixelRatio =
        window.devicePixelRatio || 1;

    canvas.width =
        width * devicePixelRatio;

    canvas.height =
        height * devicePixelRatio;

    ctx.setTransform(
        devicePixelRatio,
        0,
        0,
        devicePixelRatio,
        0,
        0
    );

    ctx.clearRect(
        0,
        0,
        width,
        height
    );

    if (values.length < 2) {
        ctx.fillStyle = "#6b7280";
        ctx.font = "14px Arial";
        ctx.fillText(
            "Waiting for data...",
            20,
            30
        );

        return;
    }

    const paddingLeft = 45;
    const paddingRight = 15;
    const paddingTop = 20;
    const paddingBottom = 30;

    const chartWidth =
        width -
        paddingLeft -
        paddingRight;

    const chartHeight =
        height -
        paddingTop -
        paddingBottom;

    let minValue =
        Math.min(...values);

    let maxValue =
        Math.max(...values);

    if (maxValue === minValue) {
        maxValue += 1;
        minValue -= 1;
    }

    const range =
        maxValue - minValue;

    minValue -= range * 0.1;
    maxValue += range * 0.1;

    /*
     * Grid lines.
     */
    ctx.font = "11px Arial";
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";

    const gridLines = 4;

    for (let i = 0; i <= gridLines; i++) {
        const y =
            paddingTop +
            (chartHeight / gridLines) * i;

        const value =
            maxValue -
            ((maxValue - minValue) /
                gridLines) * i;

        ctx.beginPath();

        ctx.moveTo(
            paddingLeft,
            y
        );

        ctx.lineTo(
            width - paddingRight,
            y
        );

        ctx.strokeStyle =
            "#e5e7eb";

        ctx.lineWidth = 1;
        ctx.stroke();

        ctx.fillStyle =
            "#6b7280";

        ctx.fillText(
            value.toFixed(1),
            paddingLeft - 8,
            y
        );
    }

    /*
     * Measurement line.
     */
    ctx.beginPath();

    values.forEach((value, index) => {
        const x =
            paddingLeft +
            (chartWidth /
                (values.length - 1)) *
                index;

        const y =
            paddingTop +
            chartHeight -
            ((value - minValue) /
                (maxValue - minValue)) *
                chartHeight;

        if (index === 0) {
            ctx.moveTo(x, y);
        } else {
            ctx.lineTo(x, y);
        }
    });

    ctx.strokeStyle =
        "#2563eb";

    ctx.lineWidth = 2;
    ctx.stroke();

    /*
     * Data points.
     */
    values.forEach((value, index) => {
        const x =
            paddingLeft +
            (chartWidth /
                (values.length - 1)) *
                index;

        const y =
            paddingTop +
            chartHeight -
            ((value - minValue) /
                (maxValue - minValue)) *
                chartHeight;

        ctx.beginPath();

        ctx.arc(
            x,
            y,
            3,
            0,
            Math.PI * 2
        );

        ctx.fillStyle =
            "#2563eb";

        ctx.fill();
    });

    /*
     * Chart label.
     */
    ctx.textAlign = "left";
    ctx.textBaseline = "alphabetic";
    ctx.fillStyle = "#6b7280";
    ctx.font = "11px Arial";

    ctx.fillText(
        `${label} (${unit})`,
        paddingLeft,
        height - 8
    );
}


/*
 * Update all charts.
 */
function updateCharts() {
    drawChart(
        "temperatureChart",
        state.history.temperature,
        "Temperature",
        "°C"
    );

    drawChart(
        "humidityChart",
        state.history.humidity,
        "Humidity",
        "%"
    );

    drawChart(
        "iaqChart",
        state.history.iaq,
        "Indoor Air Quality",
        "IAQ"
    );

    drawChart(
        "co2Chart",
        state.history.co2,
        "CO₂ Equivalent",
        "ppm"
    );

    drawChart(
        "vocChart",
        state.history.voc,
        "VOC Equivalent",
        "ppm"
    );
}


/*
 * Fetch and display the latest telemetry.
 */
async function updateDashboard() {
    try {
        const data =
            await getSensorData();

        state.current = data;

        updateValues(data);

        addHistoryPoint(data);

        updateCharts();

    } catch (error) {
        console.error(
            "Unable to retrieve telemetry:",
            error
        );

        updateConnectionStatus(false);
    }
}


/*
 * Start dashboard.
 */
updateDashboard();

setInterval(
    updateDashboard,
    POLL_INTERVAL_MS
);


/*
 * Redraw charts when the window changes size.
 */
window.addEventListener(
    "resize",
    () => {
        updateCharts();
    }
);
