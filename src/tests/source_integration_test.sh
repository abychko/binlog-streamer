#!/usr/bin/env bash
# Usage: source_integration_test.sh <path-to-binary>
# Skipped when BINLOG_STREAMER_TEST_SOURCE_YML is unset, or the binary is not a
# developer-mode build.
set -u

BINARY="${1:?usage: source_integration_test.sh <path-to-binary>}"

if [ -z "${BINLOG_STREAMER_TEST_SOURCE_YML:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_SOURCE_YML is not set"
    exit 0
fi

VERSION_TEXT="$("$BINARY" --version 2>&1)"
case "$VERSION_TEXT" in
    *"(developer mode)"*) ;;
    *)
        echo "skipped: not a developer-mode build"
        exit 0
        ;;
esac

SOURCE_YML="$BINLOG_STREAMER_TEST_SOURCE_YML"
if [ ! -e "$SOURCE_YML" ]; then
    echo "FATAL: BINLOG_STREAMER_TEST_SOURCE_YML=$SOURCE_YML does not exist"
    exit 1
fi

# storage.data_dir must be a real, writable, throwaway directory: a file left
# there by an earlier run makes a fresh relay refuse to start.
WORKDIR="$(mktemp -d)"
chmod 700 "$WORKDIR"
DATA_DIR="$WORKDIR/data"
mkdir -p "$DATA_DIR"

cp "$SOURCE_YML" "$WORKDIR/source.yml"
chmod 640 "$WORKDIR/source.yml"

# server_id distinct from the other integration tests sharing the source.
cat > "$WORKDIR/settings.yml" <<SETTINGS
server:
  server_id: 999003
storage:
  data_dir: $DATA_DIR
  retention:
    policy: age
    period: 30d
  disk:
    max_size: 2T
    purge_high_watermark: 1900G
    purge_low_watermark: 1800G
    min_free_space: 1M
    recovery_reserve: 10G
cache:
  policy: time_window
  window: 12h
  max_size: 1T
SETTINGS
chmod 640 "$WORKDIR/settings.yml"
SETTINGS_YML="$WORKDIR/settings.yml"

STDERR_LOG="$(mktemp)"
cleanup() {
    if [ -n "${RELAY_PID:-}" ] && kill -0 "$RELAY_PID" 2>/dev/null; then
        kill -KILL "$RELAY_PID" 2>/dev/null
    fi
    rm -rf "$WORKDIR" "$STDERR_LOG"
}
trap cleanup EXIT

"$BINARY" --config "$SETTINGS_YML" >/dev/null 2>"$STDERR_LOG" &
RELAY_PID=$!

DUMP_STARTED=0
for _ in $(seq 1 300); do
    if grep -q "dump requested" "$STDERR_LOG" 2>/dev/null; then
        DUMP_STARTED=1
        break
    fi
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if [ "$DUMP_STARTED" -ne 1 ]; then
    echo "FATAL: 'dump requested' did not appear in stderr within 30s; stderr:"
    cat "$STDERR_LOG"
    exit 1
fi

kill -TERM "$RELAY_PID"

EXITED=0
for _ in $(seq 1 100); do
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        EXITED=1
        break
    fi
    sleep 0.1
done

if [ "$EXITED" -ne 1 ]; then
    echo "FATAL: relay did not exit within 10s of SIGTERM; stderr so far:"
    cat "$STDERR_LOG"
    exit 1
fi

wait "$RELAY_PID"
EXIT_CODE=$?

STDERR_TEXT="$(cat "$STDERR_LOG")"
if [ "$EXIT_CODE" -ne 0 ]; then
    echo "FATAL: expected exit code 0 after SIGTERM, got $EXIT_CODE; stderr:"
    echo "$STDERR_TEXT"
    exit 1
fi
if ! printf '%s' "$STDERR_TEXT" | grep -q "^binlog-streamer: stopped:"; then
    echo "FATAL: expected a 'stopped:' line in stderr, got:"
    echo "$STDERR_TEXT"
    exit 1
fi
if ! printf '%s' "$STDERR_TEXT" | grep -q "^binlog-streamer: first event " ||
   ! printf '%s' "$STDERR_TEXT" | grep -q "^binlog-streamer: last event " ||
   ! printf '%s' "$STDERR_TEXT" | grep -q "^binlog-streamer: events="; then
    echo "FATAL: expected first/last event and counter summary lines in stderr, got:"
    echo "$STDERR_TEXT"
    exit 1
fi

echo "OK: dump started, SIGTERM produced exit 0 with a full summary"
echo "$STDERR_TEXT"
