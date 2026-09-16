#pragma once

#include <sstream>
#include <string>

#include "ClientRole.h"

namespace Protocol
{
    constexpr int DefaultPort = 50505;
    // 每个客户端连接建立后的首次校时使用此保留编号；补偿任务使用更高的动态编号。
    constexpr int InitialTimeSyncRequestId = 1;

    enum class MessageType
    {
        Unknown,
        Hello,
        Ready,
        Trigger,
        Role,
        Ping,
        Pong,
        TimeSyncRequest,
        TimeSyncResponse,
        ServerTime,
        ScheduledTrigger,
        TeamCodeRequest,
        TeamCodeResponse,
        TeamReadyRequest,
        RebindWindowRequest,
        ForceStopActionRequest,
        TeamReady,
        OcrRequest,
        OcrResponse,
        SelfThunderRequest,
        SelfThunderResponse,
        LoadoutRequest,
        LoadoutResponse,
        ListingRequest,
        ListingResponse,
        MailRequest,
        MailResponse
    };

    enum class LoadoutType
    {
        Normal,
        Secret
    };

    struct ParsedMessage
    {
        MessageType type = MessageType::Unknown;
        int value = 0;
        int current = 0;
        int total = 0;
        int requestId = 0;
        int purchaseCount = 1;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int referenceWidth = 0;
        int referenceHeight = 0;
        long long timestampMs = 0;
        long long timestampMs2 = 0;
        bool success = false;
        ClientRole role = ClientRoleUtil::DefaultRole;
        std::string payload;
        std::string price;
    };

    inline std::string BuildHelloMessage()
    {
        return "HELLO\n";
    }

    inline std::string BuildReadyMessage(bool ready)
    {
        return ready ? "READY 1\n" : "READY 0\n";
    }

    inline std::string BuildTriggerMessage(int seconds)
    {
        return "TRIGGER " + std::to_string(seconds) + "\n";
    }

    inline std::string BuildRoleMessage(ClientRole role)
    {
        return role == ClientRole::Boss ? "ROLE BOSS\n" : "ROLE COIN\n";
    }

    inline std::string BuildPingMessage()
    {
        return "PING\n";
    }

    inline std::string BuildPongMessage()
    {
        return "PONG\n";
    }

    inline std::string BuildTimeSyncRequestMessage(int requestId, long long serverSendMs)
    {
        return "TIME_SYNC_REQUEST " + std::to_string(requestId) + " " + std::to_string(serverSendMs) + "\n";
    }

    inline std::string BuildTimeSyncResponseMessage(int requestId, long long clientReceiveMs, long long clientSendMs)
    {
        return "TIME_SYNC_RESPONSE " + std::to_string(requestId) + " " + std::to_string(clientReceiveMs) + " " + std::to_string(clientSendMs) + "\n";
    }

    inline std::string BuildScheduledTriggerMessage(int seconds, long long targetClientMs)
    {
        return "SCHEDULED_TRIGGER " + std::to_string(seconds) + " " + std::to_string(targetClientMs) + "\n";
    }

    inline std::string BuildServerTimeMessage(long long serverTimeMs, long long clientOffsetMs)
    {
        return "SERVER_TIME " + std::to_string(serverTimeMs) + " " + std::to_string(clientOffsetMs) + "\n";
    }

    inline bool IsValidTeamCodePayload(const std::string& text)
    {
        if (text.size() != 7)
        {
            return false;
        }

        for (size_t index = 0; index < 3; ++index)
        {
            if (text[index] < 'A' || text[index] > 'Z')
            {
                return false;
            }
        }

        for (size_t index = 3; index < text.size(); ++index)
        {
            if (text[index] < '0' || text[index] > '9')
            {
                return false;
            }
        }

        return true;
    }

    inline bool IsTeamCodeStatusPayload(const std::string& text)
    {
        return text == "INVALID" || text == "ERROR";
    }

    inline std::string BuildTeamCodeRequestMessage()
    {
        return "TEAM_CODE_REQUEST\n";
    }

    inline std::string BuildTeamCodeResponseMessage(const std::string& payload)
    {
        return "TEAM_CODE_RESPONSE " + payload + "\n";
    }

    inline std::string BuildTeamReadyRequestMessage()
    {
        return "TEAM_READY_REQUEST\n";
    }

    inline std::string BuildRebindWindowRequestMessage()
    {
        return "REBIND_WINDOW_REQUEST\n";
    }

    inline std::string BuildForceStopActionRequestMessage()
    {
        return "FORCE_STOP_ACTION_REQUEST\n";
    }

    inline bool IsValidTeamReadyValue(int current, int total)
    {
        return (current == 0 && total == 0) || (current >= 0 && total > 0 && current <= total);
    }

    inline std::string BuildTeamReadyMessage(int state, int current, int total)
    {
        return "TEAM_READY " + std::to_string(state) + " " + std::to_string(current) + " " + std::to_string(total) + "\n";
    }

    inline std::string NormalizeLinePayload(std::string text)
    {
        for (char& ch : text)
        {
            if (ch == '\r' || ch == '\n' || ch == '\t')
            {
                ch = ' ';
            }
        }
        return text;
    }

    inline std::string BuildOcrRequestMessage(int requestId, int x, int y, int width, int height, int referenceWidth, int referenceHeight)
    {
        return "OCR_REQUEST " + std::to_string(requestId) + " " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(width) + " " + std::to_string(height) + " " + std::to_string(referenceWidth) + " " + std::to_string(referenceHeight) + "\n";
    }

    inline std::string BuildOcrResponseMessage(int requestId, bool success, const std::string& payload)
    {
        return "OCR_RESPONSE " + std::to_string(requestId) + (success ? " OK " : " ERROR ") + NormalizeLinePayload(payload) + "\n";
    }

    inline std::string BuildSelfThunderRequestMessage(int requestId)
    {
        return "SELF_THUNDER_REQUEST " + std::to_string(requestId) + "\n";
    }

    inline std::string BuildSelfThunderResponseMessage(int requestId, bool success, const std::string& payload)
    {
        return "SELF_THUNDER_RESPONSE " + std::to_string(requestId) + (success ? " OK " : " ERROR ") + NormalizeLinePayload(payload) + "\n";
    }

    inline std::string LoadoutTypeToPayload(LoadoutType type)
    {
        return type == LoadoutType::Secret ? "SECRET" : "NORMAL";
    }

    inline bool IsValidLoadoutTypePayload(const std::string& payload)
    {
        return payload == "NORMAL" || payload == "SECRET";
    }

    inline std::string BuildLoadoutRequestMessage(int requestId, LoadoutType type)
    {
        return "LOADOUT_REQUEST " + std::to_string(requestId) + " " + LoadoutTypeToPayload(type) + "\n";
    }

    inline std::string BuildLoadoutResponseMessage(int requestId, bool success, const std::string& payload)
    {
        return "LOADOUT_RESPONSE " + std::to_string(requestId) + (success ? " OK " : " ERROR ") + NormalizeLinePayload(payload) + "\n";
    }

    inline std::string BuildListingRequestMessage(int requestId, bool hasExchange, int purchaseCount, const std::string& price, const std::string& listingText)
    {
        return "LISTING_REQUEST " + std::to_string(requestId) + (hasExchange ? " 1 " : " 0 ") + std::to_string(purchaseCount) + " "
            + NormalizeLinePayload(price) + " " + NormalizeLinePayload(listingText) + "\n";
    }

    inline std::string BuildListingResponseMessage(int requestId, bool success, const std::string& payload)
    {
        return "LISTING_RESPONSE " + std::to_string(requestId) + (success ? " OK " : " ERROR ") + NormalizeLinePayload(payload) + "\n";
    }

    inline std::string BuildMailRequestMessage(int requestId)
    {
        return "MAIL_REQUEST " + std::to_string(requestId) + "\n";
    }

    inline std::string BuildMailResponseMessage(int requestId, bool success, const std::string& payload)
    {
        return "MAIL_RESPONSE " + std::to_string(requestId) + (success ? " OK " : " ERROR ") + NormalizeLinePayload(payload) + "\n";
    }

    inline bool ParseLine(const std::string& line, ParsedMessage& message)
    {
        message = ParsedMessage{};

        if (line == "HELLO")
        {
            message.type = MessageType::Hello;
            return true;
        }

        if (line == "PING")
        {
            message.type = MessageType::Ping;
            return true;
        }

        if (line == "PONG")
        {
            message.type = MessageType::Pong;
            return true;
        }

        if (line == "TEAM_CODE_REQUEST")
        {
            message.type = MessageType::TeamCodeRequest;
            return true;
        }

        if (line == "TEAM_READY_REQUEST")
        {
            message.type = MessageType::TeamReadyRequest;
            return true;
        }

        if (line == "REBIND_WINDOW_REQUEST")
        {
            message.type = MessageType::RebindWindowRequest;
            return true;
        }

        if (line == "FORCE_STOP_ACTION_REQUEST")
        {
            message.type = MessageType::ForceStopActionRequest;
            return true;
        }

        std::istringstream stream(line);
        std::string command;
        stream >> command;
        if (stream.fail())
        {
            return false;
        }

        if (command == "TIME_SYNC_REQUEST")
        {
            int requestId = 0;
            long long serverSendMs = 0;
            std::string extra;
            stream >> requestId >> serverSendMs;
            if (stream.fail() || requestId <= 0 || serverSendMs <= 0 || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::TimeSyncRequest;
            message.requestId = requestId;
            message.timestampMs = serverSendMs;
            return true;
        }

        if (command == "TIME_SYNC_RESPONSE")
        {
            int requestId = 0;
            long long clientReceiveMs = 0;
            long long clientSendMs = 0;
            std::string extra;
            stream >> requestId >> clientReceiveMs >> clientSendMs;
            if (stream.fail() || requestId <= 0 || clientReceiveMs <= 0 || clientSendMs < clientReceiveMs || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::TimeSyncResponse;
            message.requestId = requestId;
            message.timestampMs = clientReceiveMs;
            message.timestampMs2 = clientSendMs;
            return true;
        }

        if (command == "SERVER_TIME")
        {
            long long serverTimeMs = 0;
            long long clientOffsetMs = 0;
            std::string extra;
            stream >> serverTimeMs >> clientOffsetMs;
            if (stream.fail() || serverTimeMs <= 0 || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::ServerTime;
            message.timestampMs = serverTimeMs;
            message.timestampMs2 = clientOffsetMs;
            return true;
        }

        if (command == "ROLE")
        {
            std::string roleText;
            std::string extra;
            stream >> roleText;
            if (stream.fail() || (stream >> extra) || (roleText != "COIN" && roleText != "BOSS"))
            {
                return false;
            }
            message.type = MessageType::Role;
            message.role = roleText == "BOSS" ? ClientRole::Boss : ClientRole::Coin;
            return true;
        }

        if (command == "TEAM_CODE_RESPONSE")
        {
            std::string payload;
            std::string extra;
            stream >> payload;
            if (stream.fail() || (stream >> extra))
            {
                return false;
            }
            if (!IsValidTeamCodePayload(payload) && !IsTeamCodeStatusPayload(payload))
            {
                return false;
            }
            message.type = MessageType::TeamCodeResponse;
            message.payload = payload;
            return true;
        }

        if (command == "TEAM_READY")
        {
            int state = 0;
            int current = 0;
            int total = 0;
            std::string extra;
            stream >> state >> current >> total;
            if (stream.fail() || (stream >> extra))
            {
                return false;
            }
            if (state < -1 || state > 1)
            {
                return false;
            }
            if ((state == -1 && (current != 0 || total != 0)) || (state != -1 && !IsValidTeamReadyValue(current, total)))
            {
                return false;
            }
            if (state == 1 && (current == 0 || current != total))
            {
                return false;
            }
            if (state == 0 && total > 0 && current >= total)
            {
                return false;
            }
            message.type = MessageType::TeamReady;
            message.value = state;
            message.current = current;
            message.total = total;
            return true;
        }

        if (command == "OCR_REQUEST")
        {
            int requestId = 0;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            int referenceWidth = 0;
            int referenceHeight = 0;
            std::string extra;
            stream >> requestId >> x >> y >> width >> height >> referenceWidth >> referenceHeight;
            if (stream.fail() || (stream >> extra))
            {
                return false;
            }
            if (requestId <= 0 || x < 0 || y < 0 || width <= 0 || height <= 0 || referenceWidth <= 0 || referenceHeight <= 0)
            {
                return false;
            }
            message.type = MessageType::OcrRequest;
            message.requestId = requestId;
            message.x = x;
            message.y = y;
            message.width = width;
            message.height = height;
            message.referenceWidth = referenceWidth;
            message.referenceHeight = referenceHeight;
            return true;
        }

        if (command == "OCR_RESPONSE")
        {
            int requestId = 0;
            std::string status;
            stream >> requestId >> status;
            if (stream.fail() || requestId <= 0 || (status != "OK" && status != "ERROR"))
            {
                return false;
            }

            std::string payload;
            std::getline(stream, payload);
            if (!payload.empty() && payload.front() == ' ')
            {
                payload.erase(0, 1);
            }

            message.type = MessageType::OcrResponse;
            message.requestId = requestId;
            message.success = status == "OK";
            message.payload = payload;
            return true;
        }

        if (command == "SELF_THUNDER_REQUEST")
        {
            int requestId = 0;
            std::string extra;
            stream >> requestId;
            if (stream.fail() || requestId <= 0 || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::SelfThunderRequest;
            message.requestId = requestId;
            return true;
        }

        if (command == "SELF_THUNDER_RESPONSE")
        {
            int requestId = 0;
            std::string status;
            stream >> requestId >> status;
            if (stream.fail() || requestId <= 0 || (status != "OK" && status != "ERROR"))
            {
                return false;
            }

            std::string payload;
            std::getline(stream, payload);
            if (!payload.empty() && payload.front() == ' ')
            {
                payload.erase(0, 1);
            }

            message.type = MessageType::SelfThunderResponse;
            message.requestId = requestId;
            message.success = status == "OK";
            message.payload = payload;
            return true;
        }

        if (command == "LOADOUT_REQUEST")
        {
            int requestId = 0;
            std::string payload;
            std::string extra;
            stream >> requestId >> payload;
            if (stream.fail() || requestId <= 0 || !IsValidLoadoutTypePayload(payload) || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::LoadoutRequest;
            message.requestId = requestId;
            message.payload = payload;
            return true;
        }

        if (command == "LOADOUT_RESPONSE")
        {
            int requestId = 0;
            std::string status;
            stream >> requestId >> status;
            if (stream.fail() || requestId <= 0 || (status != "OK" && status != "ERROR"))
            {
                return false;
            }

            std::string payload;
            std::getline(stream, payload);
            if (!payload.empty() && payload.front() == ' ')
            {
                payload.erase(0, 1);
            }

            message.type = MessageType::LoadoutResponse;
            message.requestId = requestId;
            message.success = status == "OK";
            message.payload = payload;
            return true;
        }

        if (command == "LISTING_REQUEST")
        {
            int requestId = 0;
            int hasExchange = 0;
            int purchaseCount = 0;
            std::string price;
            stream >> requestId >> hasExchange >> purchaseCount >> price;
            if (stream.fail() || requestId <= 0 || (hasExchange != 0 && hasExchange != 1)
                || purchaseCount <= 0 || purchaseCount > 99 || price.empty())
            {
                return false;
            }

            std::string listingText;
            std::getline(stream, listingText);
            if (!listingText.empty() && listingText.front() == ' ')
            {
                listingText.erase(0, 1);
            }
            if (listingText.empty())
            {
                return false;
            }

            message.type = MessageType::ListingRequest;
            message.requestId = requestId;
            message.value = hasExchange;
            message.purchaseCount = purchaseCount;
            message.price = price;
            message.payload = listingText;
            return true;
        }

        if (command == "LISTING_RESPONSE")
        {
            int requestId = 0;
            std::string status;
            stream >> requestId >> status;
            if (stream.fail() || requestId <= 0 || (status != "OK" && status != "ERROR"))
            {
                return false;
            }

            std::string payload;
            std::getline(stream, payload);
            if (!payload.empty() && payload.front() == ' ')
            {
                payload.erase(0, 1);
            }

            message.type = MessageType::ListingResponse;
            message.requestId = requestId;
            message.success = status == "OK";
            message.payload = payload;
            return true;
        }

        if (command == "MAIL_REQUEST")
        {
            int requestId = 0;
            std::string extra;
            stream >> requestId;
            if (stream.fail() || requestId <= 0 || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::MailRequest;
            message.requestId = requestId;
            return true;
        }

        if (command == "MAIL_RESPONSE")
        {
            int requestId = 0;
            std::string status;
            stream >> requestId >> status;
            if (stream.fail() || requestId <= 0 || (status != "OK" && status != "ERROR"))
            {
                return false;
            }
            std::string payload;
            std::getline(stream, payload);
            if (!payload.empty() && payload.front() == ' ')
            {
                payload.erase(0, 1);
            }
            message.type = MessageType::MailResponse;
            message.requestId = requestId;
            message.success = status == "OK";
            message.payload = payload;
            return true;
        }

        if (command == "SCHEDULED_TRIGGER")
        {
            int seconds = 0;
            long long targetClientMs = 0;
            std::string extra;
            stream >> seconds >> targetClientMs;
            if (stream.fail() || seconds < 0 || targetClientMs <= 0 || (stream >> extra))
            {
                return false;
            }
            message.type = MessageType::ScheduledTrigger;
            message.value = seconds;
            message.timestampMs = targetClientMs;
            return true;
        }

        int value = 0;
        std::string extra;
        stream >> value;
        if (stream.fail() || (stream >> extra))
        {
            return false;
        }

        if (command == "READY" && (value == 0 || value == 1))
        {
            message.type = MessageType::Ready;
            message.value = value;
            return true;
        }

        if (command == "TRIGGER" && value >= 0)
        {
            message.type = MessageType::Trigger;
            message.value = value;
            return true;
        }

        return false;
    }
}
