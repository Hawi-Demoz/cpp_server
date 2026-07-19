#include <iostream>
#include <filesystem>
#include <fstream>
#include <winsock2.h>
#include <sstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

string getContentType(const string& filePath) {
    string extension = filesystem::path(filePath).extension().string();

    if (extension == ".html" || extension == ".htm") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".txt") return "text/plain; charset=utf-8";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";

    return "application/octet-stream";
}

string buildHttpResponse(const string& statusLine, const string& contentType, const string& body) {
    ostringstream response;
    response << statusLine << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    return response.str();
}

string make404Page(const string& path) {
    ostringstream body;
    body << "<html><body><h1>404 Not Found</h1>";
    body << "<p>The file for path '" << path << "' was not found.</p>";
    body << "</body></html>";
    return body.str();
}

string readFileContents(const filesystem::path& filePath) {
    ifstream file(filePath, ios::binary);
    if (!file.is_open()) {
        return "";
    }

    ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool sendAll(SOCKET socketHandle, const string& data) {
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

bool parseRequestLine(const string& requestLine, string& method, string& path) {
    istringstream stream(requestLine);
    string version;

    if (!(stream >> method >> path >> version)) {
        return false;
    }

    return true;
}

int main() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cout << "WSAStartup failed: " << result << endl;
        return 1;
    }

    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        cout << "Failed to create socket" << endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8080);

    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "Bind failed" << endl;
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        cout << "Listen failed" << endl;
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    cout << "Server listening on http://localhost:8080" << endl;

    SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
    if (clientSocket == INVALID_SOCKET) {
        cout << "Accept failed" << endl;
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    cout << "Client connected!" << endl;

    char buffer[4096] = {0};
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "Failed to read request" << endl;
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    string request(buffer, bytesReceived);
    size_t requestLineEnd = request.find("\r\n");
    string requestLine = request.substr(0, requestLineEnd);

    string method;
    string path;
    if (!parseRequestLine(requestLine, method, path)) {
        cout << "Malformed HTTP request line" << endl;
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }

    if (method != "GET") {
        string body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";
        string response = buildHttpResponse("HTTP/1.1 405 Method Not Allowed", "text/html; charset=utf-8", body);
        if (!sendAll(clientSocket, response)) {
            cout << "Failed to send 405 response" << endl;
        }
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 0;
    }

    if (path == "/") {
        path = "/index.html";
    }

    if (path.find("..") != string::npos) {
        string body = make404Page(path);
        string response = buildHttpResponse("HTTP/1.1 404 Not Found", "text/html; charset=utf-8", body);
        if (!sendAll(clientSocket, response)) {
            cout << "Failed to send 404 response" << endl;
        }
        closesocket(clientSocket);
        closesocket(serverSocket);
        WSACleanup();
        return 0;
    }

    filesystem::path publicDir = filesystem::path("public");
    filesystem::path requestedFile = publicDir / path.substr(1);

    string response;

    if (!filesystem::exists(requestedFile) || !filesystem::is_regular_file(requestedFile)) {
        string body = make404Page(path);
        response = buildHttpResponse("HTTP/1.1 404 Not Found", "text/html; charset=utf-8", body);
    } else {
        string body = readFileContents(requestedFile);
        string contentType = getContentType(requestedFile.string());
        response = buildHttpResponse("HTTP/1.1 200 OK", contentType, body);
    }

    if (!sendAll(clientSocket, response)) {
        cout << "Failed to send response" << endl;
    }

    cout << "----- Request received -----" << endl;
    cout << buffer << endl;
    cout << "Method: " << method << endl;
    cout << "Path: " << path << endl;
    cout << "-----------------------------" << endl;

    closesocket(clientSocket);
    closesocket(serverSocket);
    WSACleanup();

    return 0;
}
