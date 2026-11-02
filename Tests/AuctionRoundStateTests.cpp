#include "../DF_PlayClick_Client_Qt/AuctionRoundState.h"
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
int main()
{
    using Action = AuctionRoundState::Action;
    AuctionRoundState state;
    for (int i = 0; i < 100; ++i)
        require(state.observe(false, true) == Action::None, "wait indefinitely while buy disabled");
    require(state.observe(true, true) == Action::Buy, "buy when enabled");
    state.invoked(Action::Buy);
    for (int i = 0; i < 100; ++i)
        require(state.observe(true, true) == Action::None, "stale enabled sample cannot start sell");
    require(state.observe(true, false) == Action::None, "sell change alone is not proof of purchase start");
    require(state.observe(false, false) == Action::None, "wait while listing disabled");
    require(state.observe(true, false) == Action::None, "purchase finished but sell still disabled");
    require(state.observe(true, true) == Action::Sell, "sell when enabled after purchase start");
    state.invoked(Action::Sell);
    for (int i = 0; i < 100; ++i)
        require(state.observe(true, true) == Action::None, "stale enabled sample cannot complete");
    require(state.observe(false, true) == Action::None, "buy change alone is not proof of listing start");
    require(state.observe(true, false) == Action::None, "listing busy observed");
    require(state.observe(false, true) == Action::None, "both buttons required to finish");
    require(state.observe(true, true) == Action::Complete, "finish after listing busy then both enabled");
    AuctionRoundState second;
    require(second.observe(true, true) == Action::Buy, "new round resets state");
    second.invoked(Action::Buy);
    require(second.observe(false, true) == Action::Sell, "sell may be immediately enabled when buy starts");
    second.invoked(Action::Sell);
    require(second.observe(false, true) == Action::None, "preexisting disabled buy cannot complete sell");
    require(second.observe(true, false) == Action::None, "observe sell disabled");
    require(second.observe(true, true) == Action::Complete, "second round completes");
    using CoinAction = AuctionCoinState::Action;
    AuctionCoinState coinListing(AuctionCoinState::Mode::Listing);
    require(coinListing.observe(false, true) == CoinAction::Failed, "coin listing rejects unavailable buy");
    require(coinListing.observe(false, false) == CoinAction::Failed, "coin listing rejects both disabled");
    require(coinListing.observe(true, true) == CoinAction::Invoke, "coin listing invokes buy");
    coinListing.invoked();
    require(coinListing.observe(true, true) == CoinAction::None, "stale all-enabled cannot finish coin listing");
    require(coinListing.observe(false, true) == CoinAction::None, "coin listing observes buy disabled");
    require(coinListing.observe(true, false) == CoinAction::None, "coin listing needs both buttons");
    require(coinListing.observe(true, true) == CoinAction::Complete, "coin listing finishes after both recover");
    AuctionCoinState coinExecution(AuctionCoinState::Mode::Execution);
    require(coinExecution.observe(true, false) == CoinAction::Failed, "coin execution rejects unavailable sell");
    require(coinExecution.observe(false, false) == CoinAction::Failed, "coin execution rejects both disabled");
    require(coinExecution.observe(true, true) == CoinAction::Invoke, "coin execution invokes sell");
    coinExecution.invoked();
    require(coinExecution.observe(true, true) == CoinAction::None, "stale all-enabled cannot finish coin execution");
    require(coinExecution.observe(false, false) == CoinAction::None, "coin execution waits for buy ready");
    require(coinExecution.observe(true, false) == CoinAction::Complete, "coin execution finishes at requested state");
    std::puts("PASS purchase/listing sequencing, disabled waits, delayed UI, completion gates, round reset");
}
