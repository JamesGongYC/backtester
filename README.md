# Nasdaq MBO backtester

This replays a day of AAPL orders on Nasdaq and simulates a simple market-making strategy. It reads Databento `XNAS.ITCH` data, rebuilds the book in file order, and uses the premarket messages to build the opening book.

## How it works

- A hash map finds resting orders by ID. An ordered map keeps prices sorted on each side, and arrival numbers track which orders are ahead in the queue.
- In a `T → F → C` execution, the simulator reads `F` as the fill and takes shares out of the historical book once, on `C`.
- The strategy quotes at the best bid and ask with an inventory cap and assumes instant order placement and cancellation. The run reports simulated fills, inventory, and P&L. Add `--fills` to save the fills and markouts.

## Run it

```sh
make
./backtester path/to/aapl.dbn --date 2026-09-21 --fills fills.csv
```

Use an uncompressed DBN v1–v3 file or a Databento MBO CSV. Pass the date in Eastern time and use `--close 13:00` on an early-close day. Run `./backtester --help` for the other options.

To time the book replay on its own, run it with the strategy disabled.

```sh
./backtester path/to/aapl.dbn --date 2026-09-21 --benchmark --replay-only
```

`make test` checks the replay and fill logic with small fixtures. The goal is to replay a full day in under a second. That still needs a benchmark on a full AAPL file.
