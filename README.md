# trafmon

Traffic monitor. Periodically polls network-interface counters (via SNMP) on
one or more devices and publishes per-interface throughput as structured JSON
to an MQTT broker.

One of a small family of single-binary MQTT monitor daemons that share a common
structure and config convention:

- **hostmon** — this host's system + platform metrics
- **connmon** — outbound connectivity (UPnP port maps, reachability, dynamic DNS)
- **trafmon** — per-interface network traffic counters (via SNMP)

## What it does

Each poll it walks the configured SNMP interfaces, computes deltas (octets in/out,
bits/sec, errors, oper status, speed) over the poll window, and reports them.
Interfaces are grouped into named **bundles**; all interfaces in a bundle are
published together in one message. A bundle can span multiple physical devices —
handy for aggregating, e.g., redundant uplinks on different routers into one
logical group. Plus lifecycle `startup` / `shutdown` and a periodic `heartbeat`.

## Build

Single C source (`trafmon.c`) with headers in `include/`, plus the `discover`
helper (`discover.c`). Needs an SNMP-reachable device to poll and an MQTT broker.

    make                    # native build (trafmon + discover)
    make armhf              # cross-compile for 32-bit ARM (armhf)
    make format             # clang-format the source
    make test               # build and run against the local .cfg
    make clean

## Install

**Host-specific config convention:** the Makefile installs
`trafmon.<hostname>.cfg` if it exists, otherwise falls back to the generic
`trafmon.cfg`. Commit one config per deployment host (e.g. `trafmon.bastu.cfg`)
alongside the documented default `trafmon.cfg`.

    make install-dev          # native:  binary -> /usr/local/bin/trafmon
                              #          config -> /etc/default/trafmon
                              #          unit   -> trafmon.service (enabled)
    make install-dev-armhf    # same, from the armhf cross-build
    make remove-dev           # uninstall

Runs as the systemd service `trafmon.service`; runtime config is
`/etc/default/trafmon`.

## Configuration

Every setting is a config-file `key=value` **and** an equivalent `--key value`
command-line flag. `--config <file>` selects the file (default `trafmon.cfg`).

### MQTT (common to hostmon / connmon / trafmon)

| key | default | meaning |
|---|---|---|
| `mqtt-server` | `mqtt://localhost` | broker URL |
| `mqtt-client` | `trafmon` | client id |
| `mqtt-topic-prefix` | `system/traffic` | base topic |
| `mqtt-tls-insecure` | `false` | skip TLS cert verification |
| `mqtt-reconnect-delay` | `5` | reconnect backoff start (s) |
| `mqtt-reconnect-delay-max` | `60` | reconnect backoff cap (s) |

### trafmon-specific

| key | default | meaning |
|---|---|---|
| `name` | *(hostname)* | instance name — topic for daemon-level events |
| `heartbeat-period` | `60` | heartbeat interval (s) |
| `traffic-poll-period` | `60` | SNMP poll interval (s) |
| `traffic-iface[<bundle>][<n>]` | | interface to poll — `device:community:ifIndex:name` |

`traffic-iface[<bundle>][<n>]` assigns SNMP interface `<n>` to a named `<bundle>`,
e.g. `traffic-iface[sauna][1]=192.168.0.224:public:2:uplink` (device
`192.168.0.224`, community `public`, SNMP ifIndex `2`, label `uplink`). Entries
are 1-indexed and repeatable.

## Output (MQTT)

- **Traffic** — one message per bundle per poll, to **`<mqtt-topic-prefix>/<bundle>`**,
  carrying an `interfaces` array.
- **Daemon events** (`startup` / `shutdown` / `heartbeat`) — to
  **`<mqtt-topic-prefix>/<name>`**.

```
$ mosquitto_sub -t 'system/traffic/#'
{ "timestamp":1761738933, "hostname":"bastu", "name":"bastu", "event":"startup", "success":true, "message":"daemon started" }
{ "timestamp":1761738995, "hostname":"bastu", "name":"sauna", "event":"traffic", "success":true,
  "message":"2/2 interfaces ok",
  "interfaces":[
    { "device":"192.168.0.224", "interface":"uplink", "duration":60, "in_octets":…, "out_octets":…, "in_bps":…, "out_bps":…, "oper_status":1, "speed_mbps":100 },
    { "device":"192.168.0.225", "interface":"server", "duration":60, … } ] }
{ "timestamp":1761738999, "hostname":"bastu", "name":"bastu", "event":"heartbeat", "success":true,
  "message":"daemon active (1)", "traffic_enabled":true, "traffic_bundles":1, "traffic_targets":7 }
```

## Companion tool: `discover`

Walks SNMP `ifDescr` on one or more devices and prints ready-to-paste
`traffic-iface[<host>][<n>]=…` config lines (bundle defaults to the host arg;
rename as you like):

    ./discover 192.168.0.224 192.168.0.225:private >> /etc/default/trafmon

## License

CC BY-NC-SA 4.0 — see [LICENSE](LICENSE).
