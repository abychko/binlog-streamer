# CTest driver for --validate-config against a real binary. Invoked as:
#   cmake -DBINARY=<path> -DPACKAGING_DIR=<path> -DSCENARIO=<owner|broken> -P validate_config_test.cmake

IF(NOT DEFINED BINARY OR NOT DEFINED PACKAGING_DIR OR NOT DEFINED SCENARIO)
    MESSAGE(FATAL_ERROR "BINARY, PACKAGING_DIR and SCENARIO must be set")
ENDIF()

IF(SCENARIO STREQUAL "owner")
    # The positive path needs privileged setup this script can't perform, and
    # running as root would make this negative case pass trivially: skipped,
    # visible to ctest via SKIP_REGULAR_EXPRESSION.
    EXECUTE_PROCESS(COMMAND id -u OUTPUT_VARIABLE CURRENT_UID OUTPUT_STRIP_TRAILING_WHITESPACE)
    IF(CURRENT_UID STREQUAL "0")
        MESSAGE(STATUS "skipped: running as root")
        RETURN()
    ENDIF()
ELSEIF(NOT SCENARIO MATCHES "^(broken|developer)$")
    MESSAGE(FATAL_ERROR "unknown SCENARIO: ${SCENARIO}")
ENDIF()

EXECUTE_PROCESS(COMMAND mktemp -d OUTPUT_VARIABLE WORK_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)

# Collected instead of calling MESSAGE(FATAL_ERROR) directly, so WORK_DIR is
# always removed below even when a check fails (script mode has no
# try/finally; FATAL_ERROR would otherwise abort before the cleanup line).
SET(FAILURE_MESSAGE "")

IF(SCENARIO STREQUAL "owner")
    FILE(COPY "${PACKAGING_DIR}/settings.yml" DESTINATION "${WORK_DIR}")
    # Filled from the current (non-root) user's own values; the files end up
    # owned by that user, which violates the "owner must be root" contract.
    FILE(WRITE "${WORK_DIR}/source.yml" "host: 127.0.0.1\nuser: repl\npassword: Secr3tPass\n")
    FILE(WRITE "${WORK_DIR}/replica.yml" "listen_port: 3307\n")
    EXECUTE_PROCESS(COMMAND chmod 0640 "${WORK_DIR}/source.yml" "${WORK_DIR}/replica.yml")
    EXECUTE_PROCESS(COMMAND "${BINARY}" --validate-config --config "${WORK_DIR}/settings.yml"
        RESULT_VARIABLE EXIT_CODE OUTPUT_VARIABLE STDOUT_TEXT ERROR_VARIABLE STDERR_TEXT)
    IF(NOT EXIT_CODE EQUAL 1)
        SET(FAILURE_MESSAGE "expected exit code 1, got ${EXIT_CODE}; stdout: ${STDOUT_TEXT}; stderr: ${STDERR_TEXT}")
    ELSEIF(NOT STDERR_TEXT MATCHES "wrong directory owner")
        # WORK_DIR itself isn't owned by root, so the directory-ownership
        # check fails first; file ownership is covered by SecretFileCheckTest.
        SET(FAILURE_MESSAGE "expected 'wrong directory owner' in stderr, got: ${STDERR_TEXT}")
    ENDIF()
ELSEIF(SCENARIO STREQUAL "developer")
    # Only meaningful in a DEVELOPER_MODE=ON build: the same not-root-owned
    # files the "owner" scenario rejects must be accepted here instead, with
    # the relaxation visible on stderr.
    FILE(COPY "${PACKAGING_DIR}/settings.yml" DESTINATION "${WORK_DIR}")
    FILE(WRITE "${WORK_DIR}/source.yml" "host: 127.0.0.1\nuser: repl\npassword: Secr3tPass\n")
    FILE(WRITE "${WORK_DIR}/replica.yml" "listen_port: 3307\n")
    EXECUTE_PROCESS(COMMAND chmod 0640 "${WORK_DIR}/source.yml" "${WORK_DIR}/replica.yml")
    EXECUTE_PROCESS(COMMAND "${BINARY}" --validate-config --config "${WORK_DIR}/settings.yml"
        RESULT_VARIABLE EXIT_CODE OUTPUT_VARIABLE STDOUT_TEXT ERROR_VARIABLE STDERR_TEXT)
    IF(NOT EXIT_CODE EQUAL 0)
        SET(FAILURE_MESSAGE "expected exit code 0 in a developer-mode build, got ${EXIT_CODE}; stdout: ${STDOUT_TEXT}; stderr: ${STDERR_TEXT}")
    ELSEIF(NOT STDERR_TEXT MATCHES "developer mode")
        SET(FAILURE_MESSAGE "expected a developer-mode banner in stderr, got: ${STDERR_TEXT}")
    ENDIF()
ELSE() # broken
    FILE(WRITE "${WORK_DIR}/settings.yml" "server:\n  server_id: not-a-number\n")
    EXECUTE_PROCESS(COMMAND "${BINARY}" --validate-config --config "${WORK_DIR}/settings.yml"
        RESULT_VARIABLE EXIT_CODE OUTPUT_VARIABLE STDOUT_TEXT ERROR_VARIABLE STDERR_TEXT)
    IF(NOT EXIT_CODE EQUAL 1)
        SET(FAILURE_MESSAGE "expected exit code 1, got ${EXIT_CODE}; stdout: ${STDOUT_TEXT}; stderr: ${STDERR_TEXT}")
    ELSEIF(NOT STDERR_TEXT MATCHES "settings\\.yml:2:3: server\\.server_id: ")
        # Exit code 1 alone doesn't distinguish: an empty WORK_DIR without
        # source.yml also exits 1.
        SET(FAILURE_MESSAGE "expected the server_id error at settings.yml:2:3 in stderr, got: ${STDERR_TEXT}")
    ENDIF()
ENDIF()

FILE(REMOVE_RECURSE "${WORK_DIR}")

IF(NOT FAILURE_MESSAGE STREQUAL "")
    MESSAGE(FATAL_ERROR "${FAILURE_MESSAGE}")
ENDIF()
