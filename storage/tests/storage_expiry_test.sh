#!/usr/bin/env bash
set -euo pipefail
BINARY="${1:?usage: storage_expiry_test.sh <path-to-binary>}"
if [ -z "${BINLOG_STREAMER_TEST_SOURCE_YML:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_SOURCE_YML is not set"; exit 0
fi
if [ -z "${BINLOG_STREAMER_TEST_ADMIN_MYSQL:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_ADMIN_MYSQL is not set"; exit 0
fi
case "$("$BINARY" --version 2>&1)" in
    *"(developer mode)"*) ;;
    *) echo "skipped: not a developer-mode build"; exit 0 ;;
esac
ADMIN=( $BINLOG_STREAMER_TEST_ADMIN_MYSQL )
WORKDIR="$(mktemp -d)"
RELAY_PID=""
cleanup() {
    if [ -n "$RELAY_PID" ] && kill -0 "$RELAY_PID" 2>/dev/null; then
        kill -KILL "$RELAY_PID" 2>/dev/null || true
        wait "$RELAY_PID" 2>/dev/null || true
    fi
    rm -rf "$WORKDIR"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
fail() {
    echo "FATAL: [expiry] $*"
    cat "$WORKDIR"/*.log 2>/dev/null || true
    exit 1
}
chmod 700 "$WORKDIR"
DATA_DIR="$WORKDIR/data"
mkdir "$DATA_DIR"
cp "$BINLOG_STREAMER_TEST_SOURCE_YML" "$WORKDIR/source.yml"
chmod 640 "$WORKDIR/source.yml"
cat > "$WORKDIR/settings.yml" <<SETTINGS
server:
  server_id: 999009
storage:
  data_dir: $DATA_DIR
  retention:
    policy: age
    period: 1s
  # Keep the test independent of the host file-system free space.
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
"${ADMIN[@]}" -e "CREATE DATABASE IF NOT EXISTS binlog_streamer_test;
CREATE TABLE IF NOT EXISTS binlog_streamer_test.storage_integration_test (id INT PRIMARY KEY AUTO_INCREMENT, v LONGBLOB) ENGINE=InnoDB;"
BINLOG_BASENAME="$("${ADMIN[@]}" -N -e "SHOW VARIABLES LIKE 'log_bin_basename'" | awk '{print $2}')"
[ -n "$BINLOG_BASENAME" ] || fail 'missing log_bin_basename'
wait_line() {
    local pattern="$1" log="$2"
    for _ in $(seq 1 300); do
        if grep -q "$pattern" "$log"; then return 0; fi
        kill -0 "$RELAY_PID" 2>/dev/null || break
        sleep 0.1
    done
    fail "did not see '$pattern' within 30s"
}
start() {
    "$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$1" &
    RELAY_PID=$!
    wait_line 'dump requested' "$1"
}
stop_relay() {
    kill -TERM "$RELAY_PID" || fail 'relay exited before SIGTERM'
    local exited=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$RELAY_PID" 2>/dev/null; then exited=1; break; fi
        sleep 0.1
    done
    [ "$exited" = 1 ] || fail 'relay did not stop within 10s'
    local rc=0
    wait "$RELAY_PID" || rc=$?
    RELAY_PID=""
    [ "$rc" = 0 ] || fail "relay exit code $rc"
}
check_names() {
    local expected="$1" actual
    actual="$(cd "$DATA_DIR" && ls | grep -E '^[^./]+\.[0-9]{6}$' | sort)"
    [ "$actual" = "$expected" ] || fail "files differ: expected [$expected], got [$actual]"
    [ "$(cat "$DATA_DIR/binlog.index")" = "$expected" ] || fail 'index differs from expected files'
}
check_purged() {
    local log="$1" expected="$2" actual
    actual="$(sed -n 's/^binlog-streamer: purged /purged /p' "$log")"
    [ "$actual" = "$expected" ] || fail "purged lines differ: expected [$expected], got [$actual]"
    if grep -q 'warning: could not remove' "$log"; then fail 'unlink warning'; fi
}
check_prefix() {
    local name="$1" size
    size="$(wc -c < "$DATA_DIR/$name")"
    head -c "$size" "${BINLOG_BASENAME}.${name##*.}" | cmp - "$DATA_DIR/$name" || fail "prefix differs for $name"
}
current_file() {
    local status
    if ! status="$("${ADMIN[@]}" -N -e 'SHOW BINARY LOG STATUS' 2>/dev/null)"; then
        status="$("${ADMIN[@]}" -N -e 'SHOW MASTER STATUS')" || fail 'cannot read current source file'
    fi
    printf '%s\n' "$status" | awk 'NR == 1 {print $1}'
}
FIRST_LOG="$WORKDIR/first.log"
SECOND_LOG="$WORKDIR/second.log"
start "$FIRST_LOG"
F1="$(grep -m1 'dump requested' "$FIRST_LOG" | sed -n 's/.*(file \([^)]*\)).*/\1/p')"
[ -n "$F1" ] || fail 'missing starting file'
sleep 1
CURRENT="$(current_file)"
[ "$F1" = "$CURRENT" ] || fail "relay started from $F1, source is at $CURRENT; the scenario needs the current file"
MARKER="expiry-$$-$(date +%s)"
for n in 1 2 3; do
    "${ADMIN[@]}" -e 'FLUSH BINARY LOGS;'
    CURRENT="$(current_file)"
    [ -n "$CURRENT" ] || fail 'source returned an empty current file'
    case "$n" in
        1) F2="$CURRENT" ;;
        2) F3="$CURRENT" ;;
        3) F4="$CURRENT" ;;
    esac
    "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_integration_test (v) VALUES ('$MARKER-$n');"
    sleep 3
done
CAUGHT_UP=0
for _ in $(seq 1 300); do
    if grep -qas "$MARKER-3" "$DATA_DIR/$F4" 2>/dev/null; then CAUGHT_UP=1; break; fi
    kill -0 "$RELAY_PID" 2>/dev/null || break
    sleep 0.2
done
[ "$CAUGHT_UP" = 1 ] || fail 'third marker did not reach the fourth file within 60s'
stop_relay
check_purged "$FIRST_LOG" "$(printf 'purged %s\npurged %s' "$F1" "$F2")"
check_names "$(printf '%s\n%s' "$F3" "$F4")"
cmp "${BINLOG_BASENAME}.${F3##*.}" "$DATA_DIR/$F3" || fail 'closed file differs from source'
check_prefix "$F4"
sleep 3
start "$SECOND_LOG"
wait_line "purged $F3" "$SECOND_LOG"
stop_relay
check_purged "$SECOND_LOG" "purged $F3"
check_names "$F4"
check_prefix "$F4"
echo "OK: [expiry] rotations purged $F1 and $F2; startup purged $F3; $F4 remains a source prefix"
