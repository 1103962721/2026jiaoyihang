#include <cassert>
#include <string>
#include "../Shared/Protocol.h"
#include "../Shared/ClientRole.h"

int main()
{
    assert(Protocol::DefaultPort == 50505);
    assert(Protocol::BuildHelloMessage() == "HELLO\n");
    assert(Protocol::BuildReadyMessage(true) == "READY 1\n");
    assert(Protocol::BuildReadyMessage(false) == "READY 0\n");
    assert(Protocol::BuildTriggerMessage(5) == "TRIGGER 5\n");
    assert(Protocol::BuildTriggerMessage(5, "123.50") == "TRIGGER 5 123.50\n");
    assert(Protocol::BuildScheduledTriggerMessage(2, 123456789, "88") == "SCHEDULED_TRIGGER 2 123456789 88\n");
    assert(Protocol::BuildRoleMessage(ClientRole::Coin) == "ROLE COIN\n");
    assert(Protocol::BuildRoleMessage(ClientRole::Boss) == "ROLE BOSS\n");
    assert(Protocol::BuildPingMessage() == "PING\n");
    assert(Protocol::BuildPongMessage() == "PONG\n");
    assert(Protocol::BuildTeamCodeRequestMessage() == "TEAM_CODE_REQUEST\n");
    assert(Protocol::BuildTeamReadyRequestMessage() == "TEAM_READY_REQUEST\n");
    assert(Protocol::BuildTeamCodeResponseMessage("ABC1234") == "TEAM_CODE_RESPONSE ABC1234\n");
    assert(Protocol::BuildTeamCodeResponseMessage("INVALID") == "TEAM_CODE_RESPONSE INVALID\n");
    assert(Protocol::BuildTeamCodeResponseMessage("ERROR") == "TEAM_CODE_RESPONSE ERROR\n");
    assert(Protocol::BuildTeamReadyMessage(0, 1, 2) == "TEAM_READY 0 1 2\n");
    assert(Protocol::BuildTeamReadyMessage(0, 0, 0) == "TEAM_READY 0 0 0\n");
    assert(Protocol::BuildTeamReadyMessage(1, 2, 2) == "TEAM_READY 1 2 2\n");
    assert(Protocol::BuildTeamReadyMessage(-1, 0, 0) == "TEAM_READY -1 0 0\n");
    assert(Protocol::BuildOcrRequestMessage(7, 1945, 1261, 500, 60, 2560, 1440) == "OCR_REQUEST 7 1945 1261 500 60 2560 1440\n");
    assert(Protocol::BuildOcrResponseMessage(7, true, "取消行动") == "OCR_RESPONSE 7 OK 取消行动\n");
    assert(Protocol::BuildOcrResponseMessage(7, false, "line\nbreak") == "OCR_RESPONSE 7 ERROR line break\n");
    assert(Protocol::BuildSelfThunderRequestMessage(11) == "SELF_THUNDER_REQUEST 11\n");
    assert(Protocol::BuildSelfThunderResponseMessage(11, true, "完成") == "SELF_THUNDER_RESPONSE 11 OK 完成\n");
    assert(Protocol::BuildSelfThunderResponseMessage(11, false, "line\nbreak") == "SELF_THUNDER_RESPONSE 11 ERROR line break\n");
    assert(Protocol::BuildSaveLoadoutRequestMessage(15, "weapon", "backpack", "egg", "bullet", 5, 10)
        == "SAVE_LOADOUT_REQUEST 15 5 10 weapon|backpack|egg|bullet\n");
    assert(Protocol::BuildSaveLoadoutRequestMessage(15, "weapon", "backpack", "egg", "bullet", 5, 10, true)
        == "SAVE_LOADOUT_REQUEST 15 5 10 weapon|backpack|egg|bullet|PISTOL\n");
    assert(Protocol::BuildListingRequestMessage(21, true, 5, false, "99.50", "测试 上架 文本") == "LISTING_REQUEST 21 1 5 0 99.50 测试 上架 文本\n");
    assert(Protocol::BuildListingResponseMessage(21, true, "识别到上架") == "LISTING_RESPONSE 21 OK 识别到上架\n");
    assert(Protocol::BuildMailRequestMessage(31) == "MAIL_REQUEST 31\n");
    assert(Protocol::BuildMailResponseMessage(31, true, "领取完成") == "MAIL_RESPONSE 31 OK 领取完成\n");
    assert(Protocol::BuildAccountLoginStopRequestMessage() == "ACCOUNT_LOGIN_STOP_REQUEST\n");
    assert(Protocol::BuildForceStopActionAckMessage() == "FORCE_STOP_ACTION_ACK\n");
    assert(Protocol::BuildClientKickedMessage() == "CLIENT_KICKED\n");
    assert(Protocol::BuildExecutionResponseMessage(false, "窗口失效\n") == "EXECUTION_RESPONSE ERROR 窗口失效 \n");
    assert(Protocol::IsValidTeamCodePayload("ABC1234"));
    assert(!Protocol::IsValidTeamCodePayload("abc1234"));
    assert(!Protocol::IsValidTeamCodePayload("AB12345"));
    assert(Protocol::IsValidTeamReadyValue(0, 1));
    assert(Protocol::IsValidTeamReadyValue(2, 2));
    assert(!Protocol::IsValidTeamReadyValue(3, 2));

    Protocol::ParsedMessage parsed{};
    assert(Protocol::ParseLine("HELLO", parsed));
    assert(parsed.type == Protocol::MessageType::Hello);

    assert(Protocol::ParseLine("PING", parsed));
    assert(parsed.type == Protocol::MessageType::Ping);

    assert(Protocol::ParseLine("PONG", parsed));
    assert(parsed.type == Protocol::MessageType::Pong);

    assert(Protocol::ParseLine("FORCE_STOP_ACTION_ACK", parsed));
    assert(parsed.type == Protocol::MessageType::ForceStopActionAck);

    assert(Protocol::ParseLine("READY 1", parsed));
    assert(parsed.type == Protocol::MessageType::Ready);
    assert(parsed.value == 1);

    assert(Protocol::ParseLine("READY 0", parsed));
    assert(parsed.type == Protocol::MessageType::Ready);
    assert(parsed.value == 0);

    assert(Protocol::ParseLine("TRIGGER 5", parsed));
    assert(parsed.type == Protocol::MessageType::Trigger);
    assert(parsed.value == 5);

    assert(Protocol::ParseLine("TRIGGER 0", parsed));
    assert(parsed.type == Protocol::MessageType::Trigger);
    assert(parsed.value == 0);

    assert(Protocol::ParseLine("TRIGGER 3 123.50", parsed));
    assert(parsed.type == Protocol::MessageType::Trigger);
    assert(parsed.value == 3);
    assert(parsed.bossPrice == "123.50");

    assert(Protocol::ParseLine("SCHEDULED_TRIGGER 2 123456789 88", parsed));
    assert(parsed.type == Protocol::MessageType::ScheduledTrigger);
    assert(parsed.value == 2);
    assert(parsed.timestampMs == 123456789);
    assert(parsed.bossPrice == "88");

    assert(Protocol::ParseLine("ROLE COIN", parsed));
    assert(parsed.type == Protocol::MessageType::Role);
    assert(parsed.role == ClientRole::Coin);

    assert(Protocol::ParseLine("ROLE BOSS", parsed));
    assert(parsed.type == Protocol::MessageType::Role);
    assert(parsed.role == ClientRole::Boss);
    assert(!Protocol::ParseLine("ROLE UNKNOWN", parsed));
    assert(!Protocol::ParseLine("ROLE COIN EXTRA", parsed));

    assert(Protocol::ParseLine("TEAM_CODE_REQUEST", parsed));
    assert(parsed.type == Protocol::MessageType::TeamCodeRequest);

    assert(Protocol::ParseLine("TEAM_READY_REQUEST", parsed));
    assert(parsed.type == Protocol::MessageType::TeamReadyRequest);

    assert(Protocol::ParseLine("TEAM_CODE_RESPONSE ABC1234", parsed));
    assert(parsed.type == Protocol::MessageType::TeamCodeResponse);
    assert(parsed.payload == "ABC1234");

    assert(Protocol::ParseLine("TEAM_CODE_RESPONSE INVALID", parsed));
    assert(parsed.type == Protocol::MessageType::TeamCodeResponse);
    assert(parsed.payload == "INVALID");

    assert(Protocol::ParseLine("TEAM_CODE_RESPONSE ERROR", parsed));
    assert(parsed.type == Protocol::MessageType::TeamCodeResponse);
    assert(parsed.payload == "ERROR");

    assert(Protocol::ParseLine("TEAM_READY 0 1 2", parsed));
    assert(parsed.type == Protocol::MessageType::TeamReady);
    assert(parsed.value == 0);
    assert(parsed.current == 1);
    assert(parsed.total == 2);

    assert(Protocol::ParseLine("TEAM_READY 0 0 0", parsed));
    assert(parsed.type == Protocol::MessageType::TeamReady);
    assert(parsed.value == 0);
    assert(parsed.current == 0);
    assert(parsed.total == 0);

    assert(Protocol::ParseLine("TEAM_READY 1 2 2", parsed));
    assert(parsed.type == Protocol::MessageType::TeamReady);
    assert(parsed.value == 1);
    assert(parsed.current == 2);
    assert(parsed.total == 2);

    assert(Protocol::ParseLine("TEAM_READY -1 0 0", parsed));
    assert(parsed.type == Protocol::MessageType::TeamReady);
    assert(parsed.value == -1);
    assert(parsed.current == 0);
    assert(parsed.total == 0);

    assert(Protocol::ParseLine("OCR_REQUEST 7 1945 1261 500 60 2560 1440", parsed));
    assert(parsed.type == Protocol::MessageType::OcrRequest);
    assert(parsed.requestId == 7);
    assert(parsed.x == 1945);
    assert(parsed.y == 1261);
    assert(parsed.width == 500);
    assert(parsed.height == 60);
    assert(parsed.referenceWidth == 2560);
    assert(parsed.referenceHeight == 1440);

    assert(Protocol::ParseLine("OCR_RESPONSE 7 OK 取消行动", parsed));
    assert(parsed.type == Protocol::MessageType::OcrResponse);
    assert(parsed.requestId == 7);
    assert(parsed.success);
    assert(parsed.payload == "取消行动");

    assert(Protocol::ParseLine("OCR_RESPONSE 8 OK", parsed));
    assert(parsed.type == Protocol::MessageType::OcrResponse);
    assert(parsed.requestId == 8);
    assert(parsed.success);
    assert(parsed.payload.empty());

    assert(Protocol::ParseLine("OCR_RESPONSE 9 ERROR OCR失败", parsed));
    assert(parsed.type == Protocol::MessageType::OcrResponse);
    assert(parsed.requestId == 9);
    assert(!parsed.success);
    assert(parsed.payload == "OCR失败");

    assert(Protocol::ParseLine("LOADOUT_REQUEST 13 NORMAL", parsed));
    assert(parsed.type == Protocol::MessageType::LoadoutRequest);
    assert(parsed.requestId == 13);
    assert(parsed.payload == "NORMAL");

    assert(Protocol::ParseLine("LOADOUT_REQUEST 14 SECRET", parsed));
    assert(parsed.type == Protocol::MessageType::LoadoutRequest);
    assert(parsed.requestId == 14);
    assert(parsed.payload == "SECRET");

    assert(Protocol::ParseLine("LOADOUT_RESPONSE 13 OK 完成", parsed));
    assert(parsed.type == Protocol::MessageType::LoadoutResponse);
    assert(parsed.requestId == 13);
    assert(parsed.success);
    assert(parsed.payload == "完成");

    assert(Protocol::ParseLine("LOADOUT_RESPONSE 14 ERROR 配装失败", parsed));
    assert(parsed.type == Protocol::MessageType::LoadoutResponse);
    assert(parsed.requestId == 14);
    assert(!parsed.success);
    assert(parsed.payload == "配装失败");

    assert(Protocol::ParseLine("SAVE_LOADOUT_REQUEST 15 5 10 weapon|backpack|egg|bullet", parsed));
    assert(parsed.type == Protocol::MessageType::SaveLoadoutRequest);
    assert(!parsed.loadoutPistol);
    assert(Protocol::ParseLine("SAVE_LOADOUT_REQUEST 16 5 10 weapon|backpack|egg|bullet|PISTOL", parsed));
    assert(parsed.type == Protocol::MessageType::SaveLoadoutRequest);
    assert(parsed.loadoutPistol);
    assert(parsed.loadoutWeapon == "weapon");
    assert(!Protocol::ParseLine("SAVE_LOADOUT_REQUEST 16 5 10 weapon|backpack|egg|bullet|UNKNOWN", parsed));

    assert(Protocol::ParseLine("LISTING_REQUEST 21 1 5 0 99.50 测试 上架 文本", parsed));
    assert(parsed.type == Protocol::MessageType::ListingRequest);
    assert(parsed.requestId == 21);
    assert(parsed.value == 1);
    assert(parsed.purchaseCount == 5);
    assert(!parsed.loadoutMode);
    assert(parsed.price == "99.50");
    assert(parsed.payload == "测试 上架 文本");

    assert(Protocol::ParseLine("LISTING_RESPONSE 21 OK 识别到上架", parsed));
    assert(parsed.type == Protocol::MessageType::ListingResponse);
    assert(parsed.requestId == 21);
    assert(parsed.success);
    assert(parsed.payload == "识别到上架");

    assert(Protocol::ParseLine("MAIL_REQUEST 31", parsed));
    assert(parsed.type == Protocol::MessageType::MailRequest);
    assert(parsed.requestId == 31);
    assert(Protocol::ParseLine("MAIL_RESPONSE 31 OK 领取完成", parsed));
    assert(parsed.type == Protocol::MessageType::MailResponse);
    assert(parsed.success);
    assert(parsed.payload == "领取完成");

    assert(Protocol::ParseLine("SELF_THUNDER_REQUEST 11", parsed));
    assert(parsed.type == Protocol::MessageType::SelfThunderRequest);
    assert(parsed.requestId == 11);

    assert(Protocol::ParseLine("SELF_THUNDER_RESPONSE 11 OK 完成", parsed));
    assert(parsed.type == Protocol::MessageType::SelfThunderResponse);
    assert(parsed.requestId == 11);
    assert(parsed.success);
    assert(parsed.payload == "完成");

    assert(Protocol::ParseLine("SELF_THUNDER_RESPONSE 12 ERROR 自雷失败", parsed));
    assert(parsed.type == Protocol::MessageType::SelfThunderResponse);

    assert(parsed.requestId == 12);
    assert(!parsed.success);
    assert(parsed.payload == "自雷失败");

    assert(Protocol::ParseLine("ACCOUNT_LOGIN_REQUEST 7 user pass", parsed));
    assert(parsed.type == Protocol::MessageType::AccountLoginRequest);
    assert(parsed.requestId == 7);
    assert(parsed.payload == "user\npass");
    assert(Protocol::ParseLine("ACCOUNT_LOGIN_STOP_REQUEST", parsed));
    assert(parsed.type == Protocol::MessageType::AccountLoginStopRequest);
    assert(Protocol::ParseLine("CLIENT_KICKED", parsed));
    assert(parsed.type == Protocol::MessageType::ClientKicked);
    assert(Protocol::ParseLine("EXECUTION_RESPONSE ERROR 窗口失效", parsed));
    assert(parsed.type == Protocol::MessageType::ExecutionResponse);
    assert(!parsed.success);
    assert(parsed.payload == "窗口失效");
    assert(Protocol::ParseLine("ACCOUNT_LOGIN_RESPONSE 7 STARTED launching", parsed));
    assert(parsed.type == Protocol::MessageType::AccountLoginResponse);
    assert(parsed.value == 1);
    assert(Protocol::ParseLine("ACCOUNT_LOGIN_RESPONSE 7 OK game-ready", parsed));
    assert(parsed.success);
    assert(Protocol::ParseLine("ACCOUNT_LOGIN_RESPONSE 7 READY 行前备战", parsed));
    assert(parsed.type == Protocol::MessageType::AccountLoginResponse);
    assert(parsed.value == 3 && parsed.success);
    assert(Protocol::ParseLine("ACCOUNT_LOGIN_RESPONSE 7 BANNED account-frozen", parsed));
    assert(parsed.type == Protocol::MessageType::AccountLoginResponse);
    assert(parsed.value == -2);
    assert(!parsed.success);
    assert(parsed.payload == "account-frozen");

    assert(!Protocol::ParseLine("READY 2", parsed));
    assert(!Protocol::ParseLine("TRIGGER -1", parsed));
    assert(!Protocol::ParseLine("TRIGGER 1 99 extra", parsed));
    assert(!Protocol::ParseLine("TEAM_CODE_RESPONSE", parsed));
    assert(!Protocol::ParseLine("TEAM_CODE_RESPONSE abc1234", parsed));
    assert(!Protocol::ParseLine("TEAM_CODE_RESPONSE AB12345", parsed));
    assert(!Protocol::ParseLine("TEAM_CODE_RESPONSE ABC123", parsed));
    assert(!Protocol::ParseLine("TEAM_CODE_RESPONSE ABC12345", parsed));
    assert(!Protocol::ParseLine("TEAM_CODE_RESPONSE ABC 1234", parsed));
    assert(!Protocol::ParseLine("TEAM_READY", parsed));
    assert(!Protocol::ParseLine("TEAM_READY 2 2 2", parsed));
    assert(!Protocol::ParseLine("TEAM_READY 1 1 2", parsed));
    assert(!Protocol::ParseLine("TEAM_READY 0 2 2", parsed));
    assert(!Protocol::ParseLine("TEAM_READY -1 1 2", parsed));
    assert(!Protocol::ParseLine("TEAM_READY 0 3 2", parsed));
    assert(!Protocol::ParseLine("OCR_REQUEST 0 0 0 300 60 2560 1440", parsed));
    assert(!Protocol::ParseLine("OCR_REQUEST 1 -1 0 300 60 2560 1440", parsed));
    assert(!Protocol::ParseLine("OCR_REQUEST 1 0 0 0 60 2560 1440", parsed));
    assert(!Protocol::ParseLine("OCR_REQUEST 1 0 0 300 60 0 1440", parsed));
    assert(!Protocol::ParseLine("OCR_RESPONSE 0 OK text", parsed));
    assert(!Protocol::ParseLine("OCR_RESPONSE 1 BAD text", parsed));
    assert(!Protocol::ParseLine("LOADOUT_REQUEST 0 NORMAL", parsed));
    assert(!Protocol::ParseLine("LOADOUT_REQUEST 1 UNKNOWN", parsed));
    assert(!Protocol::ParseLine("LOADOUT_REQUEST 1 NORMAL extra", parsed));
    assert(!Protocol::ParseLine("LOADOUT_RESPONSE 0 OK text", parsed));
    assert(!Protocol::ParseLine("LOADOUT_RESPONSE 1 BAD text", parsed));
    assert(!Protocol::ParseLine("LISTING_REQUEST 0 1 5 0 99 text", parsed));
    assert(!Protocol::ParseLine("LISTING_REQUEST 1 2 5 0 99 text", parsed));
    assert(!Protocol::ParseLine("LISTING_REQUEST 1 1 0 0 99 text", parsed));
    assert(!Protocol::ParseLine("LISTING_REQUEST 1 1 100 0 99 text", parsed));
    assert(!Protocol::ParseLine("LISTING_REQUEST 1 1 5 2 99 text", parsed));
    assert(Protocol::ParseLine("LISTING_REQUEST 1 1 5 0  text", parsed));
    assert(parsed.payload.empty());
    assert(Protocol::ParseLine("LISTING_REQUEST 1 1 5 1 99", parsed));
    assert(parsed.loadoutMode);
    assert(parsed.payload.empty());
    assert(!Protocol::ParseLine("LISTING_RESPONSE 0 OK text", parsed));
    assert(!Protocol::ParseLine("LISTING_RESPONSE 1 BAD text", parsed));
    assert(!Protocol::ParseLine("SELF_THUNDER_REQUEST 0", parsed));
    assert(!Protocol::ParseLine("SELF_THUNDER_REQUEST 1 extra", parsed));
    assert(!Protocol::ParseLine("SELF_THUNDER_RESPONSE 0 OK text", parsed));
    assert(!Protocol::ParseLine("SELF_THUNDER_RESPONSE 1 BAD text", parsed));
    assert(!Protocol::ParseLine("UNKNOWN", parsed));

    return 0;
}
