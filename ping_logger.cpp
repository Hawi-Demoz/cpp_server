// Stage 2: Repeated ICMP pings on a schedule, logged in memory.
// Compile (MSYS2 UCRT64):
//   g++ -o ping_logger.exe ping_logger.cpp -liphlpapi -lws2_32
// Run:
//   ./ping_logger.exe
//   ./ping_logger.exe 1.1.1.1
// Stop with Ctrl+C — it will print a short summary of what was stored.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// One recorded measurement. This is the "black box" sample we'll later serve as JSON.
struct PingSample {
    std::chrono::system_clock::time_point timestamp;
    bool success;
    DWORD latencyMs; // meaningful only when success == true
};

// Set to false by Ctrl+C so the loop can exit cleanly after the current wait/ping.
static volatile bool g_running = true;

BOOL WINAPI onConsoleCtrl(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        std::cout << "\nStopping after current ping/sleep...\n";
        return TRUE;
    }
    return FALSE;
}

std::string formatTime(const std::chrono::system_clock::time_point& tp) {
    // Convert to local calendar time for human-readable logs.
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm localTm{};
    localtime_s(&localTm, &t); // Windows-specific secure localtime

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%H:%M:%S");
    return oss.str();
}

PingSample doOnePing(HANDLE icmpHandle, DWORD destAddr, DWORD timeoutMs) {
    const char sendData[] = "ping-stage2";
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

int main(int argc, char* argv[]) {
    const char* targetIp = (argc >= 2) ? argv[1] : "8.8.8.8";
    const DWORD pingTimeoutMs = 2000;   // max wait for one Echo Reply
    const DWORD intervalMs = 2000;      // aim for one sample every ~2 seconds

    // Ctrl+C sets g_running = false instead of killing the process immediately.
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cout << "WSAStartup failed\n";
        return 1;
    }

    IN_ADDR destAddr{};
    if (inet_pton(AF_INET, targetIp, &destAddr) != 1) {
        std::cout << "Invalid IPv4 address: " << targetIp << "\n";
        WSACleanup();
        return 1;
    }

    HANDLE icmpHandle = IcmpCreateFile();
    if (icmpHandle == INVALID_HANDLE_VALUE) {
        std::cout << "IcmpCreateFile failed: " << GetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // In-memory history. Grows for as long as the program runs.
    // Stage 4 will expose this; for now we only print it.
    std::vector<PingSample> history;

    std::cout << "Logging pings to " << targetIp
              << " every ~" << (intervalMs / 1000) << "s. Ctrl+C to stop.\n";

    // -----------------------------------------------------------------------
    // Timing model (important):
    //
    //   loop {
    //     1. IcmpSendEcho(...)   // BLOCKS up to pingTimeoutMs
    //     2. push result into history
    //     3. Sleep(intervalMs)   // BLOCKS for the gap between samples
    //   }
    //
    // This program does ONE thing at a time on ONE thread. While Sleeping or
    // waiting for ICMP, it cannot do anything else (like answer HTTP).
    // That limitation is exactly why Stage 3 exists.
    //
    // "Without blocking forever" means: we don't call an infinite Sleep or
    // a single never-returning wait. We block in SHORT, bounded chunks
    // (timeout / interval), then loop and check g_running again.
    // -----------------------------------------------------------------------
    while (g_running) {
        auto pingStart = std::chrono::steady_clock::now();

        PingSample sample = doOnePing(icmpHandle, destAddr.S_un.S_addr, pingTimeoutMs);
        history.push_back(sample);

        if (sample.success) {
            std::cout << "[" << formatTime(sample.timestamp) << "] OK  "
                      << sample.latencyMs << " ms  (samples=" << history.size() << ")\n"
                      << std::flush;
        } else {
            std::cout << "[" << formatTime(sample.timestamp) << "] FAIL"
                      << "         (samples=" << history.size() << ")\n"
                      << std::flush;
        }

        // Sleep only the remaining time so the *cycle* stays near intervalMs,
        // even if the ping itself took part of that budget.
        // Example: interval=2000, ping took 137ms → sleep ~1863ms.
        auto elapsed = std::chrono::steady_clock::now() - pingStart;
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        if (elapsedMs < static_cast<long long>(intervalMs) && g_running) {
            Sleep(static_cast<DWORD>(intervalMs - elapsedMs)); // Windows Sleep(ms)
        }
    }

    // Dump what we stored — proves the vector held real history.
    std::cout << "\n===== In-memory log (" << history.size() << " samples) =====\n";
    for (size_t i = 0; i < history.size(); ++i) {
        const PingSample& s = history[i];
        std::cout << "  #" << (i + 1) << "  " << formatTime(s.timestamp) << "  ";
        if (s.success) {
            std::cout << s.latencyMs << " ms\n";
        } else {
            std::cout << "FAIL\n";
        }
    }
    std::cout << "=============================================\n";

    IcmpCloseHandle(icmpHandle);
    WSACleanup();
    return 0;
}
