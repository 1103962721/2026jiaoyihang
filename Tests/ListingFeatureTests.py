from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_text(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8-sig")


def test_server_listing_controls_and_status_column_exist():
    header = read_text("DF_PlayClick_Server_Qt/MainWindow.h")
    source = read_text("DF_PlayClick_Server_Qt/MainWindow.cpp")
    assert "listingPriceEdit_" in header
    assert "listingExchangeCheck_" in header
    assert "listingPurchaseCountSpin_" in header
    assert "setValue(5)" in source
    assert 'QStringLiteral("上架状态")' in source
    assert "startListing(" in source


def test_listing_only_targets_coin_clients_and_tracks_status():
    controller = read_text("DF_PlayClick_Server_Qt/ServerController.cpp")
    manager = read_text("DF_PlayClick_Server_Qt/ServerConnectionManager.cpp")
    assert "client.role != ClientRole::Coin" in controller
    assert 'QStringLiteral("执行中")' in manager
    assert 'QStringLiteral("成功")' in manager
    assert 'QStringLiteral("失败")' in manager


def test_client_listing_sequence_contains_confirmed_coordinates_and_price_input():
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    for point in (
        "{ 946, 64 }", "{ 183, 232 }", "{ 786, 304 }", "{ 2323, 1111 }",
        "{ 2324, 1032 }",
        "{ 2199, 1230 }", "{ 2182, 1147 }", "{ 429, 72 }", "{ 1686, 214 }",
        "{ 1880, 934 }", "{ 1939, 745 }", "{ 1827, 858 }",
    ):
        assert point in source
    assert "pasteText(boundHwnd_, listingText" in source
    assert "pressKeyRepeated(VK_BACK, 5" in source
    assert "pasteText(boundHwnd_, listingPrice" in source
    assert 'findTextBounds(sellImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), QStringLiteral("出售")' in source
    assert source.index("pressKeyRepeated(VK_BACK, 5") < source.index("pasteText(boundHwnd_, listingPrice")


def test_listing_clears_text_input_before_pasting():
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    click_text = source.index("if (!click(textInput)")
    paste_text = source.index("pasteText(boundHwnd_, listingText", click_text)
    segment = source[click_text:paste_text]
    delay_before_select_all = segment.index("QThread::msleep(300);")
    select_all = segment.index("pressKeyChord(VK_CONTROL, 'A'")
    delay_before_clear = segment.index("QThread::msleep(100);", select_all)
    clear_text = segment.index("pressKey(VK_BACK", delay_before_clear)
    assert delay_before_select_all < select_all < delay_before_clear < clear_text


def test_listing_uses_two_stage_exchange_coordinates_and_delays():
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    assert "{ 2323, 1111 }" in source
    assert "{ 2324, 1032 }" in source
    first_exchange = source.index("if (!click(hasExchange ? exchangeEntry : noExchangeEntry))")
    second_exchange = source.index("const POINT purchasePoint = hasExchange ? exchangeOption : noExchangeOption", first_exchange)
    between_stages = source[first_exchange:second_exchange]
    assert "QThread::msleep(1000);" in between_stages
    assert "QThread::msleep(200);" not in between_stages

    press_escape = source.index("if (!input_.pressKey(VK_ESCAPE", second_exchange)
    after_second_stage = source[second_exchange:press_escape]
    assert "purchaseCount" in after_second_stage
    assert "clickIndex < purchaseCount" in after_second_stage
    assert "bounded(1200, 1501)" in after_second_stage
    assert "msleep(static_cast<unsigned long>(clickIntervalMs))" in after_second_stage
    assert "QThread::msleep(200);" in after_second_stage


def test_listing_waits_then_finds_and_clicks_sell_text():
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    move_price_area = source.index("moveReferencePoint(boundHwnd_, priceArea")
    wait_after_move = source.index("QThread::msleep(1500);", move_price_area)
    focus_click = source.index("if (!click(priceArea))", wait_after_move)
    wait_after_click = source.index("QThread::msleep(1000);", focus_click)
    capture_sell_area = source.index('const QString sellImagePath = captureOcrRegion(sellRegion, QStringLiteral("listing_sell")', wait_after_click)
    find_sell = source.index('findTextBounds(sellImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), QStringLiteral("出售")', capture_sell_area)
    click_sell = source.index("clickClientPoint(boundHwnd_, sellClickPoint", find_sell)
    assert move_price_area < wait_after_move < focus_click < wait_after_click < capture_sell_area < find_sell < click_sell
    retry_segment = source[focus_click:click_sell]
    assert "SellRecognitionAttempts = 3" in retry_segment
    assert "attempt <= SellRecognitionAttempts" in retry_segment
    assert 'QStringLiteral("连续 3 次未识别到“出售”' in retry_segment


def test_listing_sell_ocr_uses_2k_region_and_positioned_json_result():
    controller = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    runner_header = read_text("DF_PlayClick_Client_Qt/UmiOcrRunner.h")
    runner_source = read_text("DF_PlayClick_Client_Qt/UmiOcrRunner.cpp")
    assert "sellRegion.x = 0" in controller
    assert "sellRegion.y = 0" in controller
    assert "sellRegion.width = 1650" in controller
    assert "sellRegion.height = 1440" in controller
    assert "sellTextBounds.center()" in controller
    assert "pressKeyChordByScanCode" not in controller
    assert "findTextBounds(" in runner_header
    assert 'QStringLiteral("json")' in runner_source
    assert 'value(QStringLiteral("box"))' in runner_source
    assert 'contains(keyword, Qt::CaseInsensitive)' in runner_source

def test_listing_uses_three_price_clicks_with_confirmed_delays():
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    first_click = source.index("if (!click(firstPriceClick))")
    wait_after_first_click = source.index("QThread::msleep(2000);", first_click)
    price_panel_click = source.index("if (!click(pricePanelClick))", wait_after_first_click)
    wait_after_panel_click = source.index("QThread::msleep(500);", price_panel_click)
    price_input_click = source.index("if (!click(priceInput))", wait_after_panel_click)
    wait_after_price_input = source.index("QThread::msleep(200);", price_input_click)
    clear_price = source.index("pressKeyRepeated(VK_BACK, 5", wait_after_price_input)
    assert first_click < wait_after_first_click < price_panel_click < wait_after_panel_click
    assert wait_after_panel_click < price_input_click < wait_after_price_input < clear_price


def test_listing_ocr_uses_2k_reference_region_and_keyword():
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    assert "region.x = 1448" in source
    assert "region.y = 368" in source
    assert "region.width = 630" in source
    assert "region.height = 700" in source
    assert "constexpr int ReferenceWidth = 2560" in source
    assert "region.referenceWidth = ReferenceWidth" in source
    assert "constexpr int ReferenceHeight = 1440" in source
    assert "region.referenceHeight = ReferenceHeight" in source
    assert 'contains(QStringLiteral("上架")' in source


def test_client_listing_is_connected_and_rejects_boss_role():
    header = read_text("DF_PlayClick_Client_Qt/ClientController.h")
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    assert "handleListingRequest" in header
    assert "executeListingSequence" in header
    assert "&ClientConnection::listingRequestReceived" in source
    assert "currentRole_ != ClientRole::Coin" in source
    assert "sendListingResponse(requestId, false" in source


def test_listing_holds_local_input_mutex_for_whole_sequence():
    dispatcher_header = read_text("DF_PlayClick_Client_Qt/Win32InputDispatcher.h")
    source = read_text("DF_PlayClick_Client_Qt/ClientController.cpp")
    assert "class Win32InputSequenceLock" in dispatcher_header
    assert "Win32InputSequenceLock inputSequenceLock" in source
    assert "inputSequenceLock.acquire(errorText" in source
