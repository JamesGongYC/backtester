#pragma once

#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hft {

using Ns = std::uint64_t;
using Price = std::int64_t;
constexpr Ns second = 1'000'000'000;
constexpr Price price_scale = 1'000'000'000;
constexpr Ns undefined_time = std::numeric_limits<Ns>::max();
constexpr Price undefined_price = std::numeric_limits<Price>::max();
constexpr unsigned last = 0x80;
constexpr unsigned snapshot = 0x20;
constexpr unsigned bad_book = 0x04;

struct Record {
    Ns event, receive;
    std::uint64_t id;
    Price price;
    std::uint32_t instrument, size, sequence;
    std::uint16_t publisher;
    unsigned flags;
    char action, side;
};

template<class T>
T number(std::string_view value) {
    T result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("Invalid integer: " + std::string(value));
    return result;
}

inline Ns timestamp(std::string_view value) {
    if (value.empty()) return undefined_time;
    if (value.find('-') == std::string_view::npos) return number<Ns>(value);
    if (value.ends_with("Z")) value.remove_suffix(1);
    else if (value.ends_with("+00:00")) value.remove_suffix(6);

    if (value.size() < 19 || value[4] != '-' || value[7] != '-' ||
        (value[10] != 'T' && value[10] != ' ') || value[13] != ':' || value[16] != ':')
        throw std::runtime_error("Expected UTC timestamp: " + std::string(value));
    using namespace std::chrono;
    const auto date = year(number<int>(value.substr(0, 4))) /
                      number<unsigned>(value.substr(5, 2)) / number<unsigned>(value.substr(8, 2));
    const auto days = sys_days(date).time_since_epoch().count();
    const auto hour = number<unsigned>(value.substr(11, 2));
    const auto minute = number<unsigned>(value.substr(14, 2));
    const auto sec = number<unsigned>(value.substr(17, 2));
    if (!date.ok() || days < 0 || hour > 23 || minute > 59 || sec > 59)
        throw std::runtime_error("Invalid UTC date/time: " + std::string(value));

    Ns fraction = 0;
    if (value.size() > 19) {
        if (value[19] != '.' || value.size() == 20 || value.size() > 29)
            throw std::runtime_error("Expected a fraction of at most nine digits in the timestamp");
        fraction = number<Ns>(value.substr(20));
        for (std::size_t i = value.size() - 20; i < 9; ++i) fraction *= 10;
    }
    const Ns seconds = static_cast<Ns>(days) * 86'400 + hour * 3'600 + minute * 60 + sec;
    if (seconds > (undefined_time - 1 - fraction) / second)
        throw std::runtime_error("UTC timestamp exceeds the nanosecond range");
    return seconds * second + fraction;
}

inline Price csv_price(std::string_view value, bool fixed) {
    if (value.empty()) return undefined_price;
    if (fixed) return number<Price>(value);

    // Databento's pretty CSV prices are decimal dollars, including integer dollars.
    const auto dot = value.find('.');
    const auto whole = number<Price>(value.substr(0, dot));
    Price fraction = 0;
    if (dot != std::string_view::npos) {
        const auto digits = value.substr(dot + 1);
        if (digits.empty() || digits.size() > 9 || digits.find_first_not_of("0123456789") != digits.npos)
            throw std::runtime_error("Invalid decimal price: " + std::string(value));
        fraction = number<Price>(digits);
        for (auto count = digits.size(); count < 9; ++count) fraction *= 10;
    }
    constexpr auto limit = (undefined_price - price_scale) / price_scale;
    if (whole < -limit || whole > limit)
        throw std::runtime_error("Decimal price outside the supported range; use --fixed-price for raw CSV");
    return whole * price_scale + (value.front() == '-' ? -fraction : fraction);
}

class CsvReader {
    enum Column { event, receive, id, price, instrument, size, sequence, publisher, flags, action, side, count };
    std::array<std::size_t, count> indices_{};
    std::istream& input_;
    bool fixed_;
    std::string line_;
    std::vector<std::string_view> fields_;
    std::size_t row_ = 1;

    void split() {
        if (!line_.empty() && line_.back() == '\r') line_.pop_back();
        fields_.clear();
        std::string_view line(line_);
        for (;;) {
            const auto comma = line.find(',');
            auto field = line.substr(0, comma);
            if (field.size() >= 2 && field.front() == '"' && field.back() == '"')
                field = field.substr(1, field.size() - 2);
            fields_.push_back(field);
            if (comma == line.npos) break;
            line.remove_prefix(comma + 1);
        }
    }

    std::string_view field(Column column) const {
        const auto index = indices_[column];
        if (index >= fields_.size()) throw std::runtime_error("Missing field");
        return fields_[index];
    }

public:
    CsvReader(std::istream& input, bool fixed) : input_(input), fixed_(fixed) {
        if (!std::getline(input_, line_)) throw std::runtime_error("Missing CSV header");
        if (line_.starts_with("\xEF\xBB\xBF")) line_.erase(0, 3);
        split();
        constexpr std::array<std::string_view, count> names{
            "ts_event", "ts_recv", "order_id", "price", "instrument_id", "size",
            "sequence", "publisher_id", "flags", "action", "side"
        };
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::size_t j = 0;
            while (j < fields_.size() && fields_[j] != names[i]) ++j;
            if (j == fields_.size()) throw std::runtime_error("Missing CSV column: " + std::string(names[i]));
            indices_[i] = j;
        }
    }

    bool next(Record& record) {
        if (!std::getline(input_, line_)) {
            if (!input_.eof() || input_.bad()) throw std::runtime_error("CSV read failed");
            return false;
        }
        ++row_;
        split();
        try {
            if (field(action).size() != 1 || field(side).size() != 1)
                throw std::runtime_error("Expected single-character action and side");
            const auto act = field(action).front();
            const bool uses_price = act == 'A' || act == 'M' || act == 'F' || act == 'T';
            record = {timestamp(field(event)), timestamp(field(receive)), number<std::uint64_t>(field(id)),
                      uses_price ? csv_price(field(price), fixed_) : undefined_price,
                      number<std::uint32_t>(field(instrument)), number<std::uint32_t>(field(size)),
                      number<std::uint32_t>(field(sequence)), number<std::uint16_t>(field(publisher)),
                      number<std::uint8_t>(field(flags)), act, field(side).front()};
        } catch (const std::exception& error) {
            throw std::runtime_error("CSV row " + std::to_string(row_) + ": " + error.what());
        }
        return true;
    }
};

class DbnReader {
    std::istream& input_;
    std::array<char, 1020> bytes_{};

    template<class T>
    T get(std::size_t offset) const {
        static_assert(std::endian::native == std::endian::little);
        T value;
        std::memcpy(&value, bytes_.data() + offset, sizeof(T));
        return value;
    }

    void read(std::size_t offset, std::size_t size) {
        if (!input_.read(bytes_.data() + offset, static_cast<std::streamsize>(size)))
            throw std::runtime_error("Truncated DBN stream");
    }

public:
    explicit DbnReader(std::istream& input) : input_(input) {
        read(0, 8);
        if (std::string_view(bytes_.data(), 3) != "DBN" || get<unsigned char>(3) < 1 ||
            get<unsigned char>(3) > 3)
            throw std::runtime_error("Expected uncompressed DBN version 1, 2, or 3");
        constexpr std::size_t dataset_length = 16;
        const auto length = get<std::uint32_t>(4);
        if (length < 100) throw std::runtime_error("Invalid DBN metadata length");
        read(0, dataset_length);
        if (std::string_view(bytes_.data(), 10) != std::string_view("XNAS.ITCH\0", 10))
            throw std::runtime_error("Expected the XNAS.ITCH dataset");
        if (!input_.ignore(length - dataset_length) || input_.gcount() != length - dataset_length)
            throw std::runtime_error("Truncated DBN metadata");
    }

    bool next(Record& result) {
        const auto words = input_.get();
        if (words == std::char_traits<char>::eof()) {
            if (!input_.eof() || input_.bad()) throw std::runtime_error("DBN read failed");
            return false;
        }
        const auto size = static_cast<std::size_t>(words) * 4;
        if (size < 16) throw std::runtime_error("Invalid DBN record length");
        read(1, size - 1);
        if (get<unsigned char>(1) != 0xa0 || (size != 56 && size != 64))
            throw std::runtime_error("Expected a historical MBO record in the DBN file");
        result = {get<Ns>(8), get<Ns>(40), get<std::uint64_t>(16), get<Price>(24),
                  get<std::uint32_t>(4), get<std::uint32_t>(32), get<std::uint32_t>(52),
                  get<std::uint16_t>(2), get<unsigned char>(36), bytes_[38], bytes_[39]};
        return true;
    }
};

} // namespace hft
