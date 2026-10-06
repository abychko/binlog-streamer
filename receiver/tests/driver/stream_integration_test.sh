#!/usr/bin/env bash
set -u

DRIVER="${1:?usage: stream_integration_test.sh <driver-binary>}"

if [ -z "${BINLOG_STREAMER_TEST_SOURCE_YML:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_SOURCE_YML is not set"
    exit 0
fi
if [ -z "${BINLOG_STREAMER_TEST_ADMIN_MYSQL:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_ADMIN_MYSQL is not set"
    exit 0
fi

SOURCE_YML="$BINLOG_STREAMER_TEST_SOURCE_YML"
if [ ! -e "$SOURCE_YML" ]; then
    echo "FATAL: BINLOG_STREAMER_TEST_SOURCE_YML=$SOURCE_YML does not exist"
    exit 1
fi

# Deliberately unquoted: it may carry flags.
# shellcheck disable=SC2206
ADMIN=( $BINLOG_STREAMER_TEST_ADMIN_MYSQL )

SCHEMA_SQL="CREATE DATABASE IF NOT EXISTS binlog_streamer_test;
DROP TABLE IF EXISTS binlog_streamer_test.stream_integration_test;
CREATE TABLE binlog_streamer_test.stream_integration_test (
    id INT PRIMARY KEY AUTO_INCREMENT,
    v LONGBLOB
) ENGINE=InnoDB;"
if ! "${ADMIN[@]}" -e "$SCHEMA_SQL"; then
    echo "FATAL: could not prepare the binlog_streamer_test schema"
    exit 1
fi

START_GTID_SET="$("${ADMIN[@]}" -N -e 'SELECT @@GLOBAL.gtid_executed')"
BINLOG_BASENAME="$("${ADMIN[@]}" -N -e "SHOW VARIABLES LIKE 'log_bin_basename'" | awk '{print $2}')"
if [ -z "$BINLOG_BASENAME" ]; then
    echo "FATAL: could not read log_bin_basename from the source"
    exit 1
fi

SINK="$(mktemp)"
OUT="$(mktemp)"
cleanup() {
    if [ -n "${DRIVER_PID:-}" ] && kill -0 "$DRIVER_PID" 2>/dev/null; then
        kill -KILL "$DRIVER_PID" 2>/dev/null
    fi
    rm -f "$SINK" "$OUT"
}
trap cleanup EXIT

"$DRIVER" --source-yml="$SOURCE_YML" --server-id=999001 --start-gtid-set="$START_GTID_SET" \
    --stop-after-gtid-events=5 --wait-heartbeats=1 --heartbeat-period=1 \
    --sink-file="$SINK" --max-seconds=60 >"$OUT" 2>&1 &
DRIVER_PID=$!

sleep 2

INSERT_SQL="INSERT INTO stream_integration_test (v) VALUES ('row2');
INSERT INTO stream_integration_test (v) VALUES ('row3');
INSERT INTO stream_integration_test (v) VALUES ('row4');
INSERT INTO stream_integration_test (v) VALUES (REPEAT('a', 25165824));
INSERT INTO stream_integration_test (v) VALUES ('row6');"
if ! "${ADMIN[@]}" binlog_streamer_test -e "$INSERT_SQL"; then
    echo "FATAL: the five test transactions did not all succeed"
    exit 1
fi

wait "$DRIVER_PID"
DRIVER_EXIT=$?
DRIVER_PID=""
DRIVER_REPORT="$(cat "$OUT")"
echo "$DRIVER_REPORT"

if [ "$DRIVER_EXIT" -ne 0 ]; then
    echo "FATAL: driver exited $DRIVER_EXIT (expected 0 - it did not reach its own stop condition)"
    exit 1
fi

get_field() { echo "$DRIVER_REPORT" | grep -oE "(^| )$1=[^ ]*" | tail -1 | cut -d= -f2; }
FILE="$(get_field file)"
FIRST_OFFSET="$(get_field first_offset)"
LAST_FILE="$(get_field last_file)"
LAST_OFFSET="$(get_field last_offset)"
GTID_EVENTS="$(get_field gtid_events)"
HEARTBEATS="$(get_field heartbeats)"
LARGEST_EVENT_LENGTH="$(get_field largest_event_length)"
LARGEST_EVENT_SUB_PACKETS="$(get_field largest_event_sub_packets)"

FAIL=0
[ "${GTID_EVENTS:-0}" -ge 5 ] || { echo "FATAL: gtid_events=$GTID_EVENTS, expected >= 5"; FAIL=1; }
[ "${HEARTBEATS:-0}" -ge 1 ] || { echo "FATAL: heartbeats=$HEARTBEATS, expected >= 1"; FAIL=1; }
[ "${LARGEST_EVENT_LENGTH:-0}" -gt 16777215 ] ||
    { echo "FATAL: largest_event_length=$LARGEST_EVENT_LENGTH, expected > 16777215"; FAIL=1; }
[ "${LARGEST_EVENT_SUB_PACKETS:-0}" -ge 2 ] ||
    { echo "FATAL: largest_event_sub_packets=$LARGEST_EVENT_SUB_PACKETS, expected >= 2"; FAIL=1; }
[ "$FILE" = "$LAST_FILE" ] ||
    { echo "FATAL: first/last event came from different files ($FILE vs $LAST_FILE) - not one contiguous range"; FAIL=1; }
[ "$FAIL" -eq 0 ] || exit 1

if ! "${ADMIN[@]}" -e "FLUSH BINARY LOGS;"; then
    echo "FATAL: FLUSH BINARY LOGS failed"
    exit 1
fi

SOURCE_FILE_PATH="${BINLOG_BASENAME}.${FILE##*.}"
if [ ! -r "$SOURCE_FILE_PATH" ]; then
    echo "FATAL: cannot read $SOURCE_FILE_PATH"
    exit 1
fi
LENGTH=$((LAST_OFFSET - FIRST_OFFSET))
if ! tail -c +$((FIRST_OFFSET + 1)) "$SOURCE_FILE_PATH" | head -c "$LENGTH" | cmp - "$SINK"; then
    echo "FATAL: cmp against $SOURCE_FILE_PATH (offset $FIRST_OFFSET, length $LENGTH) found a difference"
    exit 1
fi

echo "OK: byte-exact match against $SOURCE_FILE_PATH, offset $FIRST_OFFSET, length $LENGTH"
