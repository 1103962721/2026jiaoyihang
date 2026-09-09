#pragma once

#include <winsock2.h>
#include <string>

namespace SocketUtil
{
    class WsaSession
    {
    public:
        WsaSession()
        {
            ok_ = WSAStartup(MAKEWORD(2, 2), &data_) == 0;
        }

        ~WsaSession()
        {
            if (ok_)
            {
                WSACleanup();
            }
        }

        bool Ok() const { return ok_; }

    private:
        WSADATA data_{};
        bool ok_ = false;
    };

    inline bool SendAll(SOCKET socketHandle, const char* data, int length)
    {
        int sentTotal = 0;
        while (sentTotal < length)
        {
            int sent = send(socketHandle, data + sentTotal, length - sentTotal, 0);
            if (sent <= 0)
            {
                return false;
            }
            sentTotal += sent;
        }
        return true;
    }

    inline bool SendLine(SOCKET socketHandle, const std::string& line)
    {
        return SendAll(socketHandle, line.c_str(), static_cast<int>(line.size()));
    }
}
