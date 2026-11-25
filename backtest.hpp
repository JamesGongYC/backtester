#pragma once

#include "mbo.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>

namespace hft {

struct Order {
    Price price;
    std::uint64_t priority;
    std::uint32_t remaining;
    char side;
};

struct Top {
    Price price;
    std::uint64_t size;
};

class Book {
    using Levels = std::map<Price, std::uint64_t>;
    Levels bids_, asks_;
    std::unordered_map<std::uint64_t, Order> orders_;
    std::uint64_t priority_ = 0;

    Levels& levels(char side) { return side == 'B' ? bids_ : asks_; }

    void add(const Record& record) {
        const auto [_, inserted] = orders_.emplace(
            record.id, Order{record.price, ++priority_, record.size, record.side});
        if (!inserted) throw std::runtime_error("Duplicate add for order " + std::to_string(record.id));
        levels(record.side)[record.price] += record.size;
    }

public:
    Book() { orders_.reserve(65'536); }

    std::optional<Top> bid() const {
        if (bids_.empty()) return {};
        const auto [price, size] = *bids_.rbegin();
        return Top{price, size};
    }

    std::optional<Top> ask() const {
        if (asks_.empty()) return {};
        const auto [price, size] = *asks_.begin();
        return Top{price, size};
    }

    std::size_t count() const { return orders_.size(); }
    std::uint64_t priority() const { return priority_; }

    const Order* lookup(std::uint64_t id) const {
        const auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    template<class OnRemove>
    void apply(const Record& record, OnRemove on_remove) {
        switch (record.action) {
        case 'T': case 'F': case 'N':
            return;
        case 'R':
            orders_.clear();
            bids_.clear();
            asks_.clear();
            return;
        case 'A': case 'C': case 'M':
            break;
        default:
            throw std::runtime_error("Unknown MBO action");
        }

        if (record.side != 'B' && record.side != 'A')
            throw std::runtime_error("Expected a bid or ask on a book update");
        if (record.action != 'C' && (record.price <= 0 || record.price == undefined_price))
            throw std::runtime_error("Missing or invalid order price");
        if (record.action != 'M' && record.size == 0)
            throw std::runtime_error("Zero-size add or cancel");
        if (record.action == 'A') {
            add(record);
            return;
        }

        auto entry = orders_.find(record.id);
        if (entry == orders_.end() && record.action == 'M' && record.size != 0) {
            add(record);
            return;
        }
        if (entry == orders_.end())
            throw std::runtime_error("Missing resting order " + std::to_string(record.id) +
                                     "; supply a starting snapshot or the earlier book events");

        auto& order = entry->second;
        if (record.side != order.side) throw std::runtime_error("Resting order changed sides");
        if (record.action == 'C' && record.size > order.remaining)
            throw std::runtime_error("Cancel exceeds resting order quantity");
        const bool replaced = record.action == 'M' &&
            (record.price != order.price || record.size > order.remaining);
        const auto removed = record.action == 'C' ? record.size :
            replaced ? order.remaining : order.remaining - record.size;

        auto& side = levels(order.side);
        auto level = side.find(order.price);
        on_remove(order, removed);
        level->second -= removed;
        order.remaining -= removed;
        if (level->second == 0) side.erase(level);
        if (order.remaining == 0) orders_.erase(entry);
        if (replaced && record.size) add(record);
    }
};

struct Session {
    Ns open;
    Ns close;
};

struct Settings {
    std::uint32_t quote_size = 100;
    std::uint32_t max_inventory = 500;
    Ns markout = 60 * second;
    long double fee = 0;
    bool replay_only = false;
};

struct Quote {
    Price price;
    std::uint64_t ahead;
    std::uint64_t posted_after;
    std::uint32_t remaining;
};

struct Fill {
    Ns time;
    Ns exchange_time;
    std::uint32_t sequence;
    char side;
    Price price;
    std::uint32_t size;
    std::int64_t inventory;
    long double cash;
    std::optional<long double> markout;
};

struct Counts {
    std::uint64_t warmup_records = 0;
    std::uint64_t session_records = 0;
    std::uint64_t session_events = 0;
    std::uint64_t volume = 0;
};

class Backtest {
    Book book_;
    Session session_;
    Settings settings_;
    std::optional<Quote> bid_, ask_;
    std::optional<long double> last_mid_;
    Ns clock_ = 0;
    std::size_t next_markout_ = 0;
    bool pending_event_ = false;
    bool active_event_ = false;

    std::optional<Quote>& quote(char side) { return side == 'B' ? bid_ : ask_; }

    void subtract_ahead(const Order& order, std::uint32_t removed) {
        auto& resting = quote(order.side);
        if (resting && order.price == resting->price && order.priority <= resting->posted_after) {
            if (removed > resting->ahead) throw std::runtime_error("Inconsistent simulated queue");
            resting->ahead -= removed;
        }
    }

    void passive_fill(const Record& record) {
        const auto* historical = book_.lookup(record.id);
        if (!historical)
            throw std::runtime_error("Fill for missing resting order " + std::to_string(record.id));
        auto& resting = quote(historical->side);
        if (!resting || resting->price != historical->price || resting->ahead != 0 ||
            historical->priority <= resting->posted_after)
            return;
        if (record.size > historical->remaining)
            throw std::runtime_error("Fill exceeds historical resting order quantity");
        const auto amount = std::min(resting->remaining, record.size);
        if (!amount) return;
        const auto signed_amount = (historical->side == 'B' ? 1 : -1) * std::int64_t(amount);
        inventory += signed_amount;
        cash -= signed_amount * static_cast<long double>(resting->price) / price_scale;
        cash -= settings_.fee * amount;
        counts.volume += amount;
        fills.push_back({clock_, record.event, record.sequence, historical->side,
                         resting->price, amount, inventory, cash, {}});
        resting->remaining -= amount;
        if (resting->remaining == 0) resting.reset();
    }

    void sync_quote(std::optional<Quote>& resting, Top top, std::int64_t room) {
        if (room <= 0) {
            resting.reset();
            return;
        }
        if (resting && resting->price == top.price) {
            resting->remaining = static_cast<std::uint32_t>(std::min<std::int64_t>(resting->remaining, room));
            return;
        }
        const auto size = static_cast<std::uint32_t>(std::min<std::int64_t>(settings_.quote_size, room));
        resting = Quote{top.price, top.size, book_.priority(), size};
    }

    void after_event(bool active) {
        if (settings_.replay_only) return;
        const auto bid = book_.bid();
        const auto ask = book_.ask();
        if (!active || !bid || !ask || bid->price >= ask->price) {
            bid_.reset();
            ask_.reset();
            return;
        }

        last_mid_ = (static_cast<long double>(bid->price) + ask->price) / (2 * price_scale);
        while (next_markout_ < fills.size()) {
            auto& fill = fills[next_markout_];
            if (clock_ - fill.time < settings_.markout) break;
            const auto sign = fill.side == 'B' ? 1 : -1;
            fill.markout = sign * (*last_mid_ - static_cast<long double>(fill.price) / price_scale);
            ++next_markout_;
        }

        // Inside-quote baseline. Existing orders retain their queue when their price is unchanged.
        sync_quote(bid_, *bid, std::int64_t(settings_.max_inventory) - inventory);
        sync_quote(ask_, *ask, std::int64_t(settings_.max_inventory) + inventory);
    }

public:
    Counts counts;
    std::vector<Fill> fills;
    std::int64_t inventory = 0;
    long double cash = 0;

    Backtest(Session session, Settings settings) : session_(session), settings_(settings) {}

    bool apply(const Record& record) {
        if (!pending_event_) {
            const bool is_snapshot = (record.flags & snapshot) != 0;
            if (!is_snapshot && (record.event == undefined_time || record.event == 0))
                throw std::runtime_error("Missing exchange timestamp");
            if (!is_snapshot && record.event >= session_.close) return false;
            active_event_ = !is_snapshot && record.event >= session_.open;
            if (active_event_) clock_ = std::max(clock_, record.event);
        }
        if (record.flags & bad_book) throw std::runtime_error("Feed reports an unrecoverable book gap");
        if (record.flags & (0x40 | 0x10))
            throw std::runtime_error("Top-of-book or price-level record in order-level input");
        pending_event_ = !(record.flags & last);
        const bool active = active_event_;
        if (active) {
            ++counts.session_records;
        } else {
            ++counts.warmup_records;
            bid_.reset();
            ask_.reset();
        }

        if (record.action == 'R') {
            bid_.reset();
            ask_.reset();
        }
        if (active && !settings_.replay_only && record.action == 'F') passive_fill(record);
        book_.apply(record, [this](const Order& order, std::uint32_t amount) {
            subtract_ahead(order, amount);
        });

        if (!pending_event_) {
            if (active) ++counts.session_events;
            after_event(active);
        }
        return true;
    }

    void finish() const {
        if (pending_event_) throw std::runtime_error("Input ended inside an MBO event (missing F_LAST)");
        if (!counts.session_records) throw std::runtime_error("No regular-session records for the selected date and instrument");
    }

    const Book& book() const { return book_; }
    std::optional<long double> close_mid() const { return last_mid_; }
};

} // namespace hft
