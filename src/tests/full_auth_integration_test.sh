#!/usr/bin/env bash
# Usage: full_auth_integration_test.sh <path-to-binary>
# Skipped like source_integration_test.sh, and when
# BINLOG_STREAMER_TEST_ADMIN_MYSQL is unset.
set -u

BINARY="${1:?usage: full_auth_integration_test.sh <path-to-binary>}"

if [ -z "${BINLOG_STREAMER_TEST_SOURCE_YML:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_SOURCE_YML is not set"
    exit 0
fi
if [ -z "${BINLOG_STREAMER_TEST_ADMIN_MYSQL:-}" ]; then
    echo "skipped: BINLOG_STREAMER_TEST_ADMIN_MYSQL is not set"
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

# Unquoted deliberately: BINLOG_STREAMER_TEST_ADMIN_MYSQL is a command line that
# may carry flags.
# shellcheck disable=SC2206
ADMIN=( $BINLOG_STREAMER_TEST_ADMIN_MYSQL )

wait_for_dump_then_stop() {
    local STDERR_LOG="$1"
    local PID="$2"
    DUMP_STARTED=0
    for _ in $(seq 1 300); do
        if grep -q "dump requested" "$STDERR_LOG" 2>/dev/null; then
            DUMP_STARTED=1
            break
        fi
        if ! kill -0 "$PID" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if [ "$DUMP_STARTED" -ne 1 ]; then
        return 1
    fi
    kill -TERM "$PID"
    EXITED=0
    for _ in $(seq 1 100); do
        if ! kill -0 "$PID" 2>/dev/null; then
            EXITED=1
            break
        fi
        sleep 0.1
    done
    if [ "$EXITED" -ne 1 ]; then
        return 1
    fi
    wait "$PID"
    EXIT_CODE=$?
    return 0
}

write_settings_yml() {
    local DIR="$1"
    local SERVER_ID="$2"
    local DATA_DIR="$DIR/data"
    mkdir -p "$DATA_DIR"
    cat > "$DIR/settings.yml" <<SETTINGS
server:
  server_id: $SERVER_ID
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
    chmod 640 "$DIR/settings.yml"
}

# The key is a SHOW STATUS value, not a secret: safe to read via the admin
# client.
fetch_source_public_key() {
    "${ADMIN[@]}" -N --raw -e "SHOW STATUS LIKE 'Caching_sha2_password_rsa_public_key'" |
        sed '1s/^[^\t]*\t//' > "$1"
}

# On failure, RELAY_PID is left set on purpose so cleanup_current can still
# reach it.
run_relay_to_dump_and_stop() {
    local DIR="$1"
    STDERR_LOG="$(mktemp)"
    "$BINARY" --config "$DIR/settings.yml" >/dev/null 2>"$STDERR_LOG" &
    RELAY_PID=$!
    if ! wait_for_dump_then_stop "$STDERR_LOG" "$RELAY_PID"; then
        return 1
    fi
    RELAY_PID=""
    return 0
}

cleanup_current() {
    if [ -n "${RELAY_PID:-}" ] && kill -0 "$RELAY_PID" 2>/dev/null; then
        kill -KILL "$RELAY_PID" 2>/dev/null
        wait "$RELAY_PID" 2>/dev/null
    fi
    RELAY_PID=""
    [ -n "${WORKDIR:-}" ] && rm -rf "$WORKDIR"
    WORKDIR=""
    [ -n "${STDERR_LOG:-}" ] && rm -f "$STDERR_LOG"
    STDERR_LOG=""
}

# Reconnects through the relay binary, not the admin client, so no password ends
# up on the command line.
restore_source_auth_cache() {
    WORKDIR="$(mktemp -d)"
    chmod 700 "$WORKDIR"
    local KEY_FILE="$WORKDIR/source-public-key.pem"
    if ! fetch_source_public_key "$KEY_FILE"; then
        echo "FATAL: [restore] could not read the source's RSA public key" >&2
        cleanup_current
        return 1
    fi
    if [ ! -s "$KEY_FILE" ]; then
        echo "FATAL: [restore] Caching_sha2_password_rsa_public_key is empty - is the source using caching_sha2_password?" >&2
        cleanup_current
        return 1
    fi
    chmod 644 "$KEY_FILE"

    cp "$SOURCE_YML" "$WORKDIR/source.yml"
    printf 'source_public_key_path: %s\n' "$KEY_FILE" >> "$WORKDIR/source.yml"
    chmod 640 "$WORKDIR/source.yml"
    write_settings_yml "$WORKDIR" 999009

    if ! run_relay_to_dump_and_stop "$WORKDIR"; then
        echo "FATAL: [restore] relay did not reach dump and exit cleanly; stderr:" >&2
        cat "$STDERR_LOG" >&2
        cleanup_current
        return 1
    fi
    local RC=$EXIT_CODE
    cleanup_current
    if [ "$RC" -ne 0 ]; then
        echo "FATAL: [restore] expected exit code 0 after SIGTERM, got $RC" >&2
        return 1
    fi
    return 0
}

# Single EXIT trap, shared by every scenario (a second one would silently
# replace it): always repopulates the auth cache, since scenario C empties it
# deliberately.
final_exit() {
    local RC=$?
    cleanup_current
    if ! restore_source_auth_cache; then
        echo "FATAL: could not restore the source's caching_sha2_password hash cache on exit; the next test run against $SOURCE_YML will fail" >&2
        RC=1
    fi
    exit "$RC"
}
trap final_exit EXIT

OVERALL_FAIL=0

# --- Scenario A: source_public_key_path set to the source's own key ---
WORKDIR="$(mktemp -d)"
chmod 700 "$WORKDIR"

if ! "${ADMIN[@]}" -e "FLUSH PRIVILEGES;"; then
    echo "FATAL: [A] FLUSH PRIVILEGES failed"
    exit 1
fi

KEY_FILE="$WORKDIR/source-public-key.pem"
if ! fetch_source_public_key "$KEY_FILE"; then
    echo "FATAL: [A] could not read the source's RSA public key"
    exit 1
fi
if [ ! -s "$KEY_FILE" ]; then
    echo "FATAL: [A] Caching_sha2_password_rsa_public_key is empty - is the source using caching_sha2_password?"
    exit 1
fi
chmod 644 "$KEY_FILE"

cp "$SOURCE_YML" "$WORKDIR/source.yml"
printf 'source_public_key_path: %s\n' "$KEY_FILE" >> "$WORKDIR/source.yml"
chmod 640 "$WORKDIR/source.yml"
write_settings_yml "$WORKDIR" 999006

if ! run_relay_to_dump_and_stop "$WORKDIR"; then
    echo "FATAL: [A] relay did not reach dump and exit cleanly; stderr:"
    cat "$STDERR_LOG"
    OVERALL_FAIL=1
elif [ "$EXIT_CODE" -ne 0 ]; then
    echo "FATAL: [A] expected exit code 0 after SIGTERM, got $EXIT_CODE; stderr:"
    cat "$STDERR_LOG"
    OVERALL_FAIL=1
else
    echo "OK: [A] source_public_key_path reached dump, exit 0"
fi
cleanup_current

# --- Scenario B: get_source_public_key: true, no local key file ---
WORKDIR="$(mktemp -d)"
chmod 700 "$WORKDIR"

if ! "${ADMIN[@]}" -e "FLUSH PRIVILEGES;"; then
    echo "FATAL: [B] FLUSH PRIVILEGES failed"
    exit 1
fi

cp "$SOURCE_YML" "$WORKDIR/source.yml"
printf 'get_source_public_key: true\n' >> "$WORKDIR/source.yml"
chmod 640 "$WORKDIR/source.yml"
write_settings_yml "$WORKDIR" 999007

if ! run_relay_to_dump_and_stop "$WORKDIR"; then
    echo "FATAL: [B] relay did not reach dump and exit cleanly; stderr:"
    cat "$STDERR_LOG"
    OVERALL_FAIL=1
elif [ "$EXIT_CODE" -ne 0 ]; then
    echo "FATAL: [B] expected exit code 0 after SIGTERM, got $EXIT_CODE; stderr:"
    cat "$STDERR_LOG"
    OVERALL_FAIL=1
else
    echo "OK: [B] get_source_public_key reached dump, exit 0"
fi
cleanup_current

# --- Scenario C: neither setting - full authentication must be refused ---
WORKDIR="$(mktemp -d)"
chmod 700 "$WORKDIR"

if ! "${ADMIN[@]}" -e "FLUSH PRIVILEGES;"; then
    echo "FATAL: [C] FLUSH PRIVILEGES failed"
    exit 1
fi

cp "$SOURCE_YML" "$WORKDIR/source.yml"
chmod 640 "$WORKDIR/source.yml"
write_settings_yml "$WORKDIR" 999008

STDERR_LOG="$(mktemp)"
"$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$STDERR_LOG" &
RELAY_PID=$!

# SIGTERM as soon as the refusal line shows up; don't wait for the retry loop.
REFUSED=0
for _ in $(seq 1 300); do
    if grep -q "Authentication requires secure connection." "$STDERR_LOG" 2>/dev/null; then
        REFUSED=1
        break
    fi
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

if [ "$REFUSED" -ne 1 ]; then
    echo "FATAL: [C] 'Authentication requires secure connection.' did not appear in stderr within 30s; stderr:"
    cat "$STDERR_LOG"
    OVERALL_FAIL=1
else
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
        echo "FATAL: [C] relay did not exit within 10s of SIGTERM; stderr so far:"
        cat "$STDERR_LOG"
        OVERALL_FAIL=1
    else
        wait "$RELAY_PID"
        EXIT_CODE=$?
        if [ "$EXIT_CODE" -ne 0 ]; then
            echo "FATAL: [C] expected exit code 0 after SIGTERM, got $EXIT_CODE; stderr:"
            cat "$STDERR_LOG"
            OVERALL_FAIL=1
        else
            echo "OK: [C] refused without a key source, exit 0 after SIGTERM"
        fi
    fi
fi
cleanup_current

# --- Scenario D: plain source.yml, no FLUSH PRIVILEGES - proves the cache [C] emptied works again ---
if ! restore_source_auth_cache; then
    echo "FATAL: [D] could not repopulate the source's password-hash cache before this scenario"
    OVERALL_FAIL=1
else
    WORKDIR="$(mktemp -d)"
    chmod 700 "$WORKDIR"
    cp "$SOURCE_YML" "$WORKDIR/source.yml"
    chmod 640 "$WORKDIR/source.yml"
    write_settings_yml "$WORKDIR" 999010

    if ! run_relay_to_dump_and_stop "$WORKDIR"; then
        echo "FATAL: [D] relay did not reach dump and exit cleanly; stderr:"
        cat "$STDERR_LOG"
        OVERALL_FAIL=1
    elif [ "$EXIT_CODE" -ne 0 ]; then
        echo "FATAL: [D] expected exit code 0 after SIGTERM, got $EXIT_CODE; stderr:"
        cat "$STDERR_LOG"
        OVERALL_FAIL=1
    else
        echo "OK: [D] plain source.yml reached dump via the cached hash, exit 0"
    fi
    cleanup_current
fi

if [ "$OVERALL_FAIL" -ne 0 ]; then
    exit 1
fi
echo "OK: all four full-authentication scenarios passed"
