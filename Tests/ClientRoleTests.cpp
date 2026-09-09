#include <cassert>
#include "../Shared/ClientRole.h"

int main()
{
    assert(ClientRoleUtil::DefaultRole == ClientRole::Coin);
    assert(ClientRoleUtil::ToDisplayText(ClientRole::Coin) == L"币号");
    assert(ClientRoleUtil::ToDisplayText(ClientRole::Boss) == L"老板");
    assert(ClientRoleUtil::FromDisplayText(L"老板") == ClientRole::Boss);
    assert(ClientRoleUtil::FromDisplayText(L"未知") == ClientRole::Coin);
    return 0;
}
