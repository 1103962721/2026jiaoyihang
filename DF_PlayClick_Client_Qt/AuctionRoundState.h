#pragma once

// Each invocation needs a subsequent busy observation. A stale all-enabled
// sample must neither start listing early nor finish a newly invoked round.
class AuctionRoundState
{
public:
    enum class Action { None, Buy, Sell, Complete };
    Action observe(bool buyEnabled, bool sellEnabled)
    {
        switch (stage_)
        {
        case Stage::Buy:
            return buyEnabled ? Action::Buy : Action::None;
        case Stage::BuyStarted:
            if (!buyEnabled) stage_ = Stage::Sell;
            else return Action::None;
            [[fallthrough]];
        case Stage::Sell:
            return sellEnabled ? Action::Sell : Action::None;
        case Stage::SellStarted:
            if (!sellEnabled) stage_ = Stage::Complete;
            return Action::None;
        case Stage::Complete:
            return buyEnabled && sellEnabled ? Action::Complete : Action::None;
        }
        return Action::None;
    }
    void invoked(Action action)
    {
        if (action == Action::Buy) stage_ = Stage::BuyStarted;
        if (action == Action::Sell) stage_ = Stage::SellStarted;
    }
private:
    enum class Stage { Buy, BuyStarted, Sell, SellStarted, Complete };
    Stage stage_ = Stage::Buy;
};
