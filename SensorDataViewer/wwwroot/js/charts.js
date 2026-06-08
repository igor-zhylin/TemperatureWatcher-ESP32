// Thin wrapper around Chart.js for Blazor JS interop.
// One Chart instance is kept per canvas id so re-imports replace cleanly.
window.sensorCharts = window.sensorCharts || {};

window.renderLineChart = (canvasId, labels, label, unit, color, data) => {
    const canvas = document.getElementById(canvasId);
    if (!canvas || typeof Chart === "undefined") return;

    if (window.sensorCharts[canvasId]) {
        window.sensorCharts[canvasId].destroy();
    }

    // Pull text/grid colors from the active Bootstrap theme so charts stay legible in dark mode.
    const css = getComputedStyle(document.documentElement);
    const textColor = (css.getPropertyValue("--bs-body-color") || "#212529").trim();
    const gridColor = (css.getPropertyValue("--bs-border-color") || "#dee2e6").trim();

    window.sensorCharts[canvasId] = new Chart(canvas, {
        type: "line",
        data: {
            labels: labels,
            datasets: [{
                label: `${label} (${unit})`,
                data: data,
                borderColor: color,
                backgroundColor: color + "22",
                borderWidth: 2,
                pointRadius: 0,
                pointHoverRadius: 4,
                tension: 0.25,
                fill: true,
            }],
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            interaction: { mode: "index", intersect: false },
            plugins: {
                legend: { display: true, labels: { color: textColor } },
                tooltip: { callbacks: { label: (c) => `${c.parsed.y} ${unit}` } },
            },
            scales: {
                x: {
                    ticks: { maxTicksLimit: 12, autoSkip: true, color: textColor },
                    grid: { color: gridColor },
                },
                y: {
                    title: { display: true, text: unit, color: textColor },
                    ticks: { color: textColor },
                    grid: { color: gridColor },
                },
            },
        },
    });
};

window.destroyChart = (canvasId) => {
    if (window.sensorCharts[canvasId]) {
        window.sensorCharts[canvasId].destroy();
        delete window.sensorCharts[canvasId];
    }
};
