# Changelog

Changes by minor version; each entry includes its patch releases.

## 0.41 — 2026-09-28

### Changed

- `server.send_linger` defaults to 200us: less CPU and faster delivery with
  many replicas at high transaction rates. A replica that has caught up
  waits for new events without polling.
- Lower CPU use for replicas catching up from stored history.
- Licensed under the GNU General Public License 2.0, with a permission to
  link with OpenSSL.

## 0.40 — 2026-09-27

### Changed

- `server.max_connections` defaults to 64 instead of 128.

## 0.39 — 2026-09-27

### Added

- `server.send_linger` in `settings.yml`, off by default: a replica that has
  caught up gets new events in batches, at most this delay later.

### Changed

- Lower CPU use over TLS, when receiving from the source and when serving
  stored history.
- Lower memory use per replica.

## 0.38 — 2026-09-26

### Changed

- The status page is three configuration files in
  `/etc/binlog-streamer/html`: `index.html`, `status.css` and `status.js`.
- Lower CPU use with replicas that keep up with the source; a replica that
  reconnects catches up faster.

### Fixed

- The status page sometimes showed an empty byte lag for a replica keeping
  up with the source.

## 0.37 — 2026-09-25

### Added

- TLS and compression of the source connection on the status page and in
  `/status.json`.

### Changed

- The status page shows a replica's address under its report_host.

## 0.36 — 2026-09-25

### Added

- The status page names replicas by their report_host; `/status.json` gains
  `report_host`.

## 0.35 — 2026-09-25

### Changed

- The systemd unit is `Type=notify`: `systemctl start` and `restart` fail
  when the configuration is rejected.

## 0.34 — 2026-09-25

### Added

- Each replica's program and version in `/status.json` and on the status
  page.
- `systemctl reload` rereads the configuration; a configuration that does
  not validate fails the reload and the relay keeps running.

### Changed

- The status page shows the source's `server_uuid` on its own row.

### Fixed

- The relay did not exit on SIGTERM when started with SIGHUP ignored, for
  example under `nohup`.

## 0.33 — 2026-09-24

### Added

- SIGHUP reloads the configuration: new `hosts` of configured replica users
  apply at once and connected replicas stay; other changes are logged as
  needing a restart.

### Fixed

- A replica refused before login saw `#HY000` inside the error message.
- An error from the source without an SQL state was rejected as malformed.

## 0.32 — 2026-09-24

### Added

- Zabbix template for `/status.json`, installed under
  `/usr/share/doc/binlog-streamer/zabbix/`.

### Changed

- Shorter comments in the packaged `source.yml` and `replica.yml`.

### Fixed

- SIGHUP stopped the relay.

## 0.31 — 2026-09-24

### Added

- Disk and memory use against their limits in `/status.json`.

### Changed

- The status page is a dashboard: source, replicas, disk and memory at a
  glance, coloured by thresholds.
- `server.max_connections` defaults to 128 instead of 151.

## 0.30 — 2026-09-24

### Added

- Relay state over HTTP: `/status.json` for monitoring and a status page,
  set in `monitoring.http` of `settings.yml` (default `0.0.0.0:8080`). The
  listener has no TLS or authentication of its own.

## 0.29 — 2026-09-23

### Added

- TLS on both connections. Replicas are offered it, with a configured or a
  generated certificate, and `require_secure_transport` can require it; the
  source connection uses it as `ssl_mode` in `source.yml` says.

### Changed

- A dump by file name and position is refused with error 1236 naming the
  reason, instead of 1047.

## 0.28 — 2026-09-23

### Changed

- A lost source stream no longer ends the run: the relay reconnects while
  replicas stay connected, and exits with code 3 after ten failed attempts.

## 0.27 — 2026-09-23

### Changed

- Replicas see the source's version with the relay's suffix, for example
  `8.4.11-binlog-streamer-0.27.0`, instead of `9.7.0`.
- Until anything is stored, connections are refused with error 3168.

## 0.26 — 2026-09-23

### Added

- `server.max_connections` in `settings.yml`, 151 by default.

### Fixed

- A `SELECT` of several variables was answered "Unknown system variable".

## 0.25 — 2026-09-23

### Added

- zlib protocol compression on both connections.

### Fixed

- A refused ROTATE closed the file being written, and the next start
  refused to continue.

## 0.24 — 2026-09-23

### Added

- zstd protocol compression on both connections: offered to replicas by
  default, asked of the source when `compression` in `source.yml` says so.

## 0.23 — 2026-09-22

### Added

- A relay can use another relay as its source.

### Changed

- Higher receive throughput with many replicas waiting at the end of the
  stream.
- Less CPU when sending stored history to a replica.
- The packaged `settings.yml` explains how to size its limits.

### Fixed

- The relay did not exit when the source went away while a replica waited
  for events.
- Receiving stopped when the source was stopped or crashed and continued in
  a new file.
- `mysqlbinlog --raw --to-last-log` did not store each following file under
  its own name.
- A source whose `gtid_purged` holds GTIDs no binary log contains refused
  the relay's dump with error 1236.

## 0.22 — 2026-09-21

### Added

- `mysqlbinlog --read-from-remote-source=BINLOG-DUMP-GTIDS` can read the
  stored binary log from the relay.

## 0.21 — 2026-09-21

### Added

- A dump leaves out transactions the replica already has and sends
  heartbeats while there is nothing to send.
- `SELECT @@GLOBAL.gtid_executed` is answered.

## 0.20 — 2026-09-21

### Added

- A replica requesting a dump by GTID receives the stored binary log and
  then new events as they arrive.

## 0.19 — 2026-09-21

### Added

- The relay answers the statements a replica sends before its dump request
  and has a `server_uuid` of its own, kept in `auto.cnf`.

## 0.18 — 2026-09-21

### Added

- Replicas can connect and log in with `caching_sha2_password`: listen
  address and accounts in `replica.yml`, MySQL error codes for refused
  connections.

## 0.17 — 2026-09-21

### Fixed

- A tagged GTID transaction on the source stopped the relay.

## 0.16 — 2026-09-21

### Changed

- A relay with no stored history starts at the source's current binary log
  file instead of going back by `storage.retention.period`.

## 0.15 — 2026-09-21

### Added

- `storage.disk` limits are enforced: the oldest files are removed past
  `purge_high_watermark` or below `min_free_space`, and the relay stops
  with exit code 4 when it cannot stay within `max_size`.

### Changed

- The packaged `settings.yml` sets `cache.max_size` to 4G instead of 2G and
  `storage.disk.max_size` to 256G instead of 2T.

## 0.14 — 2026-09-19

### Added

- Binary log files older than `storage.retention.period` are removed.

## 0.13 — 2026-09-18

### Added

- `cache.window` releases cached events older than the window.

## 0.12 — 2026-09-18

### Added

- Received events are kept in an in-memory cache of `cache.max_size`.

## 0.11 — 2026-09-18

First release.

- Connects to one MySQL or Percona Server 8.0+ source as a replica and
  stores its binary log by GTID, in files that mirror the source's.
- Resumes from stored history after a restart, checking that it belongs to
  the same source.
- Authenticates with `caching_sha2_password`.
- Reads settings from `/etc/binlog-streamer/`; `--validate-config` checks
  them.
- Debian/Ubuntu package with a systemd service.
