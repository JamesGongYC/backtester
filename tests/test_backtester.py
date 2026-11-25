"""End-to-end tests for Databento ingest and order/queue accounting."""

import csv
from datetime import datetime, timezone
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BINARY = Path(sys.argv[1]).resolve()
NS = 1_000_000_000
COLUMNS = ("ts_event", "ts_recv", "order_id", "price", "instrument_id", "size",
           "sequence", "publisher_id", "flags", "action", "side")


def ns(utc):
    return int(datetime.fromisoformat(utc + "+00:00").timestamp()) * NS


def make_records(open_time, close_time):
    def row(offset, sequence, action, side, order=0, price=0, size=0, flags=128):
        timestamp = open_time + offset
        return (timestamp, timestamp + 87, order, price, 42, size, sequence, 2, flags, action, side)

    return [
        row(-10 * NS, 0, "R", "N", flags=160),
        row(-9 * NS, 1, "A", "B", 1, 100, 100),
        row(-8 * NS, 2, "A", "A", 2, 101, 100),
        row(-7 * NS, 3, "A", "B", 3, 99, 150),
        row(0, 4, "N", "N"),
        row(1, 5, "A", "B", 4, 100, 100),
        row(2, 6, "T", "A", price=100, size=30, flags=0),
        row(2, 6, "F", "B", 1, 100, 30, flags=0),
        row(2, 6, "C", "B", 1, 100, 30),
        row(3, 7, "C", "B", 1, 100, 70),
        row(4, 8, "T", "N", price=100, size=500),
        row(5, 9, "T", "A", price=100, size=40, flags=0),
        row(5, 9, "F", "B", 4, 100, 40, flags=0),
        row(5, 9, "C", "B", 4, 100, 40),
        row(6, 10, "F", "B", 4, 100, 60, flags=0),
        row(6, 10, "C", "B", 4, 100, 60),
        row(2 * NS, 11, "N", "N"),
        row(close_time - open_time, 12, "T", "N", flags=0),
        row(close_time - open_time, 12, "N", "N"),
    ]


def write_csv(path, records, pretty=False):
    with path.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(COLUMNS)
        for row in records:
            row = list(row)
            if pretty:
                for col in (0, 1):
                    seconds, nano = divmod(row[col], NS)
                    whole = datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
                    row[col] = f"{whole}.{nano:09}Z"
            writer.writerow(row)


def write_dbn(path, records, version=3):
    metadata = b"XNAS.ITCH".ljust(16, b"\0") + bytes(100)
    with path.open("wb") as output:
        output.write(b"DBN" + bytes([version]) + struct.pack("<I", len(metadata)) + metadata)
        for event, receive, order, price, instrument, size, seq, pub, flags, action, side in records:
            output.write(struct.pack("<BBHIQQqIBBccQiI", 14, 0xA0, pub, instrument, event,
                                     order, price * NS, size, flags, 0, action.encode(),
                                     side.encode(), receive, 87, seq))


def run(path, date, *args, check=True):
    completed = subprocess.run([BINARY, path, "--date", date, *args],
                               capture_output=True, text=True, check=check)
    if not check:
        return completed
    return dict(line.split("=", 1) for line in completed.stdout.splitlines())


def assert_result(result, fills_path, open_time, close_time):
    assert int(result["session_open_utc_ns"]) == open_time
    assert int(result["session_close_utc_ns"]) == close_time
    assert result["warmup_records"] == "4"
    assert result["session_records"] == "13"
    assert result["session_events"] == "8"
    assert result["resting_orders"] == "2"
    assert result["best_bid_shares"] == "150"
    assert result["best_bid_usd"] == "99.000000000"
    assert result["fills"] == "2"
    assert result["filled_shares"] == "100"
    assert result["end_inventory"] == "100"
    assert result["marked_pnl_usd"] == "-1.000000000"
    with fills_path.open() as output:
        fills = list(csv.DictReader(output))
    assert [(x["sequence"], x["side"], x["shares"]) for x in fills] == [
        ("9", "B", "40"), ("10", "B", "60")
    ]
    assert all(x["markout_per_share_usd"] == "0.000000000" for x in fills)
    assert int(fills[0]["ts_event_ns"]) == open_time + 5


def main():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        fills = root / "fills.csv"
        input_csv = root / "sample.csv"
        input_dbn = root / "sample.dbn"
        date = "2026-09-21"
        open_time = ns(date + "T13:30:00")
        close_time = ns(date + "T20:00:00")
        rows = make_records(open_time, close_time)
        write_csv(input_csv, rows, pretty=True)
        result = run(input_csv, date, "--fills", fills, "--markout-seconds", "1", "--fee", "0.01")
        assert_result(result, fills, open_time, close_time)

        for version in (1, 2, 3):
            write_dbn(input_dbn, rows, version)
            result = run(input_dbn, date, "--benchmark", "--fills", fills,
                         "--markout-seconds", "1", "--fee", "0.01")
            assert_result(result, fills, open_time, close_time)
            assert "decode_ms" in result and "replay_ms" in result

        streamed = subprocess.run([BINARY, "-", "--format", "dbn", "--date", date,
                                   "--benchmark", "--replay-only"],
                                  input=input_dbn.read_bytes(), capture_output=True, check=True)
        assert b"best_bid_shares=150" in streamed.stdout

        fixed_rows = [row[:3] + (row[3] * NS,) + row[4:] for row in rows]
        write_csv(input_csv, fixed_rows)
        result = run(input_csv, date, "--fixed-price", "--fills", fills,
                     "--markout-seconds", "1", "--fee", "0.01")
        assert_result(result, fills, open_time, close_time)

        date = "2026-11-27"
        open_time = ns(date + "T14:30:00")
        close_time = ns(date + "T18:00:00")
        rows = make_records(open_time, close_time)
        write_csv(input_csv, rows)
        result = run(input_csv, date, "--close", "13:00", "--replay-only")
        assert int(result["session_open_utc_ns"]) == open_time
        assert int(result["session_close_utc_ns"]) == close_time
        assert "fills" not in result
        assert result["best_bid_shares"] == "150"

        write_csv(input_csv, rows[:15])
        result = run(input_csv, date, check=False)
        assert result.returncode and "missing F_LAST" in result.stderr

        # An unknown cancel must fail loudly rather than silently corrupting queue state.
        broken = list(rows)
        broken[9] = tuple(999 if index == 2 else item for index, item in enumerate(rows[9]))
        write_csv(input_csv, broken)
        result = run(input_csv, date, check=False)
        assert result.returncode and "Missing resting order 999" in result.stderr


if __name__ == "__main__":
    main()
    print("Backtester integration tests passed")
