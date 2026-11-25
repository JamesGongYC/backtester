# Nasdaq MBO backtester

A C++20 backtester for one instrument and one regular trading session per run. It rebuilds the Nasdaq order book from Databento `XNAS.ITCH` Market by Order records and evaluates an inventory-capped, two-sided quoting baseline. Use it first with AAPL.

## Build and run

On macOS or Linux with a C++20 compiler and Make:

```sh
cd ~/Desktop/quant/HFT/backtester
make
make test
```

Replay a single day with the book only and measure decode and replay separately:

```sh
./backtester /path/to/aapl-mbo.dbn --date 2026-09-21 --benchmark --replay-only
```

Simulate 100-share quotes and write the fill ledger:

```sh
./backtester /path/to/aapl-mbo.csv --date 2026-09-21 \
  --quote-size 100 --max-inventory 500 --markout-seconds 60 \
  --fee 0.001 --fills fills.csv
```

For a compressed DBN file, decompress it first or use standard input. Benchmark mode reads the whole input before replaying:

```sh
zstd -dc /path/to/aapl-mbo.dbn.zst | \
  ./backtester - --format dbn --date 2026-09-21 --benchmark --replay-only
```

Use `--instrument ID` for a file containing multiple symbols. Use `--close 13:00` for a 1 p.m. Eastern early close. `./backtester --help` lists every option.

## Input and event handling

- Supply a day file that includes the UTC-day snapshot or the earlier book updates needed to reconstruct the opening book. The strategy starts at the first completed regular-session event. The replay applies the earlier data as warmup.
- Input supports uncompressed historical DBN v1–v3 and Databento MBO CSV. For newer DBN layouts, export to CSV using Databento's decoder. CSV accepts UTC ISO timestamps with up to nine fractional digits or integer epoch nanoseconds. It expects decimal-dollar prices by default. Use `--fixed-price` if you exported integer price units of 1e-9 dollars.
- The replay uses file order, including equal timestamps and related records in one exchange event. It stores timestamps as unsigned 64-bit UTC nanoseconds. Normal trading hours are 09:30–16:00 in `America/New_York` and are converted to UTC once per run. Use a valid Nasdaq trading date and supply the Eastern close time on early-close dates. The regular session excludes records starting at the close boundary.
- Adds, cancels, modifies, and clears update the book. A Nasdaq `T → F → C` execution uses `F` for simulated fill eligibility and `C` to decrement the historical order. The baseline sees the completed book after `F_LAST`. A missing order on cancel or a reported unrecoverable feed gap fails the run.
- Default mode streams data. `--benchmark` measures decode separately from the in-memory replay, which includes book updates and strategy work when quoting is enabled. The sub-second target should be measured with the intended full-day file.

## Simulation assumptions

The baseline posts virtual orders at the best bid and ask after each completed event. Existing displayed shares at that price start ahead. Later adds join behind. Cancels and executions against earlier orders reduce the queue ahead. Once that queue is empty, a historical passive fill on a later order at the same price supplies simulated fill volume. Quotes retain priority at an unchanged price and rejoin the queue when repriced or replenished.

The model assumes zero quote and cancel latency and leaves the historical market unchanged. It uses displayed queue order and observed passive executions at the quoted price. The baseline applies a configurable fee or rebate and marks final inventory to the last valid regular-session midpoint. A fill's signed per-share markout uses the first valid completed-event midpoint at or after the requested horizon. It is blank if that observation is unavailable within the session.

The implementation provides the replay and simulation foundation for testing flow signals and a Bellman quoting policy. The inside-quote baseline is the strategy included here.

[Databento MBO schema](https://databento.com/docs/schemas/mbo) · [Nasdaq ITCH normalization](https://databento.com/docs/datasets/xnas-itch) · [DBN format](https://databento.com/docs/conventions/databento-binary-encoding) · [Nasdaq trading calendar](https://www.nasdaq.com/market-activity/stock-market-holiday-schedule)
