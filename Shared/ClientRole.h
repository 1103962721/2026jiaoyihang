#pragma once

#include <string>

enum class ClientRole
{
    Coin,
    Boss
};

namespace ClientRoleUtil
{
    constexpr ClientRole DefaultRole = ClientRole::Coin;

    inline std::wstring ToDisplayText(ClientRole role)
    {
        return role == ClientRole::Boss ? L"老板" : L"币号";
    }

    inline ClientRole FromDisplayText(const std::wstring& text)
    {
        return text == L"老板" ? ClientRole::Boss : ClientRole::Coin;
    }
}
