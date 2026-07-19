// Stage 4: Live dashboard — /data JSON + HTML/JS page that polls it.
// (Builds on Stage 3: ping thread + HTTP server + dual-stack listen.)
//
// Compile (MSYS2 UCRT64):
//   g++ -o recorder.exe recorder.cpp -liphlpapi -lws2_32 -pthread
// Run (leave this terminal open):
//   ./recorder.exe
// Then open: http://127.0.0.1:8080/

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct PingSample {
    std::chrono::system_clock::time_point timestamp;
    bool success;
    DWORD latencyMs;
};

static std::vector<PingSample> g_history;
static std::mutex g_historyMutex;
static std::atomic<bool> g_running{true};
static std::string g_targetIp = "8.8.8.8";

// Keep RAM / JSON size bounded (oldest samples drop off).
static constexpr size_t kMaxHistory = 300;

BOOL WINAPI onConsoleCtrl(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        std::cout << "\nShutting down...\n" << std::flush;
        return TRUE;
    }
    return FALSE;
}

std::string formatTime(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm localTm{};
    localtime_s(&localTm, &t);
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%H:%M:%S");
    return oss.str();
}

long long toUnixMs(const std::chrono::system_clock::time_point& tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               tp.time_since_epoch())
        .count();
}

PingSample doOnePing(HANDLE icmpHandle, DWORD destAddr, DWORD timeoutMs) {
    const char sendData[] = "ping-stage4";
    const WORD sendSize = static_cast<WORD>(sizeof(sendData));
    constexpr DWORD replyBufferSize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    unsigned char replyBuffer[replyBufferSize] = {};

    PingSample sample;
    sample.timestamp = std::chrono::system_clock::now();
    sample.success = false;
    sample.latencyMs = 0;

    DWORD replyCount = IcmpSendEcho(
        icmpHandle,
        destAddr,
        const_cast<char*>(sendData),
        sendSize,
        nullptr,
        replyBuffer,
        replyBufferSize,
        timeoutMs
    );

    if (replyCount > 0) {
        auto* reply = reinterpret_cast<ICMP_ECHO_REPLY*>(replyBuffer);
        if (reply->Status == IP_SUCCESS) {
            sample.success = true;
            sample.latencyMs = reply->RoundTripTime;
        }
    }
    return sample;
}

void pingThreadMain(std::string targetIp) {
    IN_ADDR destAddr{};
    if (inet_pton(AF_INET, targetIp.c_str(), &destAddr) != 1) {
        std::cout << "Ping thread: invalid IP " << targetIp << "\n" << std::flush;
        return;
    }

    HANDLE icmpHandle = IcmpCreateFile();
    if (icmpHandle == INVALID_HANDLE_VALUE) {
        std::cout << "Ping thread: IcmpCreateFile failed\n" << std::flush;
        return;
    }

    const DWORD pingTimeoutMs = 2000;
    const DWORD intervalMs = 2000;

    std::cout << "Ping thread started -> " << targetIp << " every ~2s\n" << std::flush;

    while (g_running) {
        auto pingStart = std::chrono::steady_clock::now();
        PingSample sample = doOnePing(icmpHandle, destAddr.S_un.S_addr, pingTimeoutMs);

        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_historyMutex);
            g_history.push_back(sample);
            if (g_history.size() > kMaxHistory) {
                g_history.erase(g_history.begin(),
                                g_history.begin() + (g_history.size() - kMaxHistory));
            }
            count = g_history.size();
        }

        if (sample.success) {
            std::cout << "[" << formatTime(sample.timestamp) << "] OK  "
                      << sample.latencyMs << " ms  (samples=" << count << ")\n"
                      << std::flush;
        } else {
            std::cout << "[" << formatTime(sample.timestamp) << "] FAIL"
                      << "         (samples=" << count << ")\n"
                      << std::flush;
        }

        auto elapsed = std::chrono::steady_clock::now() - pingStart;
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (elapsedMs < static_cast<long long>(intervalMs) && g_running) {
            Sleep(static_cast<DWORD>(intervalMs - elapsedMs));
        }
    }

    IcmpCloseHandle(icmpHandle);
    std::cout << "Ping thread stopped\n" << std::flush;
}

std::string buildHttpResponse(const std::string& statusLine,
                              const std::string& contentType,
                              const std::string& body) {
    std::ostringstream response;
    response << statusLine << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    // Helps browsers/devtools; same-origin fetch does not need CORS, but
    // this is harmless if you ever open the HTML from elsewhere later.
    response << "Cache-Control: no-store\r\n";
    response << "\r\n";
    response << body;
    return response.str();
}

bool sendAll(SOCKET socketHandle, const std::string& data) {
    const char* bytes = data.c_str();
    int totalSent = 0;
    int totalSize = static_cast<int>(data.size());
    while (totalSent < totalSize) {
        int sent = send(socketHandle, bytes + totalSent, totalSize - totalSent, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        totalSent += sent;
    }
    return true;
}

bool parseRequestLine(const std::string& requestLine, std::string& method, std::string& path) {
    std::istringstream stream(requestLine);
    std::string version;
    return static_cast<bool>(stream >> method >> path >> version);
}

// Build JSON by hand (no library). Keep it simple and strict.
std::string buildDataJson() {
    std::vector<PingSample> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        snapshot = g_history;
    }

    std::ostringstream json;
    json << "{\"target\":\"" << g_targetIp << "\",\"samples\":[";
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const PingSample& s = snapshot[i];
        if (i > 0) {
            json << ",";
        }
        json << "{\"t\":\"" << formatTime(s.timestamp) << "\","
             << "\"epochMs\":" << toUnixMs(s.timestamp) << ","
             << "\"ok\":" << (s.success ? "true" : "false") << ","
             << "\"ms\":";
        if (s.success) {
            json << s.latencyMs;
        } else {
            json << "null";
        }
        json << "}";
    }
    json << "]}";
    return json.str();
}

// Dashboard HTML+JS embedded so the exe works even if cwd has no public/.
std::string buildDashboardHtml() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>netrec // black-box</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg: #121212;
      --bg2: #0d0d0d;
      --panel: #161616;
      --text: #e0e0e0;
      --muted: #7a7a7a;
      --dim: #4a4a4a;
      --grid: #2a2a2a;
      --border: #333;
      --accent: #5a9e6f;   /* one muted green — numbers + latency trace */
      --down: #9a5a5a;     /* semantic DOWN only, not a second brand color */
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    html, body { height: 100%; }
    body {
      font-family: "JetBrains Mono", "Fira Code", "Cascadia Mono", Consolas, "Courier New", monospace;
      font-size: 13px;
      line-height: 1.35;
      background: var(--bg);
      color: var(--text);
      position: relative;
    }
    /* subtle CRT scanlines — low opacity so data stays readable */
    body::after {
      content: "";
      pointer-events: none;
      position: fixed;
      inset: 0;
      z-index: 50;
      background: repeating-linear-gradient(
        to bottom,
        transparent 0,
        transparent 2px,
        rgba(0, 0, 0, 0.12) 2px,
        rgba(0, 0, 0, 0.12) 3px
      );
    }
    .wrap {
      max-width: 960px;
      margin: 0 auto;
      padding: 10px 12px 16px;
      position: relative;
      z-index: 1;
    }
    .titlebar {
      display: flex;
      align-items: baseline;
      gap: 10px;
      border-bottom: 1px solid var(--border);
      padding-bottom: 6px;
      margin-bottom: 8px;
    }
    .titlebar h1 {
      font-size: 13px;
      font-weight: 700;
      letter-spacing: 0.06em;
      text-transform: uppercase;
      color: var(--text);
    }
    .titlebar .sub {
      color: var(--muted);
      font-size: 12px;
    }
    .cursor {
      display: inline-block;
      width: 0.6ch;
      background: var(--accent);
      color: transparent;
      animation: blink 1.1s step-end infinite;
      margin-left: 2px;
    }
    @keyframes blink {
      50% { opacity: 0; }
    }
    .stats {
      display: grid;
      grid-template-columns: repeat(4, 1fr);
      gap: 0;
      border: 1px solid var(--border);
      background: var(--panel);
      margin-bottom: 8px;
    }
    .stat {
      padding: 6px 10px;
      border-right: 1px solid var(--border);
    }
    .stat:last-child { border-right: none; }
    .stat .k {
      color: var(--muted);
      font-size: 11px;
      letter-spacing: 0.04em;
    }
    .stat .v {
      color: var(--accent);
      font-weight: 500;
      font-size: 13px;
      margin-top: 2px;
    }
    .stat.down .v { color: var(--down); }
    .stat .dot {
      display: inline-block;
      width: 6px;
      height: 6px;
      border-radius: 1px;
      background: var(--accent);
      margin-right: 6px;
      vertical-align: middle;
    }
    .stat.down .dot { background: var(--down); }
    #err {
      color: var(--down);
      min-height: 1.2em;
      margin-bottom: 6px;
      font-size: 12px;
    }
    .panel {
      border: 1px solid var(--border);
      background: var(--bg2);
      border-radius: 2px;
      margin-bottom: 8px;
    }
    .panel-h {
      color: var(--muted);
      font-size: 11px;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      padding: 4px 8px;
      border-bottom: 1px solid var(--border);
      background: var(--panel);
    }
    .chart-wrap { padding: 4px 4px 2px; }
    canvas {
      width: 100%;
      height: 200px;
      display: block;
      image-rendering: pixelated;
    }
    .log-wrap { max-height: 340px; overflow: auto; }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
    }
    th, td {
      padding: 3px 8px;
      text-align: left;
      border-bottom: 1px solid #1e1e1e;
      white-space: nowrap;
    }
    th {
      color: var(--muted);
      font-weight: 500;
      font-size: 11px;
      letter-spacing: 0.06em;
      position: sticky;
      top: 0;
      background: var(--panel);
    }
    td.num { color: var(--accent); }
    .tag-ok { color: var(--accent); }
    .tag-down { color: var(--down); }
    .foot {
      color: var(--dim);
      font-size: 11px;
      margin-top: 6px;
    }
    @media (max-width: 640px) {
      .stats { grid-template-columns: 1fr 1fr; }
      .stat:nth-child(2) { border-right: none; }
      .stat:nth-child(1), .stat:nth-child(2) { border-bottom: 1px solid var(--border); }
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="titlebar">
      <h1>netrec</h1>
      <span class="sub">icmp black-box // poll /data @ 2s</span>
      <span class="cursor">_</span>
    </div>

    <div class="stats">
      <div class="stat">
        <div class="k">TARGET</div>
        <div class="v" id="target">--</div>
      </div>
      <div class="stat">
        <div class="k">SAMPLES</div>
        <div class="v" id="count">0</div>
      </div>
      <div class="stat" id="statusStat">
        <div class="k">STATUS</div>
        <div class="v"><span class="dot" id="statusDot"></span><span id="statusTag">[--]</span></div>
      </div>
      <div class="stat">
        <div class="k">LAST RTT</div>
        <div class="v" id="last">--</div>
      </div>
    </div>

    <div id="err"></div>

    <div class="panel">
      <div class="panel-h">latency_ms  [trace]  — green = rtt, tick = down</div>
      <div class="chart-wrap">
        <canvas id="chart" width="920" height="200"></canvas>
      </div>
    </div>

    <div class="panel">
      <div class="panel-h">history  (newest first, last 50)</div>
      <div class="log-wrap">
        <table>
          <thead>
            <tr><th>TIME</th><th>STATE</th><th>RTT</th></tr>
          </thead>
          <tbody id="rows"></tbody>
        </table>
      </div>
    </div>

    <div class="foot">updated <span id="updated">--</span> // leave recorder.exe running</div>
  </div>

  <script>
    const ACCENT = "#5a9e6f";
    const DOWN = "#9a5a5a";
    const GRID = "#2a2a2a";
    const MUTED = "#7a7a7a";
    const BG = "#0d0d0d";

    const chart = document.getElementById("chart");
    const ctx = chart.getContext("2d");

    function drawChart(samples) {
      const w = chart.width;
      const h = chart.height;
      ctx.fillStyle = BG;
      ctx.fillRect(0, 0, w, h);

      const pad = { l: 44, r: 8, t: 10, b: 22 };
      const plotW = w - pad.l - pad.r;
      const plotH = h - pad.t - pad.b;

      const vals = samples.map(s => s.ok ? s.ms : null);
      const finite = vals.filter(v => v !== null);
      const maxMs = Math.max(50, ...(finite.length ? finite : [50]));

      // horizontal grid (oscilloscope style)
      ctx.strokeStyle = GRID;
      ctx.lineWidth = 1;
      const divisions = 4;
      for (let g = 0; g <= divisions; g++) {
        const y = pad.t + (plotH * g) / divisions;
        ctx.beginPath();
        ctx.moveTo(pad.l, y);
        ctx.lineTo(pad.l + plotW, y);
        ctx.stroke();
      }
      // vertical grid
      const vDiv = 8;
      for (let g = 0; g <= vDiv; g++) {
        const x = pad.l + (plotW * g) / vDiv;
        ctx.beginPath();
        ctx.moveTo(x, pad.t);
        ctx.lineTo(x, pad.t + plotH);
        ctx.stroke();
      }

      // axes
      ctx.strokeStyle = "#3a3a3a";
      ctx.beginPath();
      ctx.moveTo(pad.l, pad.t);
      ctx.lineTo(pad.l, pad.t + plotH);
      ctx.lineTo(pad.l + plotW, pad.t + plotH);
      ctx.stroke();

      ctx.fillStyle = MUTED;
      ctx.font = "11px JetBrains Mono, Consolas, monospace";
      ctx.fillText(maxMs + "ms", 2, pad.t + 9);
      ctx.fillText("0", 28, pad.t + plotH);

      if (!samples.length) {
        ctx.fillText("awaiting samples...", pad.l + 8, pad.t + 24);
        return;
      }

      // DOWN markers: short ticks at baseline (not full colorful bars)
      for (let i = 0; i < samples.length; i++) {
        if (samples[i].ok) continue;
        const x = pad.l + (samples.length === 1 ? plotW / 2 : (i / (samples.length - 1)) * plotW);
        ctx.strokeStyle = DOWN;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(x, pad.t + plotH);
        ctx.lineTo(x, pad.t + plotH - 10);
        ctx.stroke();
      }

      // latency trace — thin, no fill
      ctx.strokeStyle = ACCENT;
      ctx.lineWidth = 1.25;
      ctx.beginPath();
      let started = false;
      for (let i = 0; i < samples.length; i++) {
        if (!samples[i].ok) { started = false; continue; }
        const x = pad.l + (samples.length === 1 ? plotW / 2 : (i / (samples.length - 1)) * plotW);
        const y = pad.t + plotH - (samples[i].ms / maxMs) * plotH;
        if (!started) { ctx.moveTo(x, y); started = true; }
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
    }

    function render(data) {
      document.getElementById("target").textContent = data.target;
      document.getElementById("count").textContent = String(data.samples.length);
      document.getElementById("updated").textContent = new Date().toLocaleTimeString();

      const last = data.samples[data.samples.length - 1];
      const statusStat = document.getElementById("statusStat");
      const statusTag = document.getElementById("statusTag");
      const lastEl = document.getElementById("last");

      if (!last) {
        statusStat.classList.remove("down");
        statusTag.textContent = "[--]";
        lastEl.textContent = "--";
      } else if (last.ok) {
        statusStat.classList.remove("down");
        statusTag.textContent = "[OK]";
        lastEl.textContent = last.ms + " ms @ " + last.t;
      } else {
        statusStat.classList.add("down");
        statusTag.textContent = "[DOWN]";
        lastEl.textContent = "timeout @ " + last.t;
      }

      drawChart(data.samples);

      const tbody = document.getElementById("rows");
      const recent = data.samples.slice(-50).reverse();
      tbody.innerHTML = recent.map(s => {
        if (s.ok) {
          return "<tr><td>" + s.t + "</td><td class='tag-ok'>[OK]</td><td class='num'>" + s.ms + " ms</td></tr>";
        }
        return "<tr><td>" + s.t + "</td><td class='tag-down'>[DOWN]</td><td class='tag-down'>--</td></tr>";
      }).join("");
    }

    async function refresh() {
      try {
        const res = await fetch("/data");
        if (!res.ok) throw new Error("HTTP " + res.status);
        const data = await res.json();
        document.getElementById("err").textContent = "";
        render(data);
      } catch (e) {
        document.getElementById("err").textContent =
          "ERR fetch /data failed — is recorder.exe running? (" + e.message + ")";
      }
    }

    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>
)HTML";
}

void handleClient(SOCKET clientSocket) {
    char buffer[4096] = {};
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }

    std::string request(buffer, bytesReceived);
    size_t requestLineEnd = request.find("\r\n");
    std::string requestLine =
        (requestLineEnd == std::string::npos) ? request : request.substr(0, requestLineEnd);

    std::string method;
    std::string path;
    if (!parseRequestLine(requestLine, method, path)) {
        closesocket(clientSocket);
        return;
    }

    // Strip query string if present (?foo=bar)
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path = path.substr(0, q);
    }

    std::string response;
    if (method != "GET") {
        std::string body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
        response = buildHttpResponse("HTTP/1.1 405 Method Not Allowed",
                                     "text/html; charset=utf-8", body);
    } else if (path == "/" || path == "/index.html" || path == "/dashboard") {
        response = buildHttpResponse("HTTP/1.1 200 OK",
                                     "text/html; charset=utf-8", buildDashboardHtml());
    } else if (path == "/data") {
        response = buildHttpResponse("HTTP/1.1 200 OK",
                                     "application/json; charset=utf-8", buildDataJson());
    } else if (path == "/status") {
        // Keep Stage 3 probe working
        std::ostringstream body;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_historyMutex);
            count = g_history.size();
        }
        body << "<html><body><h1>OK</h1><p>samples=" << count
             << "</p><p><a href=\"/\">Open dashboard</a></p></body></html>";
        response = buildHttpResponse("HTTP/1.1 200 OK",
                                     "text/html; charset=utf-8", body.str());
    } else {
        std::string body =
            "<html><body><h1>404</h1><p>Try <a href=\"/\">/</a> or <a href=\"/data\">/data</a></p></body></html>";
        response = buildHttpResponse("HTTP/1.1 404 Not Found",
                                     "text/html; charset=utf-8", body);
    }

    sendAll(clientSocket, response);
    closesocket(clientSocket);
}

int main(int argc, char* argv[]) {
    g_targetIp = (argc >= 2) ? argv[1] : "8.8.8.8";
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cout << "WSAStartup failed\n";
        return 1;
    }

    std::thread pinger(pingThreadMain, g_targetIp);

    SOCKET serverSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cout << "socket(AF_INET6) failed: " << WSAGetLastError() << "\n";
        g_running = false;
        pinger.join();
        WSACleanup();
        return 1;
    }

    BOOL yes = TRUE;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    DWORD v6only = 0;
    if (setsockopt(serverSocket, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&v6only), sizeof(v6only)) == SOCKET_ERROR) {
        std::cout << "IPV6_V6ONLY setsockopt failed: " << WSAGetLastError() << "\n";
        closesocket(serverSocket);
        g_running = false;
        pinger.join();
        WSACleanup();
        return 1;
    }

    sockaddr_in6 serverAddr{};
    serverAddr.sin6_family = AF_INET6;
    serverAddr.sin6_addr = in6addr_any;
    serverAddr.sin6_port = htons(8080);

    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "bind() failed (" << WSAGetLastError()
                  << ") — is port 8080 already in use?\n";
        closesocket(serverSocket);
        g_running = false;
        pinger.join();
        WSACleanup();
        return 1;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cout << "listen() failed\n";
        closesocket(serverSocket);
        g_running = false;
        pinger.join();
        WSACleanup();
        return 1;
    }

    u_long nonBlocking = 1;
    ioctlsocket(serverSocket, FIONBIO, &nonBlocking);

    std::cout << "Dashboard:  http://127.0.0.1:8080/\n" << std::flush;
    std::cout << "JSON data:  http://127.0.0.1:8080/data\n" << std::flush;
    std::cout << "Leave this window open. Ctrl+C to stop.\n" << std::flush;

    while (g_running) {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                Sleep(200);
                continue;
            }
            std::cout << "accept() error: " << err << "\n" << std::flush;
            break;
        }
        handleClient(clientSocket);
    }

    closesocket(serverSocket);
    g_running = false;
    pinger.join();
    WSACleanup();
    return 0;
}
