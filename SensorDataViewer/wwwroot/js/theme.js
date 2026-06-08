// Light/dark theme toggle backed by Bootstrap 5.3 `data-bs-theme` + localStorage.
(function () {
    function apply(theme) {
        document.documentElement.setAttribute("data-bs-theme", theme);
        try { localStorage.setItem("theme", theme); } catch { }
        var icon = document.getElementById("theme-toggle-icon");
        if (icon) icon.textContent = theme === "dark" ? "☀️" : "🌙";
        recolorCharts();
    }

    // Recolor already-rendered charts so a theme switch doesn't leave stale axis/text colors.
    function recolorCharts() {
        var charts = window.sensorCharts || {};
        var css = getComputedStyle(document.documentElement);
        var textColor = (css.getPropertyValue("--bs-body-color") || "#212529").trim();
        var gridColor = (css.getPropertyValue("--bs-border-color") || "#dee2e6").trim();
        Object.keys(charts).forEach(function (id) {
            var ch = charts[id];
            if (!ch) return;
            ch.options.plugins.legend.labels.color = textColor;
            ["x", "y"].forEach(function (ax) {
                ch.options.scales[ax].ticks.color = textColor;
                ch.options.scales[ax].grid.color = gridColor;
            });
            if (ch.options.scales.y.title) ch.options.scales.y.title.color = textColor;
            ch.update();
        });
    }

    function current() {
        return document.documentElement.getAttribute("data-bs-theme") || "light";
    }

    window.toggleTheme = function () {
        apply(current() === "dark" ? "light" : "dark");
    };

    // Sync the button icon once the DOM is ready (theme attr is set early in <head>).
    document.addEventListener("DOMContentLoaded", function () {
        apply(current());
    });
})();
