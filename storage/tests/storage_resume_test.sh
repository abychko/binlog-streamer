#!/usr/bin/env bash
set -u

BINARY="${1:?usage: storage_resume_test.sh <path-to-binary>}"

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

# shellcheck disable=SC2206
ADMIN=( $BINLOG_STREAMER_TEST_ADMIN_MYSQL )

SCHEMA_SQL="CREATE DATABASE IF NOT EXISTS binlog_streamer_test;
DROP TABLE IF EXISTS binlog_streamer_test.storage_resume_test;
CREATE TABLE binlog_streamer_test.storage_resume_test (
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

wait_for_line() {
    local LOG="$1" PATTERN="$2" PID="$3"
    for _ in $(seq 1 300); do
        if grep -q "$PATTERN" "$LOG" 2>/dev/null; then return 0; fi
        if [ -n "$PID" ] && ! kill -0 "$PID" 2>/dev/null; then return 1; fi
        sleep 0.1
    done
    return 1
}

# Proves only the marker bytes are on disk, not the complete group.
wait_for_marker() {
    local DATA_DIR="$1" MARKER="$2" PID="$3"
    for _ in $(seq 1 300); do
        if grep -qs "$MARKER" "$DATA_DIR"/*.[0-9][0-9][0-9][0-9][0-9][0-9] 2>/dev/null; then return 0; fi
        if [ -n "$PID" ] && ! kill -0 "$PID" 2>/dev/null; then return 1; fi
        sleep 0.2
    done
    return 1
}

# Assumes the source is quiet except for this scenario's writes, so its
# exact file length (including the marker group's trailing Xid) is stable.
wait_for_mirrored_length() {
    local FILE="$1" SOURCE_FILE_NAME="$2" PID="$3" SOURCE_SIZE
    for _ in $(seq 1 300); do
        if ! kill -0 "$PID" 2>/dev/null; then return 1; fi
        SOURCE_SIZE="$("${ADMIN[@]}" -N -e "SHOW BINARY LOGS" | awk -v name="$SOURCE_FILE_NAME" '$1 == name {print $2}')"
        case "$SOURCE_SIZE" in ''|*[!0-9]*) return 1 ;; esac
        if [ -f "$FILE" ] && [ "$(wc -c < "$FILE")" -eq "$SOURCE_SIZE" ]; then return 0; fi
        sleep 0.2
    done
    return 1
}

header_end() {
    local SOURCE_FILE_NAME="$1" END_LOG_POS
    END_LOG_POS="$("${ADMIN[@]}" -N -e "SHOW BINLOG EVENTS IN '$SOURCE_FILE_NAME' LIMIT 1,1" | awk '{print $5}')"
    case "$END_LOG_POS" in
        ''|*[!0-9]*) echo "FATAL: could not read End_log_pos for $SOURCE_FILE_NAME" >&2; return 1 ;;
    esac
    echo "$END_LOG_POS"
}

# A fresh file counts its header in appended, but Create writes that header
# outside BytesWritten. A resumed file counts only newly appended bytes.
check_storage_summary() {
    local LOG="$1" HEADER_END="$2" EXPECTED_SIZE="${3:-0}"
    local CACHE_LINES WRITER_LINES APPENDED BYTES
    CACHE_LINES="$(grep '^cache: appended=' "$LOG")"
    WRITER_LINES="$(grep '^writer: bytes=' "$LOG")"
    APPENDED="$(printf '%s\n' "$CACHE_LINES" | sed -n 's/^cache: appended=\([0-9][0-9]*\) .*/\1/p')"
    BYTES="$(printf '%s\n' "$WRITER_LINES" | sed -n 's/^writer: bytes=\([0-9][0-9]*\) .*/\1/p')"
    if [ "$(grep -c '^cache: appended=' "$LOG")" -eq 1 ] &&
       [ "$(grep -c '^writer: bytes=' "$LOG")" -eq 1 ] &&
       [ -n "$APPENDED" ] && [ -n "$BYTES" ] && [ "$APPENDED" -gt 0 ] &&
       { [ "$EXPECTED_SIZE" -eq 0 ] || [ "$APPENDED" -eq "$EXPECTED_SIZE" ]; } &&
       [ "$BYTES" -eq $((APPENDED - HEADER_END)) ]; then
        echo "OK: storage summary appended=$APPENDED bytes=$BYTES HEADER_END=$HEADER_END expected_size=$EXPECTED_SIZE"
        return 0
    fi
    local EXPECTED_BYTES="appended-$HEADER_END"
    if [ "$EXPECTED_SIZE" -gt 0 ]; then EXPECTED_BYTES=$((EXPECTED_SIZE - HEADER_END)); fi
    echo "FATAL: expected one cache/writer summary each, appended > 0, appended=$EXPECTED_SIZE (0 means any positive), bytes=$EXPECTED_BYTES; got:"
    printf '%s\n%s\n' "$CACHE_LINES" "$WRITER_LINES"
    return 1
}

# Appends the first 100 bytes of FILE's first group back onto its end - the
# shape a crash mid-write of a later group leaves. $1 FILE $2 SOURCE_FILE_NAME.
# Echoes bytes appended (100) on success, else FATAL and returns 1.
inject_corruption() {
    local FILE="$1" SOURCE_FILE_NAME="$2"
    local END_LOG_POS
    END_LOG_POS="$(header_end "$SOURCE_FILE_NAME")" || return 1
    local TMP
    TMP="$(mktemp)"
    # Read into a temp file first, then append separately - reading FILE
    # with dd and writing its own output back onto FILE in the same command
    # is not something this script relies on being well-defined.
    dd if="$FILE" bs=1 skip="$END_LOG_POS" count=100 2>/dev/null > "$TMP"
    if [ "$(wc -c < "$TMP")" -ne 100 ]; then
        echo "FATAL: read fewer than 100 bytes from $FILE at offset $END_LOG_POS" >&2
        rm -f "$TMP"
        return 1
    fi
    cat "$TMP" >> "$FILE"
    rm -f "$TMP"
    echo 100
}

assert_no_purge() {
    if grep -q ": purged " "$2"; then
        echo "FATAL: [$1] a file was purged during this scenario; its checks assume none is"
        cat "$2"
        return 1
    fi
}

# Runs one full scenario in a subshell (own WORKDIR, two relay processes,
# cleanup trap). $1 label $2 server_id (distinct per scenario) $3 stop
# signal for the first run (KILL/TERM) $4 1 to inject_corruption(), 0 to skip.
run_scenario() {
    local LABEL="$1"
    local SERVER_ID="$2"
    local STOP_SIGNAL="$3"
    local INJECT_CORRUPTION="$4"
    (
        set -u
        PRE_MARKER="pre-stop-${LABEL}"
        MID_MARKER="while-stopped-${LABEL}"
        POST_MARKER="post-restart-${LABEL}"

        FIRST_LOG="$(mktemp)"
        SECOND_LOG="$(mktemp)"
        cleanup() {
            for VAR in FIRST_PID SECOND_PID; do
                PID="${!VAR:-}"
                if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then kill -KILL "$PID" 2>/dev/null; fi
            done
            rm -rf "${WORKDIR:-}" "$FIRST_LOG" "$SECOND_LOG"
        }
        trap cleanup EXIT

        WORKDIR="$(mktemp -d)"
        chmod 700 "$WORKDIR"
        DATA_DIR="$WORKDIR/data"
        mkdir -p "$DATA_DIR"

        cp "$SOURCE_YML" "$WORKDIR/source.yml"
        chmod 640 "$WORKDIR/source.yml"

        cat > "$WORKDIR/settings.yml" <<SETTINGS
server:
  server_id: $SERVER_ID
storage:
  data_dir: $DATA_DIR
  retention:
    policy: age
    period: 10s
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

        : > "$FIRST_LOG"
        "$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$FIRST_LOG" &
        FIRST_PID=$!

        if ! wait_for_line "$FIRST_LOG" "dump requested" "$FIRST_PID"; then
            echo "FATAL: [$LABEL] 'dump requested' did not appear in stderr within 30s; stderr:"
            cat "$FIRST_LOG"
            exit 1
        fi

        # This scenario never forces a rotation - the file the resolver
        # picked stays this run's only file throughout, named identically on
        # both sides (the relay mirrors the source's own file names).
        START_FILE="$(grep -m1 "dump requested" "$FIRST_LOG" | sed -n 's/.*(file \([^)]*\)).*/\1/p')"
        if [ -z "$START_FILE" ]; then
            echo "FATAL: [$LABEL] could not parse the starting file name out of the 'dump requested' line"
            exit 1
        fi
        START_FILE_NUMBER=$((10#${START_FILE##*.}))
        RELAY_FILE="$DATA_DIR/$START_FILE"

        sleep 1 # let the first file's own header settle before issuing writes

        INSERT_SQL="INSERT INTO storage_resume_test (v) VALUES ('row1-${LABEL}');
INSERT INTO storage_resume_test (v) VALUES ('row2-${LABEL}');
INSERT INTO storage_resume_test (v) VALUES ('$PRE_MARKER');"
        if ! "${ADMIN[@]}" binlog_streamer_test -e "$INSERT_SQL"; then
            echo "FATAL: [$LABEL] the pre-stop transactions did not all succeed"
            exit 1
        fi
        if ! wait_for_marker "$DATA_DIR" "$PRE_MARKER" "$FIRST_PID"; then
            echo "FATAL: [$LABEL] relay did not catch up to the pre-stop marker within 60s; stderr so far:"
            cat "$FIRST_LOG"
            exit 1
        fi

        if ! wait_for_mirrored_length "$RELAY_FILE" "$START_FILE" "$FIRST_PID"; then
            echo "FATAL: [$LABEL] relay did not mirror the source file length within 60s"
            cat "$FIRST_LOG"
            exit 1
        fi

        # BSD wc (macOS) right-pads `wc -c < file` with leading spaces; the
        # grep witness below matches this as literal text, so strip once here.
        SIZE_BEFORE_STOP=$(wc -c < "$RELAY_FILE" | tr -d '[:space:]')

        kill "-$STOP_SIGNAL" "$FIRST_PID"
        EXITED=0
        for _ in $(seq 1 100); do
            if ! kill -0 "$FIRST_PID" 2>/dev/null; then EXITED=1; break; fi
            sleep 0.1
        done
        if [ "$EXITED" -ne 1 ]; then
            echo "FATAL: [$LABEL] first run did not exit within 10s of SIG$STOP_SIGNAL; stderr so far:"
            cat "$FIRST_LOG"
            exit 1
        fi
        wait "$FIRST_PID" 2>/dev/null
        FIRST_EXIT_CODE=$?
        assert_no_purge "${LABEL:-identity}" "$FIRST_LOG" || exit 1
        FIRST_PID=""
        if [ "$STOP_SIGNAL" = "TERM" ] && [ "$FIRST_EXIT_CODE" -ne 0 ]; then
            echo "FATAL: [$LABEL] expected exit code 0 after the first run's own SIGTERM, got $FIRST_EXIT_CODE"
            cat "$FIRST_LOG"
            exit 1
        fi

        # Only clean shutdown prints summaries. The exact source length was
        # reached before stopping; no further transaction is issued here.
        # DrainAndSync must preserve that length and account for every byte.
        if [ "$LABEL" = "clean" ]; then
            SIZE_AFTER_STOP=$(wc -c < "$RELAY_FILE" | tr -d '[:space:]')
            HEADER_END="$(header_end "$START_FILE")" || exit 1
            echo "[$LABEL] SIZE_BEFORE_STOP=$SIZE_BEFORE_STOP SIZE_AFTER_STOP=$SIZE_AFTER_STOP HEADER_END=$HEADER_END"
            if [ "$SIZE_AFTER_STOP" -ne "$SIZE_BEFORE_STOP" ]; then
                echo "FATAL: [$LABEL] expected size=$SIZE_BEFORE_STOP appended=$SIZE_AFTER_STOP bytes=$((SIZE_AFTER_STOP - HEADER_END)); got size=$SIZE_AFTER_STOP and:"
                grep -E '^cache: appended=|^writer: bytes=' "$FIRST_LOG"
                exit 1
            fi
            check_storage_summary "$FIRST_LOG" "$HEADER_END" "$SIZE_AFTER_STOP" || exit 1
            if ! grep -q "last event $START_FILE:$SIZE_BEFORE_STOP\$" "$FIRST_LOG"; then
                echo "FATAL: [$LABEL] expected a 'last event $START_FILE:$SIZE_BEFORE_STOP' line in the first run's own stderr, got:"
                cat "$FIRST_LOG"
                exit 1
            fi
        fi

        CORRUPTION_BYTES=0
        if [ "$INJECT_CORRUPTION" -eq 1 ]; then
            CORRUPTION_BYTES="$(inject_corruption "$RELAY_FILE" "$START_FILE")"
            if [ -z "$CORRUPTION_BYTES" ]; then
                echo "FATAL: [$LABEL] inject_corruption failed"
                exit 1
            fi
            SIZE_AFTER_CORRUPTION=$(wc -c < "$RELAY_FILE")
            if [ "$SIZE_AFTER_CORRUPTION" -ne $((SIZE_BEFORE_STOP + CORRUPTION_BYTES)) ]; then
                echo "FATAL: [$LABEL] $RELAY_FILE is $SIZE_AFTER_CORRUPTION bytes, expected $((SIZE_BEFORE_STOP + CORRUPTION_BYTES)) after injecting corruption"
                exit 1
            fi
        fi

        # Transactions the source receives *while the relay is down* - the
        # restart has to both recover local history and catch these up from
        # the source, not just the ones already on disk before the stop.
        if ! "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_resume_test (v) VALUES ('$MID_MARKER');"; then
            echo "FATAL: [$LABEL] the while-stopped transaction did not succeed"
            exit 1
        fi

        : > "$SECOND_LOG"
        "$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$SECOND_LOG" &
        SECOND_PID=$!

        if ! wait_for_line "$SECOND_LOG" "dump requested from stored history" "$SECOND_PID"; then
            echo "FATAL: [$LABEL] 'dump requested from stored history' did not appear in the restarted run's stderr within 30s:"
            cat "$SECOND_LOG"
            exit 1
        fi

        # Negative witness for the "identity" scenario's warning: this
        # scenario's source never changes server_id, so the warning must stay
        # silent - catches a mutant that warns on every restart, not just a real mismatch.
        if grep -q "server_id has changed" "$SECOND_LOG"; then
            echo "FATAL: [$LABEL] unexpected server_id-changed warning in the restarted run's stderr:"
            cat "$SECOND_LOG"
            exit 1
        fi

        # The exact injected byte count must appear in stderr; a scenario
        # injecting nothing must not report a truncation either.
        if [ "$INJECT_CORRUPTION" -eq 1 ]; then
            if ! wait_for_line "$SECOND_LOG" "truncated $START_FILE by $CORRUPTION_BYTES byte(s)" "$SECOND_PID"; then
                echo "FATAL: [$LABEL] expected a 'truncated $START_FILE by $CORRUPTION_BYTES byte(s)' line in the restarted run's stderr, got:"
                cat "$SECOND_LOG"
                exit 1
            fi
        else
            if grep -q "truncated $START_FILE by" "$SECOND_LOG"; then
                echo "FATAL: [$LABEL] this scenario injected no corruption, but the restarted run reported truncating $START_FILE anyway:"
                cat "$SECOND_LOG"
                exit 1
            fi
        fi

        if ! "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_resume_test (v) VALUES ('$POST_MARKER');"; then
            echo "FATAL: [$LABEL] the post-restart transaction did not succeed"
            exit 1
        fi
        if ! wait_for_marker "$DATA_DIR" "$POST_MARKER" "$SECOND_PID"; then
            echo "FATAL: [$LABEL] restarted relay did not catch up to the post-restart marker within 60s; stderr so far:"
            cat "$SECOND_LOG"
            exit 1
        fi

        kill -TERM "$SECOND_PID"
        EXITED=0
        for _ in $(seq 1 100); do
            if ! kill -0 "$SECOND_PID" 2>/dev/null; then EXITED=1; break; fi
            sleep 0.1
        done
        if [ "$EXITED" -ne 1 ]; then
            echo "FATAL: [$LABEL] restarted relay did not exit within 10s of SIGTERM; stderr so far:"
            cat "$SECOND_LOG"
            exit 1
        fi
        wait "$SECOND_PID"
        SECOND_EXIT_CODE=$?
        assert_no_purge "${LABEL:-identity}" "$SECOND_LOG" || exit 1
        SECOND_PID=""
        SECOND_TEXT="$(cat "$SECOND_LOG")"
        echo "$SECOND_TEXT"
        if [ "$SECOND_EXIT_CODE" -ne 0 ]; then
            echo "FATAL: [$LABEL] expected exit code 0 after the restarted run's own SIGTERM, got $SECOND_EXIT_CODE"
            exit 1
        fi

        check_storage_summary "$SECOND_LOG" 0 || exit 1

        # No gap or history mismatch must ever have been reported in either
        # run - the whole point of this scenario.
        if grep -q "past what storage has written\|storage does not match source history" "$FIRST_LOG" "$SECOND_LOG"; then
            echo "FATAL: [$LABEL] a gap or a history mismatch was detected somewhere across the two runs:"
            cat "$FIRST_LOG" "$SECOND_LOG"
            exit 1
        fi

        # while read, not mapfile - see storage_integration_test.sh's own
        # comment on why (this development source's macOS shell is bash 3.2).
        SORTED_NAMES=()
        while IFS= read -r NAME; do SORTED_NAMES+=("$NAME"); done < <(cd "$DATA_DIR" && ls | grep -E '^[^./]+\.[0-9]{6,}$' | sort -t. -k2 -n)
        FILE_COUNT=${#SORTED_NAMES[@]}
        if [ "$FILE_COUNT" -lt 1 ]; then
            echo "FATAL: [$LABEL] found no files in $DATA_DIR"
            exit 1
        fi
        FILES=()
        for NAME in "${SORTED_NAMES[@]}"; do FILES+=("$DATA_DIR/$NAME"); done

        INDEX_CONTENT="$(cat "$DATA_DIR/binlog.index" 2>/dev/null)"
        EXPECTED_INDEX_CONTENT="$(printf '%s\n' "${SORTED_NAMES[@]}")"
        if [ "$INDEX_CONTENT" != "$EXPECTED_INDEX_CONTENT" ]; then
            echo "FATAL: [$LABEL] $DATA_DIR/binlog.index does not match 'ls | sort' of $DATA_DIR:"
            echo "--- binlog.index ---"; echo "$INDEX_CONTENT"
            echo "--- ls | sort ---"; echo "$EXPECTED_INDEX_CONTENT"
            exit 1
        fi

        # Compared against the source's own count, not a literal "1", so a
        # rotation this scenario did not request still leaves both sides agreeing.
        SOURCE_FILE_COUNT=0
        while IFS= read -r NAME; do
            NUMBER=$((10#${NAME##*.}))
            if [ "$NUMBER" -ge "$START_FILE_NUMBER" ]; then SOURCE_FILE_COUNT=$((SOURCE_FILE_COUNT + 1)); fi
        done < <(cd "$(dirname "$BINLOG_BASENAME")" && ls | grep -E "^$(basename "$BINLOG_BASENAME")\.[0-9]{6,}$")
        if [ "$FILE_COUNT" -ne "$SOURCE_FILE_COUNT" ]; then
            echo "FATAL: [$LABEL] relay has $FILE_COUNT file(s), source has $SOURCE_FILE_COUNT file(s) from $START_FILE onward"
            exit 1
        fi

        if ! grep -qas "$POST_MARKER" "${FILES[@]}"; then
            echo "FATAL: [$LABEL] the post-restart transaction ('$POST_MARKER') was not found in any of our files"
            exit 1
        fi
        if ! grep -qas "$MID_MARKER" "${FILES[@]}"; then
            echo "FATAL: [$LABEL] the while-stopped transaction ('$MID_MARKER') was not found in any of our files"
            exit 1
        fi
        if ! grep -qas "$PRE_MARKER" "${FILES[@]}"; then
            echo "FATAL: [$LABEL] the pre-stop transaction ('$PRE_MARKER') was not found in any of our files - it was lost across the restart"
            exit 1
        fi

        LAST_INDEX=$((${#FILES[@]} - 1))
        FAIL=0
        for INDEX in "${!FILES[@]}"; do
            OUR_FILE="${FILES[$INDEX]}"
            NAME="$(basename "$OUR_FILE")"
            THIS_SOURCE_FILE="${BINLOG_BASENAME}.${NAME##*.}"
            if [ ! -r "$THIS_SOURCE_FILE" ]; then
                echo "FATAL: [$LABEL] cannot read $THIS_SOURCE_FILE"
                FAIL=1
                continue
            fi
            if ! mysqlbinlog --verify-binlog-checksum "$OUR_FILE" >/dev/null; then
                echo "FATAL: [$LABEL] mysqlbinlog --verify-binlog-checksum rejected $OUR_FILE"
                FAIL=1
            fi
            OUR_SIZE=$(wc -c < "$OUR_FILE")
            if [ "$INDEX" -eq "$LAST_INDEX" ]; then
                if ! head -c "$OUR_SIZE" "$THIS_SOURCE_FILE" | cmp - "$OUR_FILE"; then
                    echo "FATAL: [$LABEL] $OUR_FILE (current, $OUR_SIZE bytes) does not match the first $OUR_SIZE bytes of $THIS_SOURCE_FILE"
                    FAIL=1
                fi
            else
                if ! cmp "$THIS_SOURCE_FILE" "$OUR_FILE"; then
                    echo "FATAL: [$LABEL] closed file $OUR_FILE does not match $THIS_SOURCE_FILE byte for byte"
                    FAIL=1
                fi
            fi
        done
        if [ "$FAIL" -ne 0 ]; then exit 1; fi

        echo "OK: [$LABEL] $FILE_COUNT file(s) compared against $BINLOG_BASENAME.*, resumed after SIG$STOP_SIGNAL, no gap"
    )
}

# Scenario: patches the last file's server_id byte to simulate a source
# identity change, then forces FLUSH BINARY LOGS while the relay is down so
# the real file is never resent unpatched. Expects a warn-and-continue, not a refusal.
run_identity_mismatch_scenario() {
    (
        set -u
        SERVER_ID=999008
        MARKER="pre-stop-identity"
        POST_MARKER="post-restart-identity"

        FIRST_LOG="$(mktemp)"
        SECOND_LOG="$(mktemp)"
        cleanup() {
            for VAR in FIRST_PID SECOND_PID; do
                PID="${!VAR:-}"
                if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then kill -KILL "$PID" 2>/dev/null; fi
            done
            rm -rf "${WORKDIR:-}" "$FIRST_LOG" "$SECOND_LOG"
        }
        trap cleanup EXIT

        WORKDIR="$(mktemp -d)"
        chmod 700 "$WORKDIR"
        DATA_DIR="$WORKDIR/data"
        mkdir -p "$DATA_DIR"

        cp "$SOURCE_YML" "$WORKDIR/source.yml"
        chmod 640 "$WORKDIR/source.yml"

        cat > "$WORKDIR/settings.yml" <<SETTINGS
server:
  server_id: $SERVER_ID
storage:
  data_dir: $DATA_DIR
  retention:
    policy: age
    period: 10s
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

        : > "$FIRST_LOG"
        "$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$FIRST_LOG" &
        FIRST_PID=$!

        if ! wait_for_line "$FIRST_LOG" "dump requested" "$FIRST_PID"; then
            echo "FATAL: [identity] 'dump requested' did not appear in stderr within 30s; stderr:"
            cat "$FIRST_LOG"
            exit 1
        fi

        START_FILE="$(grep -m1 "dump requested" "$FIRST_LOG" | sed -n 's/.*(file \([^)]*\)).*/\1/p')"
        if [ -z "$START_FILE" ]; then
            echo "FATAL: [identity] could not parse the starting file name out of the 'dump requested' line"
            exit 1
        fi
        START_FILE_NUMBER=$((10#${START_FILE##*.}))
        RELAY_FILE="$DATA_DIR/$START_FILE"

        sleep 1 # let the first file's own header settle before issuing writes

        if ! "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_resume_test (v) VALUES ('$MARKER');"; then
            echo "FATAL: [identity] the marker transaction did not succeed"
            exit 1
        fi
        if ! wait_for_marker "$DATA_DIR" "$MARKER" "$FIRST_PID"; then
            echo "FATAL: [identity] relay did not catch up to the marker within 60s; stderr so far:"
            cat "$FIRST_LOG"
            exit 1
        fi

        kill -TERM "$FIRST_PID"
        EXITED=0
        for _ in $(seq 1 100); do
            if ! kill -0 "$FIRST_PID" 2>/dev/null; then EXITED=1; break; fi
            sleep 0.1
        done
        if [ "$EXITED" -ne 1 ]; then
            echo "FATAL: [identity] first run did not exit within 10s of SIGTERM; stderr so far:"
            cat "$FIRST_LOG"
            exit 1
        fi
        wait "$FIRST_PID" 2>/dev/null
        FIRST_EXIT_CODE=$?
        assert_no_purge "${LABEL:-identity}" "$FIRST_LOG" || exit 1
        FIRST_PID=""
        if [ "$FIRST_EXIT_CODE" -ne 0 ]; then
            echo "FATAL: [identity] expected exit code 0 after the first run's own SIGTERM, got $FIRST_EXIT_CODE"
            cat "$FIRST_LOG"
            exit 1
        fi

        SIZE_BEFORE_PATCH=$(wc -c < "$RELAY_FILE")
        printf '\xFF\xFF\xFF\xFE' | dd of="$RELAY_FILE" bs=1 seek=9 count=4 conv=notrunc 2>/dev/null
        SIZE_AFTER_PATCH=$(wc -c < "$RELAY_FILE")
        if [ "$SIZE_AFTER_PATCH" -ne "$SIZE_BEFORE_PATCH" ]; then
            echo "FATAL: [identity] patching server_id changed $RELAY_FILE's own size"
            exit 1
        fi

        # From here on $START_FILE only gets its "in use" flag cleared when
        # the restart abandons it; every other byte stays as the patch left it.
        if ! "${ADMIN[@]}" -e "FLUSH BINARY LOGS;"; then
            echo "FATAL: [identity] FLUSH BINARY LOGS failed"
            exit 1
        fi

        : > "$SECOND_LOG"
        "$BINARY" --config "$WORKDIR/settings.yml" >/dev/null 2>"$SECOND_LOG" &
        SECOND_PID=$!

        if ! wait_for_line "$SECOND_LOG" "dump requested from stored history" "$SECOND_PID"; then
            echo "FATAL: [identity] 'dump requested from stored history' did not appear in the restarted run's stderr within 30s:"
            cat "$SECOND_LOG"
            exit 1
        fi
        # 4278190079 is the patched bytes read back as little-endian
        # server_id; checked by value, not just presence, so a mutant
        # swapping "from"/"to" or naming the wrong file still fails.
        if ! grep -q "warning: source server_id has changed from 4278190079 (recorded in $START_FILE) to " "$SECOND_LOG"; then
            echo "FATAL: [identity] expected a server_id-changed warning naming 4278190079 and $START_FILE in stderr, got:"
            cat "$SECOND_LOG"
            exit 1
        fi
        if ! wait_for_line "$SECOND_LOG" "closed $START_FILE (.*since moved past" "$SECOND_PID"; then
            echo "FATAL: [identity] expected $START_FILE to be reported abandoned (source moved past it) within 30s, got:"
            cat "$SECOND_LOG"
            exit 1
        fi
        if ! "${ADMIN[@]}" binlog_streamer_test -e "INSERT INTO storage_resume_test (v) VALUES ('$POST_MARKER');"; then
            echo "FATAL: [identity] the post-restart transaction did not succeed"
            exit 1
        fi
        if ! wait_for_marker "$DATA_DIR" "$POST_MARKER" "$SECOND_PID"; then
            echo "FATAL: [identity] restarted relay did not catch up to the post-restart marker within 60s; stderr so far:"
            cat "$SECOND_LOG"
            exit 1
        fi

        kill -TERM "$SECOND_PID"
        EXITED=0
        for _ in $(seq 1 100); do
            if ! kill -0 "$SECOND_PID" 2>/dev/null; then EXITED=1; break; fi
            sleep 0.1
        done
        if [ "$EXITED" -ne 1 ]; then
            echo "FATAL: [identity] restarted relay did not exit within 10s of SIGTERM; stderr so far:"
            cat "$SECOND_LOG"
            exit 1
        fi
        wait "$SECOND_PID"
        SECOND_EXIT_CODE=$?
        assert_no_purge "${LABEL:-identity}" "$SECOND_LOG" || exit 1
        SECOND_PID=""
        if [ "$SECOND_EXIT_CODE" -ne 0 ]; then
            echo "FATAL: [identity] expected exit code 0 after the restarted run's own SIGTERM, got $SECOND_EXIT_CODE"
            cat "$SECOND_LOG"
            exit 1
        fi

        # No gap or unexpected history mismatch may ever be reported: the one
        # mismatch this scenario provokes is the server_id warning above, not
        # a "storage does not match source history" refusal.
        if grep -q "past what storage has written\|storage does not match source history" "$FIRST_LOG" "$SECOND_LOG"; then
            echo "FATAL: [identity] a gap or an unexpected history mismatch was detected somewhere across the two runs:"
            cat "$FIRST_LOG" "$SECOND_LOG"
            exit 1
        fi

        # while read, not mapfile - see storage_integration_test.sh's own
        # comment on why (this development source's macOS shell is bash 3.2).
        SORTED_NAMES=()
        while IFS= read -r NAME; do SORTED_NAMES+=("$NAME"); done < <(cd "$DATA_DIR" && ls | grep -E '^[^./]+\.[0-9]{6,}$' | sort -t. -k2 -n)
        FILE_COUNT=${#SORTED_NAMES[@]}
        if [ "$FILE_COUNT" -lt 2 ]; then
            echo "FATAL: [identity] found fewer than 2 files in $DATA_DIR ($START_FILE plus at least the file the source moved on to)"
            exit 1
        fi

        INDEX_CONTENT="$(cat "$DATA_DIR/binlog.index" 2>/dev/null)"
        EXPECTED_INDEX_CONTENT="$(printf '%s\n' "${SORTED_NAMES[@]}")"
        if [ "$INDEX_CONTENT" != "$EXPECTED_INDEX_CONTENT" ]; then
            echo "FATAL: [identity] $DATA_DIR/binlog.index does not match 'ls | sort' of $DATA_DIR:"
            echo "--- binlog.index ---"; echo "$INDEX_CONTENT"
            echo "--- ls | sort ---"; echo "$EXPECTED_INDEX_CONTENT"
            exit 1
        fi

        # Both sides still have $START_FILE (abandoned, never deleted), so
        # the same "count from the starting file onward" comparison applies.
        SOURCE_FILE_COUNT=0
        while IFS= read -r NAME; do
            NUMBER=$((10#${NAME##*.}))
            if [ "$NUMBER" -ge "$START_FILE_NUMBER" ]; then SOURCE_FILE_COUNT=$((SOURCE_FILE_COUNT + 1)); fi
        done < <(cd "$(dirname "$BINLOG_BASENAME")" && ls | grep -E "^$(basename "$BINLOG_BASENAME")\.[0-9]{6,}$")
        if [ "$FILE_COUNT" -ne "$SOURCE_FILE_COUNT" ]; then
            echo "FATAL: [identity] relay has $FILE_COUNT file(s), source has $SOURCE_FILE_COUNT file(s) from $START_FILE onward"
            exit 1
        fi

        # Size only, not a full untouched-bytes claim: abandoning the file
        # does rewrite its "in use" flag byte - that is the point, not a bug.
        SIZE_AFTER_RESTART=$(wc -c < "$RELAY_FILE")
        if [ "$SIZE_AFTER_RESTART" -ne "$SIZE_AFTER_PATCH" ]; then
            echo "FATAL: [identity] $RELAY_FILE's own size changed across the restart, expected only its own in-use flag byte to change"
            exit 1
        fi

        # $START_FILE is excluded from the byte-for-byte compare below: it
        # carries this scenario's own patch, not a real divergence.
        NEW_FILES=()
        for NAME in "${SORTED_NAMES[@]}"; do
            NUMBER=$((10#${NAME##*.}))
            if [ "$NUMBER" -gt "$START_FILE_NUMBER" ]; then NEW_FILES+=("$NAME"); fi
        done
        if [ "${#NEW_FILES[@]}" -lt 1 ]; then
            echo "FATAL: [identity] found no file after $START_FILE in $DATA_DIR"
            exit 1
        fi
        NEW_FILE_PATHS=()
        for NAME in "${NEW_FILES[@]}"; do NEW_FILE_PATHS+=("$DATA_DIR/$NAME"); done
        if ! grep -qas "$POST_MARKER" "${NEW_FILE_PATHS[@]}"; then
            echo "FATAL: [identity] the post-restart transaction ('$POST_MARKER') was not found in any of our files"
            exit 1
        fi

        LAST_INDEX=$((${#NEW_FILES[@]} - 1))
        FAIL=0
        for INDEX in "${!NEW_FILES[@]}"; do
            NAME="${NEW_FILES[$INDEX]}"
            OUR_FILE="$DATA_DIR/$NAME"
            THIS_SOURCE_FILE="${BINLOG_BASENAME}.${NAME##*.}"
            if [ ! -r "$THIS_SOURCE_FILE" ]; then
                echo "FATAL: [identity] cannot read $THIS_SOURCE_FILE"
                FAIL=1
                continue
            fi
            if ! mysqlbinlog --verify-binlog-checksum "$OUR_FILE" >/dev/null; then
                echo "FATAL: [identity] mysqlbinlog --verify-binlog-checksum rejected $OUR_FILE"
                FAIL=1
            fi
            OUR_SIZE=$(wc -c < "$OUR_FILE")
            if [ "$INDEX" -eq "$LAST_INDEX" ]; then
                if ! head -c "$OUR_SIZE" "$THIS_SOURCE_FILE" | cmp - "$OUR_FILE"; then
                    echo "FATAL: [identity] $OUR_FILE (current, $OUR_SIZE bytes) does not match the first $OUR_SIZE bytes of $THIS_SOURCE_FILE"
                    FAIL=1
                fi
            else
                if ! cmp "$THIS_SOURCE_FILE" "$OUR_FILE"; then
                    echo "FATAL: [identity] closed $OUR_FILE does not match $THIS_SOURCE_FILE byte for byte"
                    FAIL=1
                fi
            fi
        done
        if [ "$FAIL" -ne 0 ]; then exit 1; fi

        echo "OK: [identity] restart warned about a server_id change (exit 0), abandoned $START_FILE (size unchanged, only its own in-use flag cleared), ${#NEW_FILES[@]} file(s) since compared against $BINLOG_BASENAME.*"
    )
}

OVERALL_FAIL=0

if ! run_scenario crash 999006 KILL 1; then
    OVERALL_FAIL=1
fi

# Witness scenario: the same resume path also runs after an ordinary clean
# stop, not only after a crash - no artificial tail is injected here since a
# complete marker group is already on disk before the stop.
if ! run_scenario clean 999007 TERM 0; then
    OVERALL_FAIL=1
fi

if ! run_identity_mismatch_scenario; then
    OVERALL_FAIL=1
fi

if [ "$OVERALL_FAIL" -ne 0 ]; then
    exit 1
fi
echo "OK: the crash-restart, clean-restart and identity-mismatch scenarios all passed"
