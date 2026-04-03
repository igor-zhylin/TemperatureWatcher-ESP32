#pragma once

// Static head + CSS block for the /api/stats history page.
// Used by handleStats() via ap() — passed as a single large write before dynamic content.
// Single-quoted attributes throughout to avoid escaping conflicts with C string literals.
static const char HTML_STATS_HEAD[] =
  "<!DOCTYPE html><html lang='en'><head>"
  "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>History</title><style>"
  "*{margin:0;padding:0;box-sizing:border-box}"
  "body{font-family:'Segoe UI',Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;flex-direction:column}"
  "nav{background:#16213e;border-bottom:1px solid #2a2a4a;display:flex;padding:0 20px;flex-shrink:0}"
  "nav a{color:#888;text-decoration:none;font-size:.9em;padding:14px 16px;border-bottom:2px solid transparent;display:inline-block}"
  "nav a:hover{color:#eee}"
  "nav a.on{color:#e94560;border-bottom-color:#e94560}"
  "main{flex:1;padding:24px}"
  ".card{background:#16213e;border-radius:16px;padding:24px;margin:0 auto;max-width:680px}"
  "h1{color:#e94560;margin-bottom:4px;font-size:1.3em}"
  ".sub{color:#555;font-size:.85em;margin-bottom:16px}"
  "svg{width:100%;height:90px;display:block;margin-bottom:20px}"
  "table{width:100%;border-collapse:collapse;font-size:.9em}"
  "th{color:#888;font-weight:normal;text-align:left;padding:6px 8px;border-bottom:1px solid #2a2a4a}"
  "td{padding:6px 8px;border-bottom:1px solid #16213e}"
  "tr:last-child td{border:none}"
  ".v{color:#e94560;font-weight:bold}a{color:#e94560}"
  ".btn{display:inline-block;margin-top:16px;padding:8px 18px;background:#e94560;"
  "color:#fff;border-radius:8px;cursor:pointer;font-size:.9em;text-decoration:none}"
  ".btn:hover{background:#c73652}"
  "</style></head><body>"
  "<nav><a href='/'>Live</a><a class='on' href='/api/stats'>History</a><a href='/api/wifi-setup'>WiFi</a></nav>"
  "<main><div class='card'>";

// Static footer block for the /api/stats history page.
static const char HTML_STATS_FOOT[] =
  "</table><div style='text-align:center'>"
  "<a class='btn' href='/api/export'>Download CSV</a>&nbsp;&nbsp;"
  "<a class='btn' href='/api/reset-flash' onclick=\"return confirm('Delete all flash records?')\">"
  "Reset Flash</a></div></div></main></body></html>";
