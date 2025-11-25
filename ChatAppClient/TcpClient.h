#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <stdexcept>

#pragma comment(lib, "Ws2_32.lib")

class TcpClient
{
public:
    TcpClient();
    ~TcpClient();

    // Cấm copy
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // Kết nối tới server
    bool Connect(const char* ip, int port);

    // Ngắt kết nối
    void Disconnect();

    // Gửi yêu cầu
    bool SendRequest(const std::string& request);

    // Nhận phản hồi 
    std::string ReceiveResponse();

    // Lấy socket 
    SOCKET GetSocket() const { return m_socket; }

    // Tách socket ra khỏi dialog
    void DetachSocket();

private:
    SOCKET m_socket;
    bool m_isConnected;
    bool m_wsaInitialized;
};