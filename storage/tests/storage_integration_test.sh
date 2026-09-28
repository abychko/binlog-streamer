#!/usr/bin/env bash
set -u

BINARY="${1:?usage: storage_integration_test.sh <path-to-binary>}"

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

# BINLOG_STREAMER_TEST_ADMIN_MYSQL is a full client command line (e.g.
# "mysql -uroot"), expanded unquoted deliberately - it may itself carry flags.
# shellcheck disable=SC2206
ADMIN=( $BINLOG_STREAMER_TEST_ADMIN_MYSQL )

SCHEMA_SQL="CREATE DATABASE IF NOT EXISTS binlog_streamer_test;
DROP TABLE IF EXISTS binlog_streamer_test.storage_integration_test;
CREATE TABLE binlog_streamer_test.storage_integration_test (
    id INT PRIMARY KEY AUTO_INCREMENT,
    v LONGBLOB
) ENGINE=InnoDB;"
if ! "${ADMIN[@]}" -e "$SCHEMA_SQL"; then
    echo "FATAL: could not prepare the binlog_streamer_test schema"
    exit 1
fi

BINLOG_BASENAME="$("${ADMIN[@]}" -N -e "SHOW VARIABLES LIKE 'log_bin_basename'" | awk '{print $2}')"
if [ -z "$BINLOG_BASENAME" ]; then
    echo "FATAL: could not read log_bin_basename from the source"
    exit 1
fi

if ! command -v mysqlbinlog >/dev/null 2>&1; then
    echo "FATAL: mysqlbinlog not found in PATH"
    exit 1
fi

# Runs one full scenario in a subshell: its own settings.yml/data_dir/relay
# process/cleanup trap, isolated so a mid-scenario `exit` only ends the
# subshell. $1 label, $2 storage.retention.period, $3 server_id (distinct per test).
run_scenario() {
    local LABEL="$1"
    local PERIOD="$2"
    local SERVER_ID="$3"
    (
        set -u
        # Unique per run: the source's current file can still hold the
        # marker of an earlier run - a fixed marker would be found there
        # first and stop the relay early.
        MARKER="after-second-rotation-${LABEL}-$$-$(date +%s)"

        STDERR_LOG="$(mktemp)"
        cleanup() {
            if [ -n "${RELAY_PID:-}" ] && kill -0 "$RELAY_PID" 2>/dev/null; then
                kill -KILL "$RELAY_PID" 2>/dev/null
            fi
            rm -rf "${WORKDIR:-}" "$STDERR_LOG"
        }
        trap cleanup EXIT

        WORKDIR="$(mktemp -d)"
        chmod 700 "$WORKDIR"
        DATA_DIR="$WORKDIR/data"
        mkdir -p "$DATA_DIR"

        # Explicit, narrower mode than a plain cp/heredoc leaves them at:
        # DiskProtectedFileReader rejects group/other-writable files.
        cp "$SOURCE_YML" "$WORKDIR/source.yml"
        chmod 640 "$WORKDIR/source.yml"

        # disk.* thresholds copied from packaging/settings.yml as-is:
        # purge/watermark enforcement is not part of this stage, only
        # their presence is validated (config/cConfigValidator.cpp).
        cat > "$WORKDIR/settings.yml" <<SETTINGS
server:
  server_id: $SERVER_ID
storage:
  data_dir: $DATA_DIR
  retention:
    policy: age
    period: $PERIOD
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

        "$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$STDERR_LOG" &
        RELAY_PID=$!

        # Wait for the dump to actually start (up to 30s) rather than a
        # fixed sleep - same reasoning as src/tests/source_integration_test.sh.
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
            echo "FATAL: [$LABEL] 'dump requested' did not appear in stderr within 30s; stderr:"
            cat "$STDERR_LOG"
            exit 1
        fi

        sleep 1 # let registration/the first file's header settle before issuing writes

        # Five transactions, one with an event over 16 MiB - REPEAT('a', 25165824)
        # is the same oversize value already proven to cross MAX_PAYLOAD_PER_PACKET
        # in receiver/tests/driver/stream_integration_test.sh.
        INSERT_SQL="INSERT INTO storage_integration_test (v) VALUES ('row1-${LABEL}');
INSERT INTO storage_integration_test (v) VALUES ('row2-${LABEL}');
INSERT INTO storage_integration_test (v) VALUES ('row3-${LABEL}');
INSERT INTO storage_integration_test (v) VALUES (REPEAT('a', 25165824));
INSERT INTO storage_integration_test (v) VALUES ('row5-${LABEL}');"
        if ! "${ADMIN[@]}" binlog_streamer_test -e "$INSERT_SQL"; then
            echo "FATAL: [$LABEL] the five test transactions did not all succeed"
            exit 1
        fi

        # Two forced rotations, each followed by a further transaction so the
        # file just rotated into is not itself empty when the run ends.
        if ! "${ADMIN[@]}" -e "FLUSH BINARY LOGS;"; then
            echo "FATAL: [$LABEL] first FLUSH BINARY LOGS failed"
            exit 1
        fi
        if ! "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_integration_test (v) VALUES ('after-first-rotation-${LABEL}');"; then
            echo "FATAL: [$LABEL] the post-first-rotation transaction failed"
            exit 1
        fi
        if ! "${ADMIN[@]}" -e "FLUSH BINARY LOGS;"; then
            echo "FATAL: [$LABEL] second FLUSH BINARY LOGS failed"
            exit 1
        fi
        if ! "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_integration_test (v) VALUES ('$MARKER');"; then
            echo "FATAL: [$LABEL] the post-second-rotation transaction failed"
            exit 1
        fi

        # Waits for the relay to reach this run's own last transaction rather
        # than a fixed sleep: a long-lived source's history only grows, so a
        # once-sufficient fixed sleep can silently stop being enough.
        CAUGHT_UP=0
        for _ in $(seq 1 300); do
            if grep -qs "$MARKER" "$DATA_DIR"/*.[0-9][0-9][0-9][0-9][0-9][0-9] 2>/dev/null; then
                CAUGHT_UP=1
                break
            fi
            if ! kill -0 "$RELAY_PID" 2>/dev/null; then
                break # exited before catching up - reported below, not here
            fi
            sleep 0.2
        done
        if [ "$CAUGHT_UP" -ne 1 ]; then
            echo "FATAL: [$LABEL] relay did not catch up to this run's own last transaction within 60s; stderr so far:"
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
            echo "FATAL: [$LABEL] relay did not exit within 10s of SIGTERM; stderr so far:"
            cat "$STDERR_LOG"
            exit 1
        fi

        wait "$RELAY_PID"
        EXIT_CODE=$?
        RELAY_PID=""
        if [ "$PERIOD" = "10s" ] && grep -q ": purged " "$STDERR_LOG"; then
            echo "FATAL: [$LABEL] a file was purged during this scenario; its checks assume none is"
            cat "$STDERR_LOG"; exit 1
        fi
        STDERR_TEXT="$(cat "$STDERR_LOG")"
        echo "$STDERR_TEXT"

        if [ "$EXIT_CODE" -ne 0 ]; then
            echo "FATAL: [$LABEL] expected exit code 0 after SIGTERM, got $EXIT_CODE"
            exit 1
        fi

        # Witnesses: transactions flowed, one event crossed the
        # packet-splitting threshold, more than one file exists - else
        # comparisons below could pass vacuously.
        LARGEST_EVENT_LENGTH="$(printf '%s\n' "$STDERR_TEXT" | grep -oE 'largest_event_length=[0-9]+' | cut -d= -f2)"
        if [ "${LARGEST_EVENT_LENGTH:-0}" -le 16777215 ]; then
            echo "FATAL: [$LABEL] largest_event_length=$LARGEST_EVENT_LENGTH, expected > 16777215"
            exit 1
        fi

        # Listed from inside DATA_DIR: its temp-dir path can itself contain a
        # dot (macOS mktemp -d), which would throw off a numeric sort on full
        # paths. while read, not mapfile: this project targets bash 3.2.
        SORTED_NAMES=()
        while IFS= read -r NAME; do SORTED_NAMES+=("$NAME"); done < <(cd "$DATA_DIR" && ls | grep -E '^[^./]+\.[0-9]{6,}$' | sort -t. -k2 -n)
        FILE_COUNT=${#SORTED_NAMES[@]}
        if [ "$FILE_COUNT" -lt 3 ]; then
            echo "FATAL: [$LABEL] found $FILE_COUNT file(s) in $DATA_DIR, expected at least 3 (two forced rotations)"
            exit 1
        fi
        FILES=()
        for NAME in "${SORTED_NAMES[@]}"; do FILES+=("$DATA_DIR/$NAME"); done

        # binlog.index must list exactly these names, in the same order
        # cStorageEventSink appended them.
        INDEX_CONTENT="$(cat "$DATA_DIR/binlog.index" 2>/dev/null)"
        EXPECTED_INDEX_CONTENT="$(printf '%s\n' "${SORTED_NAMES[@]}")"
        if [ "$INDEX_CONTENT" != "$EXPECTED_INDEX_CONTENT" ]; then
            echo "FATAL: [$LABEL] $DATA_DIR/binlog.index does not match 'ls | sort' of $DATA_DIR:"
            echo "--- binlog.index ---"
            echo "$INDEX_CONTENT"
            echo "--- ls | sort ---"
            echo "$EXPECTED_INDEX_CONTENT"
            exit 1
        fi

        GTID_GROUPS="$(mysqlbinlog "${FILES[@]}" 2>/dev/null | grep -c '^SET @@SESSION.GTID_NEXT=')"
        if [ "${GTID_GROUPS:-0}" -lt 6 ]; then
            echo "FATAL: [$LABEL] mysqlbinlog counted $GTID_GROUPS GTID transaction(s) across our files, expected at least 6"
            exit 1
        fi

        # Specific to this run: this source accumulates history across runs,
        # so the thresholds above alone could pass without new events. grep -a
        # on raw bytes, since a row event's values are base64 in mysqlbinlog's text output.
        if ! grep -qas "$MARKER" "${FILES[@]}"; then
            echo "FATAL: [$LABEL] this run's own last transaction ('$MARKER') was not found in any of our files - new events never reached storage"
            exit 1
        fi

        # The highest-numbered file is the one not yet closed by a real
        # ROTATE ("in use"); every other file must be byte-identical to the
        # source's copy in full, the open one only up to what was written.
        LAST_INDEX=$((${#FILES[@]} - 1))

        FAIL=0
        for INDEX in "${!FILES[@]}"; do
            OUR_FILE="${FILES[$INDEX]}"
            NAME="$(basename "$OUR_FILE")"
            SOURCE_FILE="${BINLOG_BASENAME}.${NAME##*.}"
            if [ ! -r "$SOURCE_FILE" ]; then
                echo "FATAL: [$LABEL] cannot read $SOURCE_FILE"
                FAIL=1
                continue
            fi

            if ! mysqlbinlog --verify-binlog-checksum "$OUR_FILE" >/dev/null; then
                echo "FATAL: [$LABEL] mysqlbinlog --verify-binlog-checksum rejected $OUR_FILE"
                FAIL=1
            fi

            OUR_SIZE=$(wc -c < "$OUR_FILE")
            if [ "$INDEX" -eq "$LAST_INDEX" ]; then
                if ! head -c "$OUR_SIZE" "$SOURCE_FILE" | cmp - "$OUR_FILE"; then
                    echo "FATAL: [$LABEL] $OUR_FILE (current, $OUR_SIZE bytes) does not match the first $OUR_SIZE bytes of $SOURCE_FILE"
                    FAIL=1
                fi
            else
                if ! cmp "$SOURCE_FILE" "$OUR_FILE"; then
                    echo "FATAL: [$LABEL] closed file $OUR_FILE does not match $SOURCE_FILE byte for byte"
                    FAIL=1
                fi
            fi
        done

        if [ "$FAIL" -ne 0 ]; then exit 1; fi

        echo "OK: [$LABEL] $FILE_COUNT files compared against $BINLOG_BASENAME.*, $GTID_GROUPS GTID transactions, largest_event_length=$LARGEST_EVENT_LENGTH"
    )
}

# Ten-second window leaves room for writes and shutdown without expiry
# (the retention period only governs purging, never where the relay starts).
if ! run_scenario current 10s 999005; then
    exit 1
fi
echo "OK: the fresh-relay scenario passed"
