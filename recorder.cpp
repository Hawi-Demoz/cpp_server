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
  <title>Network Black-Box Recorder</title>
  <style>
    :root {
      --bg: #0f1419;
      --panel: #1a2332;
      --text: #e7ecf3;
      --muted: #8b9bb4;
      --ok: #3ecf8e;
      --fail: #f07178;
      --line: #5b9fd4;
      --grid: #2a3548;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Segoe UI", system-ui, sans-serif;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
    }
    header {
      padding: 1.25rem 1.5rem 0.5rem;
      border-bottom: 1px solid var(--grid);
    }
    header h1 {
      margin: 0 0 0.35rem;
      font-size: 1.35rem;
      font-weight: 600;
      letter-spacing: 0.02em;
    }
    header p { margin: 0; color: var(--muted); font-size: 0.95rem; }
    .meta {
      display: flex;
      flex-wrap: wrap;
      gap: 1rem 1.75rem;
      padding: 1rem 1.5rem;
      font-size: 0.95rem;
    }
    .meta span b { color: var(--ok); }
    .meta .fail b { color: var(--fail); }
    main { padding: 0 1.5rem 2rem; }
    .chart-wrap {
      background: var(--panel);
      border: 1px solid var(--grid);
      border-radius: 8px;
      padding: 0.75rem;
      margin-bottom: 1.25rem;
    }
    canvas { width: 100%; height: 220px; display: block; }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.9rem;
      background: var(--panel);
      border: 1px solid var(--grid);
      border-radius: 8px;
      overflow: hidden;
    }
    th, td { padding: 0.45rem 0.75rem; text-align: left; }
    th {
      background: #243044;
      color: var(--muted);
      font-weight: 600;
      font-size: 0.8rem;
      text-transform: uppercase;
      letter-spacing: 0.04em;
    }
    tr:nth-child(even) td { background: rgba(255,255,255,0.02); }
    .ok { color: var(--ok); }
    .bad { color: var(--fail); }
    #err { color: var(--fail); padding: 0 1.5rem; min-height: 1.2em; }
  </style>
</head>
<body>
  <header>
    <h1>Network Black-Box Recorder</h1>
    <p>ICMP latency log — refreshes from <code>/data</code> every 2 seconds</p>
  </header>
  <div class="meta">
    <span>Target: <b id="target">—</b></span>
    <span>Samples: <b id="count">0</b></span>
    <span id="lastWrap">Last: <b id="last">—</b></span>
    <span>Updated: <b id="updated">—</b></span>
  </div>
  <p id="err"></p>
  <main>
    <div class="chart-wrap">
      <canvas id="chart" width="900" height="220"></canvas>
    </div>
    <table>
      <thead>
        <tr><th>Time</th><th>Result</th><th>Latency</th></tr>
      </thead>
      <tbody id="rows"></tbody>
    </table>
  </main>
  <script>
    const chart = document.getElementById("chart");
    const ctx = chart.getContext("2d");

    function drawChart(samples) {
      const w = chart.width;
      const h = chart.height;
      ctx.clearRect(0, 0, w, h);

      const pad = { l: 40, r: 12, t: 12, b: 28 };
      const plotW = w - pad.l - pad.r;
      const plotH = h - pad.t - pad.b;

      ctx.strokeStyle = "#2a3548";
      ctx.beginPath();
      ctx.moveTo(pad.l, pad.t);
      ctx.lineTo(pad.l, pad.t + plotH);
      ctx.lineTo(pad.l + plotW, pad.t + plotH);
      ctx.stroke();

      if (!samples.length) {
        ctx.fillStyle = "#8b9bb4";
        ctx.fillText("Waiting for samples...", pad.l + 8, pad.t + 20);
        return;
      }

      const vals = samples.map(s => s.ok ? s.ms : null);
      const finite = vals.filter(v => v !== null);
      const maxMs = Math.max(50, ...(finite.length ? finite : [50]));

      ctx.fillStyle = "#8b9bb4";
      ctx.font = "12px Segoe UI, sans-serif";
      ctx.fillText(maxMs + " ms", 4, pad.t + 10);
      ctx.fillText("0", 18, pad.t + plotH);

      // Failures as vertical red marks
      for (let i = 0; i < samples.length; i++) {
        if (samples[i].ok) continue;
        const x = pad.l + (samples.length === 1 ? plotW / 2 : (i / (samples.length - 1)) * plotW);
        ctx.strokeStyle = "#f07178";
        ctx.beginPath();
        ctx.moveTo(x, pad.t);
        ctx.lineTo(x, pad.t + plotH);
        ctx.stroke();
      }

      // Latency line
      ctx.strokeStyle = "#5b9fd4";
      ctx.lineWidth = 2;
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
      ctx.lineWidth = 1;
    }

    function render(data) {
      document.getElementById("target").textContent = data.target;
      document.getElementById("count").textContent = data.samples.length;
      document.getElementById("updated").textContent = new Date().toLocaleTimeString();

      const last = data.samples[data.samples.length - 1];
      const lastEl = document.getElementById("last");
      const wrap = document.getElementById("lastWrap");
      if (!last) {
        lastEl.textContent = "—";
        wrap.classList.remove("fail");
      } else if (last.ok) {
        lastEl.textContent = last.ms + " ms @ " + last.t;
        wrap.classList.remove("fail");
      } else {
        lastEl.textContent = "FAIL @ " + last.t;
        wrap.classList.add("fail");
      }

      drawChart(data.samples);

      const tbody = document.getElementById("rows");
      const recent = data.samples.slice(-40).reverse();
      tbody.innerHTML = recent.map(s => {
        if (s.ok) {
          return "<tr><td>" + s.t + "</td><td class='ok'>OK</td><td>" + s.ms + " ms</td></tr>";
        }
        return "<tr><td>" + s.t + "</td><td class='bad'>FAIL</td><td>—</td></tr>";
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
          "Could not load /data — is recorder.exe still running? (" + e.message + ")";
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
