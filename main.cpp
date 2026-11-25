#include "backtest.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {

using namespace hft;

constexpr std::string_view usage =
    "Usage: backtester INPUT --date YYYY-MM-DD [options]\n"
    "  INPUT                  Databento XNAS.ITCH MBO .dbn or .csv, or - for stdin\n"
    "  --format dbn|csv       Required for stdin; otherwise inferred from .dbn or .csv\n"
    "  --instrument ID        Select one instrument from a multi-instrument file\n"
    "  --close HH:MM          Eastern close on an early-close day (default 16:00)\n"
    "  --fixed-price          CSV prices are raw integer units of 1e-9 USD\n"
    "  --replay-only          Rebuild the historical book without simulating orders\n"
    "  --benchmark            Load into memory first and time replay separately\n"
    "  --quote-size SHARES    Resting size per side (default 100)\n"
    "  --max-inventory SHARES Maximum absolute position (default 500)\n"
    "  --fee USD              Fee per filled share; negative for a rebate (default 0)\n"
    "  --markout-seconds N    Signed fill markout horizon (default 60)\n"
    "  --fills PATH           Write virtual fills and markouts as CSV\n";

struct Options {
    std::string input, format, date, fills;
    std::string close = "16:00";
    std::optional<std::uint32_t> instrument;
    bool fixed = false;
    bool benchmark = false;
    Settings settings;
};

Options options(int argc, char** argv) {
    if (argc < 2) throw std::runtime_error("Missing input file; use --help");
    Options result;
    result.input = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        auto value = [&]() -> std::string_view {
            if (++i >= argc) throw std::runtime_error("Missing argument for " + std::string(flag));
            return argv[i];
        };
        if (flag == "--date") result.date = value();
        else if (flag == "--format") result.format = value();
        else if (flag == "--close") result.close = value();
        else if (flag == "--instrument") result.instrument = number<std::uint32_t>(value());
        else if (flag == "--fixed-price") result.fixed = true;
        else if (flag == "--benchmark") result.benchmark = true;
        else if (flag == "--replay-only") result.settings.replay_only = true;
        else if (flag == "--quote-size") result.settings.quote_size = number<std::uint32_t>(value());
        else if (flag == "--max-inventory") result.settings.max_inventory = number<std::uint32_t>(value());
        else if (flag == "--markout-seconds") {
            const auto seconds = number<Ns>(value());
            if (seconds > undefined_time / second) throw std::runtime_error("Markout horizon overflows nanoseconds");
            result.settings.markout = seconds * second;
        } else if (flag == "--fee") {
            const auto fee = std::string(value());
            std::size_t used = 0;
            result.settings.fee = std::stold(fee, &used);
            if (used != fee.size() || !std::isfinite(result.settings.fee))
                throw std::runtime_error("Invalid per-share fee");
        } else if (flag == "--fills") result.fills = value();
        else throw std::runtime_error("Unknown option: " + std::string(flag));
    }
    if (result.date.size() != 10) throw std::runtime_error("--date must be YYYY-MM-DD");
    if (!result.settings.replay_only && (!result.settings.quote_size || !result.settings.max_inventory))
        throw std::runtime_error("Quote size and max inventory must be positive");
    if (result.settings.replay_only && !result.fills.empty())
        throw std::runtime_error("--fills requires the simulated quoting mode");
    if (result.format.empty()) {
        if (result.input.ends_with(".dbn")) result.format = "dbn";
        if (result.input.ends_with(".csv")) result.format = "csv";
    }
    if (result.format != "dbn" && result.format != "csv")
        throw std::runtime_error("Select --format dbn or csv; decompress .zst files first or pipe zstd output");
    if (result.fixed && result.format != "csv")
        throw std::runtime_error("--fixed-price only applies to CSV");
    return result;
}

Ns eastern_boundary(const std::string& date, const std::string& time) {
    if (time.size() != 5) throw std::runtime_error("Session time must be HH:MM");
    // Validate the fields before mktime, which otherwise normalizes invalid input.
    timestamp(date + "T" + time + ":00Z");
    std::tm local{};
    local.tm_year = number<int>(std::string_view(date).substr(0, 4)) - 1900;
    local.tm_mon = number<int>(std::string_view(date).substr(5, 2)) - 1;
    local.tm_mday = number<int>(std::string_view(date).substr(8, 2));
    local.tm_hour = number<int>(std::string_view(time).substr(0, 2));
    local.tm_min = number<int>(std::string_view(time).substr(3, 2));
    local.tm_isdst = -1;
    const auto utc = std::mktime(&local);
    if (utc < 0 || static_cast<Ns>(utc) > (undefined_time - 1) / second)
        throw std::runtime_error("Eastern session boundary is outside the UTC nanosecond range");
    return static_cast<Ns>(utc) * second;
}

class SelectInstrument {
    std::optional<std::uint32_t> requested_;
    std::optional<std::uint16_t> publisher_;
    std::optional<std::uint32_t> instrument_;

public:
    explicit SelectInstrument(std::optional<std::uint32_t> requested) : requested_(requested) {}

    bool accept(const Record& record) {
        if (requested_ && record.instrument != *requested_) return false;
        if (!instrument_) {
            instrument_ = record.instrument;
            publisher_ = record.publisher;
        } else if (record.instrument != *instrument_ || record.publisher != *publisher_) {
            throw std::runtime_error("Input contains multiple instruments or publishers; select one XNAS instrument");
        }
        return true;
    }

    std::uint32_t id() const { return instrument_.value(); }
};

using Steady = std::chrono::steady_clock;
double milliseconds(Steady::time_point from, Steady::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

template<class Reader>
void run(Reader& reader, SelectInstrument& selection, Backtest& test, bool benchmark) {
    Record record{};
    std::uint64_t position = 0;
    const auto start = Steady::now();
    auto apply = [&](const Record& row) {
        try {
            return test.apply(row);
        } catch (const std::exception& error) {
            throw std::runtime_error("Selected record " + std::to_string(position) + " (sequence " +
                                     std::to_string(row.sequence) + "): " + error.what());
        }
    };

    if (benchmark) {
        std::vector<Record> loaded;
        while (reader.next(record)) {
            if (selection.accept(record)) loaded.push_back(record);
        }
        const auto decoded = Steady::now();
        for (const auto& row : loaded) {
            ++position;
            if (!apply(row)) break;
        }
        test.finish();
        const auto end = Steady::now();
        std::cout << "decode_ms=" << milliseconds(start, decoded) << '\n'
                  << "replay_ms=" << milliseconds(decoded, end) << '\n';
    } else {
        while (reader.next(record)) {
            if (!selection.accept(record)) continue;
            ++position;
            if (!apply(record)) break;
        }
        test.finish();
        std::cout << "decode_and_replay_ms=" << milliseconds(start, Steady::now()) << '\n';
    }
}

void write_fills(const std::string& path, const std::vector<Fill>& fills) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot open fills output: " + path);
    out << "simulation_ts_ns,ts_event_ns,sequence,side,price_usd,shares,inventory,cash_usd,markout_per_share_usd\n";
    out << std::fixed << std::setprecision(9);
    for (const auto& fill : fills) {
        out << fill.time << ',' << fill.exchange_time << ',' << fill.sequence << ',' << fill.side
            << ',' << static_cast<long double>(fill.price) / price_scale << ',' << fill.size
            << ',' << fill.inventory << ',' << fill.cash << ',';
        if (fill.markout) out << *fill.markout;
        out << '\n';
    }
    out.close();
    if (!out) throw std::runtime_error("Failed to write fills output: " + path);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << usage;
            return 0;
        }
        const Options config = options(argc, argv);
        if (setenv("TZ", "America/New_York", 1) != 0)
            throw std::runtime_error("Cannot set America/New_York timezone");
        tzset();
        const Session session{eastern_boundary(config.date, "09:30"), eastern_boundary(config.date, config.close)};
        if (session.close <= session.open) throw std::runtime_error("Session close must follow 09:30 ET");

        std::ifstream file;
        if (config.input != "-") {
            file.open(config.input, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open input: " + config.input);
        }
        std::ios::sync_with_stdio(false);
        std::cin.tie(nullptr);
        auto& input = config.input == "-" ? std::cin : file;
        Backtest test(session, config.settings);
        SelectInstrument selection(config.instrument);
        std::cout << std::fixed << std::setprecision(9);
        if (config.format == "dbn") {
            DbnReader reader(input);
            run(reader, selection, test, config.benchmark);
        } else {
            CsvReader reader(input, config.fixed);
            run(reader, selection, test, config.benchmark);
        }

        if (!config.fills.empty()) {
            if (config.input != "-" && (config.input == config.fills ||
                (std::filesystem::exists(config.fills) &&
                 std::filesystem::equivalent(config.input, config.fills))))
                throw std::runtime_error("Fills output must differ from input");
            write_fills(config.fills, test.fills);
        }

        std::cout << "instrument_id=" << selection.id() << '\n'
                  << "session_open_utc_ns=" << session.open << '\n'
                  << "session_close_utc_ns=" << session.close << '\n'
                  << "warmup_records=" << test.counts.warmup_records << '\n'
                  << "session_records=" << test.counts.session_records << '\n'
                  << "session_events=" << test.counts.session_events << '\n'
                  << "resting_orders=" << test.book().count() << '\n';
        if (const auto bid = test.book().bid())
            std::cout << "best_bid_usd=" << static_cast<long double>(bid->price) / price_scale
                      << '\n' << "best_bid_shares=" << bid->size << '\n';
        if (const auto ask = test.book().ask())
            std::cout << "best_ask_usd=" << static_cast<long double>(ask->price) / price_scale
                      << '\n' << "best_ask_shares=" << ask->size << '\n';
        if (!config.settings.replay_only) {
            std::cout << "fills=" << test.fills.size() << '\n'
                      << "filled_shares=" << test.counts.volume << '\n'
                      << "end_inventory=" << test.inventory << '\n'
                      << "cash_usd=" << test.cash << '\n';
            if (const auto mid = test.close_mid())
                std::cout << "final_mid_usd=" << *mid << '\n'
                          << "marked_pnl_usd=" << test.cash + test.inventory * *mid << '\n';
            else
                std::cout << "marked_pnl_usd=NA\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "backtester: " << error.what() << '\n';
        return 1;
    }
}
