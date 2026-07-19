// Stage 3: Ping in the background WHILE the HTTP server keeps responding.
//
// Concurrency choice (read this before the code below):
//   Option A — single loop: accept HTTP when ready, ping when the clock says so.
//     Problem: IcmpSendEcho BLOCKS (up to timeout). During that wait the server
//     cannot accept/respond. Fixing that means non-blocking/async ICMP (harder).
//   Option B — second thread: ping loop on thread 2, HTTP accept loop on main.
//     Shared history protected by a mutex. Slightly more concepts, but each side
//     stays as simple as Stages 1–2, and HTTP never freezes during a ping.
//
// Recommendation for a beginner here: Option B (threads). Blocking ICMP makes
// a single-loop design awkward unless you jump to advanced async APIs.
//
// Compile (MSYS2 UCRT64):
//   g++ -o recorder.exe recorder.cpp -liphlpapi -lws2_32 -pthread
// Run:
//   ./recorder.exe
// Then in a browser or second terminal, hit http://localhost:8080/status
// while watching ping lines print in the recorder terminal.

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

// Shared state between ping thread and HTTP thread.
static std::vector<PingSample> g_history;
static std::mutex g_historyMutex;
static std::atomic<bool> g_running{true};

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

PingSample doOnePing(HANDLE icmpHandle, DWORD destAddr, DWORD timeoutMs) {
    const char sendData[] = "ping-stage3";
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

// Runs on its own thread. Same loop as Stage 2, but writes under a mutex.
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

    std::cout << "Ping thread started → " << targetIp << " every ~2s\n" << std::flush;

    while (g_running) {
        auto pingStart = std::chrono::steady_clock::now();
        PingSample sample = doOnePing(icmpHandle, destAddr.S_un.S_addr, pingTimeoutMs);

        size_t count = 0;
        {
            // Lock only while touching the shared vector — keep the critical
            // section short so HTTP requests aren't delayed.
            std::lock_guard<std::mutex> lock(g_historyMutex);
            g_history.push_back(sample);
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

// Snapshot of shared history for the /status page (Stage 4 will do full JSON).
std::string buildStatusPage() {
    size_t count = 0;
    bool lastOk = false;
    DWORD lastLatency = 0;
    std::string lastTime = "(none yet)";

    {
        std::lock_guard<std::mutex> lock(g_historyMutex);
        count = g_history.size();
        if (!g_history.empty()) {
            const PingSample& last = g_history.back();
            lastOk = last.success;
            lastLatency = last.latencyMs;
            lastTime = formatTime(last.timestamp);
        }
    }

    std::ostringstream body;
    body << "<!DOCTYPE html><html><body>";
    body << "<h1>Stage 3 — concurrency check</h1>";
    body << "<p>If this page loads while the terminal keeps printing ping lines, "
         << "the background thread and HTTP server are both alive.</p>";
    body << "<ul>";
    body << "<li>Samples logged: <b>" << count << "</b></li>";
    body << "<li>Last sample time: <b>" << lastTime << "</b></li>";
    body << "<li>Last result: <b>";
    if (count == 0) {
        body << "waiting...</b></li>";
    } else if (lastOk) {
        body << "OK " << lastLatency << " ms</b></li>";
    } else {
        body << "FAIL</b></li>";
    }
    body << "</ul>";
    body << "<p>Refresh this page a few times — the sample count should rise.</p>";
    body << "<p>Dashboard JSON/chart comes in Stage 4.</p>";
    body << "</body></html>";
    return body.str();
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
    std::string requestLine = request.substr(0, requestLineEnd);

    std::string method;
    std::string path;
    if (!parseRequestLine(requestLine, method, path)) {
        closesocket(clientSocket);
        return;
    }

    std::string response;
    if (method != "GET") {
        std::string body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
        response = buildHttpResponse("HTTP/1.1 405 Method Not Allowed",
                                     "text/html; charset=utf-8", body);
    } else if (path == "/" || path == "/status") {
        response = buildHttpResponse("HTTP/1.1 200 OK",
                                     "text/html; charset=utf-8", buildStatusPage());
    } else {
        std::string body = "<html><body><h1>404</h1><p>Try <a href=\"/status\">/status</a></p></body></html>";
        response = buildHttpResponse("HTTP/1.1 404 Not Found",
                                     "text/html; charset=utf-8", body);
    }

    sendAll(clientSocket, response);
    closesocket(clientSocket);
}

int main(int argc, char* argv[]) {
    const char* targetIp = (argc >= 2) ? argv[1] : "8.8.8.8";
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cout << "WSAStartup failed\n";
        return 1;
    }

    // Start pinging BEFORE listen — proves HTTP isn't needed for logging to begin.
    std::thread pinger(pingThreadMain, std::string(targetIp));

    // Dual-stack listen (IPv6 socket that also accepts IPv4).
    //
    // Windows-specific gotcha: many browsers resolve "localhost" to ::1 (IPv6)
    // first. An AF_INET-only bind (0.0.0.0) answers 127.0.0.1 but NOT ::1, so
    // you get ERR_CONNECTION_REFUSED even though the server is running.
    // Setting IPV6_V6ONLY=0 on an AF_INET6 socket accepts both.
    // (On Linux the default for V6ONLY often already allows this, but setting
    // it explicitly is portable and clear.)
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

    DWORD v6only = 0; // 0 = also accept IPv4-mapped connections
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

    // Non-blocking accept loop: check g_running periodically so Ctrl+C can exit.
    // (A blocking accept() would ignore Ctrl+C until the next browser hit.)
    u_long nonBlocking = 1;
    ioctlsocket(serverSocket, FIONBIO, &nonBlocking);

    std::cout << "HTTP server on http://127.0.0.1:8080/status  (or http://localhost:8080/status)\n"
              << std::flush;
    std::cout << "Open that URL while pings print below. Ctrl+C to stop.\n" << std::flush;

    while (g_running) {
        SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                // No client waiting — sleep briefly and try again.
                Sleep(200);
                continue;
            }
            std::cout << "accept() error: " << err << "\n" << std::flush;
            break;
        }

        // Handle one request on the HTTP thread. Fine for Stage 3; a production
        // server might spawn per-client threads or use an event loop.
        handleClient(clientSocket);
    }

    closesocket(serverSocket);
    g_running = false;
    pinger.join(); // wait for ping thread to finish its current cycle
    WSACleanup();
    return 0;
}
