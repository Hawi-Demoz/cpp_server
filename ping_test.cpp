// Stage 1: One-shot ICMP ping using the Windows IcmpSendEcho API.
// Compile (from an MSYS2 UCRT64 shell):
//   g++ -o ping_test.exe ping_test.cpp -liphlpapi -lws2_32
// Run:
//   ./ping_test.exe
//   ./ping_test.exe 1.1.1.1

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // Default target: Google Public DNS. Pass a different IPv4 address as argv[1] if you want.
    const char* targetIp = (argc >= 2) ? argv[1] : "8.8.8.8";

    // -----------------------------------------------------------------------
    // 1) WSAStartup — initialize Winsock
    //
    // Even though ICMP is not a TCP/UDP socket, some address helpers (and
    // parts of the IP stack on Windows) expect Winsock to be started first.
    // Your HTTP server already does this; we do it here for the same reason.
    // -----------------------------------------------------------------------
    WSADATA wsaData;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        std::cout << "WSAStartup failed: " << wsaResult << std::endl;
        return 1;
    }

    // -----------------------------------------------------------------------
    // 2) inet_pton — convert "8.8.8.8" (text) into a 32-bit IPv4 address
    //
    // Network APIs want binary addresses, not strings. AF_INET means IPv4.
    // inet_pton returns 1 on success, 0 if the string is not a valid address,
    // and -1 on a harder failure.
    //
    // Linux note: same function exists there. Older Windows tutorials often
    // use inet_addr(), which is obsolete and can't report all errors cleanly.
    // -----------------------------------------------------------------------
    IN_ADDR destAddr{};
    if (inet_pton(AF_INET, targetIp, &destAddr) != 1) {
        std::cout << "Invalid IPv4 address: " << targetIp << std::endl;
        WSACleanup();
        return 1;
    }

    // -----------------------------------------------------------------------
    // 3) IcmpCreateFile — open a handle to the ICMP service
    //
    // Think of this like opening a "channel" to the OS's ICMP engine.
    // It is NOT a Winsock SOCKET. You cannot recv()/send() on it.
    // On failure it returns INVALID_HANDLE_VALUE.
    // -----------------------------------------------------------------------
    HANDLE icmpHandle = IcmpCreateFile();
    if (icmpHandle == INVALID_HANDLE_VALUE) {
        std::cout << "IcmpCreateFile failed. Error: " << GetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    // Payload bytes we send inside the ICMP Echo Request.
    // The remote host is supposed to echo them back unchanged.
    const char sendData[] = "ping-stage1";
    const WORD sendSize = static_cast<WORD>(sizeof(sendData));

    // -----------------------------------------------------------------------
    // 4) Reply buffer
    //
    // IcmpSendEcho writes one or more ICMP_ECHO_REPLY structures into a
    // buffer YOU provide. The buffer must be large enough for:
    //   sizeof(ICMP_ECHO_REPLY) + size of the echoed payload + some slack
    // Microsoft docs recommend at least 8 extra bytes of room.
    // -----------------------------------------------------------------------
    constexpr DWORD replyBufferSize = sizeof(ICMP_ECHO_REPLY) + sizeof(sendData) + 8;
    unsigned char replyBuffer[replyBufferSize] = {};

    // -----------------------------------------------------------------------
    // 5) IcmpSendEcho — send Echo Request, block until reply or timeout
    //
    // Arguments (in order):
    //   icmpHandle     — from IcmpCreateFile
    //   destAddr...    — IPv4 address as a DWORD (network byte order)
    //   sendData       — pointer to payload bytes
    //   sendSize       — payload length
    //   nullptr        — optional IP options (TTL etc.); nullptr = defaults
    //   replyBuffer    — where replies are written
    //   replyBufferSize
    //   timeoutMs      — how long to wait for a reply
    //
    // Return value: number of replies written into the buffer (0 = failure).
    // -----------------------------------------------------------------------
    const DWORD timeoutMs = 3000; // 3 seconds
    DWORD replyCount = IcmpSendEcho(
        icmpHandle,
        destAddr.S_un.S_addr,
        const_cast<char*>(sendData),
        sendSize,
        nullptr,
        replyBuffer,
        replyBufferSize,
        timeoutMs
    );

    if (replyCount == 0) {
        DWORD err = GetLastError();
        // IP_REQ_TIMED_OUT = 11010 — no reply within timeoutMs
        std::cout << "Ping to " << targetIp << " failed." << std::endl;
        std::cout << "GetLastError = " << err;
        if (err == IP_REQ_TIMED_OUT) {
            std::cout << " (IP_REQ_TIMED_OUT — no reply within " << timeoutMs << " ms)";
        }
        std::cout << std::endl;
    } else {
        // The first reply starts at the beginning of replyBuffer.
        auto* reply = reinterpret_cast<ICMP_ECHO_REPLY*>(replyBuffer);

        // reply->Status == 0 (IP_SUCCESS) means we got a normal Echo Reply.
        // Non-zero values are IP status codes (destination unreachable, etc.).
        if (reply->Status == IP_SUCCESS) {
            std::cout << "Reply from " << targetIp
                      << ": bytes=" << reply->DataSize
                      << " time=" << reply->RoundTripTime << " ms"
                      << " TTL=" << static_cast<unsigned int>(reply->Options.Ttl)
                      << std::endl;
        } else {
            std::cout << "Got a reply structure, but Status=" << reply->Status
                      << " (not IP_SUCCESS). RoundTripTime="
                      << reply->RoundTripTime << " ms" << std::endl;
        }
    }

    // -----------------------------------------------------------------------
    // 6) Cleanup — reverse order of acquisition
    // -----------------------------------------------------------------------
    IcmpCloseHandle(icmpHandle);
    WSACleanup();
    return 0;
}
