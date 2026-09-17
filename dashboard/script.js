const SAMPLE_INTERVAL_MS = 5000;
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
 * Get sensor data.
 *
 * Currently this function generates demo data.
 * Later, this can be replaced with real data from
 * the Nordic Thingy:53 via USB, Bluetooth, or another
 * data source.
 */
function getSensorData() {
    const temperature = 23.48 + (Math.random() - 0.5) * 0.15;
    const humidity = 49.90 + (Math.random() - 0.5) * 0.4;
    const pressure = 995.96 + (Math.random() - 0.5) * 0.8;

    return {
        temperature: temperature,
        humidity: humidity,
        pressure: pressure,

        // BSEC values
        iaq: 50,
        iaq_accuracy: 0,
        co2: 500,
        voc: 0.50,

        // Gas sensor status
        gas_stability: 1,

        timestamp: new Date()
    };
}


/*
 * Convert IAQ value into a readable label.
 */
function getIaqLabel(iaq) {
    if (iaq <= 50) {
        return "Excellent";
    } else if (iaq <= 100) {
        return "Good";
    } else if (iaq <= 150) {
        return "Lightly polluted";
    } else if (iaq <= 200) {
        return "Moderately polluted";
    } else if (iaq <= 250) {
        return "Heavily polluted";
    } else if (iaq <= 350) {
        return "Severely polluted";
    } else {
        return "Extremely polluted";
    }
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
 * Update the measurement cards on the dashboard.
 */
function updateValues(data) {
    document.getElementById("temperature").textContent =
        `${data.temperature.toFixed(2)} °C`;

    document.getElementById("humidity").textContent =
        `${data.humidity.toFixed(2)} %`;

    document.getElementById("pressure").textContent =
        `${data.pressure.toFixed(2)} hPa`;

    document.getElementById("iaq").textContent =
        Math.round(data.iaq);

    document.getElementById("iaq-label").textContent =
        getIaqLabel(data.iaq);

    document.getElementById("iaq-accuracy").textContent =
        data.iaq_accuracy;

    document.getElementById("iaq-accuracy-label").textContent =
        getAccuracyLabel(data.iaq_accuracy);

    document.getElementById("co2").textContent =
        `${data.co2.toFixed(1)} ppm`;

    document.getElementById("voc").textContent =
        `${data.voc.toFixed(2)} ppm`;

    document.getElementById("gas-stability").textContent =
        data.gas_stability === 1 ? "Stable" : "Not stable";

    document.getElementById("last-update").textContent =
        `Last update: ${data.timestamp.toLocaleTimeString()}`;
}


/*
 * Add a new measurement to the history.
 */
function addHistoryPoint(data) {
    state.history.timestamps.push(data.timestamp);
    state.history.temperature.push(data.temperature);
    state.history.humidity.push(data.humidity);
    state.history.iaq.push(data.iaq);
    state.history.co2.push(data.co2);
    state.history.voc.push(data.voc);

    /*
     * Keep only the latest MAX_HISTORY_POINTS.
     */
    while (state.history.timestamps.length > MAX_HISTORY_POINTS) {
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
    const canvas = document.getElementById(canvasId);

    if (!canvas) {
        return;
    }

    const ctx = canvas.getContext("2d");

    /*
     * Make the canvas match its displayed size.
     */
    const rect = canvas.getBoundingClientRect();

    const width = rect.width;
    const height = rect.height;

    const devicePixelRatio = window.devicePixelRatio || 1;

    canvas.width = width * devicePixelRatio;
    canvas.height = height * devicePixelRatio;

    ctx.scale(devicePixelRatio, devicePixelRatio);

    /*
     * Clear the canvas.
     */
    ctx.clearRect(0, 0, width, height);

    if (values.length < 2) {
        ctx.font = "14px Arial";
        ctx.fillText("Waiting for data...", 20, 30);
        return;
    }

    /*
     * Chart margins.
     */
    const paddingLeft = 45;
    const paddingRight = 15;
    const paddingTop = 20;
    const paddingBottom = 30;

    const chartWidth =
        width - paddingLeft - paddingRight;

    const chartHeight =
        height - paddingTop - paddingBottom;

    /*
     * Find minimum and maximum values.
     */
    let minValue = Math.min(...values);
    let maxValue = Math.max(...values);

    /*
     * Give the graph some vertical space even when
     * all measurements are almost identical.
     */
    if (maxValue === minValue) {
        maxValue += 1;
        minValue -= 1;
    }

    const range = maxValue - minValue;

    minValue -= range * 0.1;
    maxValue += range * 0.1;

    /*
     * Draw horizontal grid lines.
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
            ((maxValue - minValue) / gridLines) * i;

        ctx.beginPath();
        ctx.moveTo(paddingLeft, y);
        ctx.lineTo(width - paddingRight, y);

        ctx.strokeStyle = "#e5e7eb";
        ctx.lineWidth = 1;
        ctx.stroke();

        ctx.fillStyle = "#6b7280";

        ctx.fillText(
            value.toFixed(1),
            paddingLeft - 8,
            y
        );
    }

    /*
     * Draw the measurement line.
     */
    ctx.beginPath();

    values.forEach((value, index) => {
        const x =
            paddingLeft +
            (chartWidth / (values.length - 1)) * index;

        const y =
            paddingTop +
            chartHeight -
            ((value - minValue) / (maxValue - minValue)) *
                chartHeight;

        if (index === 0) {
            ctx.moveTo(x, y);
        } else {
            ctx.lineTo(x, y);
        }
    });

    ctx.strokeStyle = "#2563eb";
    ctx.lineWidth = 2;
    ctx.stroke();

    /*
     * Draw data points.
     */
    values.forEach((value, index) => {
        const x =
            paddingLeft +
            (chartWidth / (values.length - 1)) * index;

        const y =
            paddingTop +
            chartHeight -
            ((value - minValue) / (maxValue - minValue)) *
                chartHeight;

        ctx.beginPath();
        ctx.arc(x, y, 3, 0, Math.PI * 2);

        ctx.fillStyle = "#2563eb";
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
        "temperature-chart",
        state.history.temperature,
        "Temperature",
        "°C"
    );

    drawChart(
        "humidity-chart",
        state.history.humidity,
        "Humidity",
        "%"
    );

    drawChart(
        "iaq-chart",
        state.history.iaq,
        "Indoor Air Quality",
        "IAQ"
    );

    drawChart(
        "co2-chart",
        state.history.co2,
        "CO₂ Equivalent",
        "ppm"
    );

    drawChart(
        "voc-chart",
        state.history.voc,
        "VOC Equivalent",
        "ppm"
    );
}


/*
 * Update the complete dashboard.
 */
function updateDashboard() {
    const data = getSensorData();

    state.current = data;

    updateValues(data);
    addHistoryPoint(data);
    updateCharts();
}


/*
 * Start the dashboard.
 */
updateDashboard();

setInterval(
    updateDashboard,
    SAMPLE_INTERVAL_MS
);


/*
 * Redraw charts when the browser window changes size.
 */
window.addEventListener("resize", () => {
    updateCharts();
});
