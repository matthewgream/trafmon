```
root@router:~# cat /etc/default/traffmon
name=router

heartbeat-period=60

traffic-poll-period=60

# bundle 'switch' - all interfaces on the local switch
traffic-iface[switch][1]=switch.lan:public:1:uplink
traffic-iface[switch][2]=switch.lan:public:2:server

# bundle 'uplink' - aggregates two physical devices into one logical group
traffic-iface[uplink][1]=router-a.lan:public:2:wan
traffic-iface[uplink][2]=router-b.lan:public:2:wan

mqtt-server=mqtt://localhost
mqtt-client=traffmon
mqtt-topic-prefix=system/traffic

root@router:~# systemctl restart traffmon && mosquitto_sub -t system/traffic/#
{ "timestamp": 1761738933, "hostname": "router", "name": "router", "event": "startup", "success": true, "message": "daemon started" }
{ "timestamp": 1761738995, "hostname": "router", "name": "switch", "event": "traffic", "success": true, "message": "2/2 interfaces ok",
  "interfaces": [
    { "device": "switch.lan", "interface": "uplink", "duration": 60, "in_octets": ..., "out_octets": ..., "in_bps": ..., ... },
    { "device": "switch.lan", "interface": "server", "duration": 60, ... }
  ] }
{ "timestamp": 1761738995, "hostname": "router", "name": "uplink", "event": "traffic", "success": true, "message": "2/2 interfaces ok",
  "interfaces": [
    { "device": "router-a.lan", "interface": "wan", ... },
    { "device": "router-b.lan", "interface": "wan", ... }
  ] }
{ "timestamp": 1761738999, "hostname": "router", "name": "router", "event": "heartbeat", "success": true, "message": "daemon active (1)",
  "traffic_enabled": true, "traffic_bundles": 2, "traffic_targets": 4 }
```

## bundles

Each `traffic-iface[<bundle>][<n>]` entry assigns an interface to a named bundle. Per poll, all interfaces in a bundle are reported in a single MQTT message published to `<mqtt-topic-prefix>/<bundle>`. A bundle can span multiple physical devices — useful for aggregating, e.g., redundant uplinks on different routers into one logical group.

Daemon-level events (`startup`, `shutdown`, `heartbeat`) publish to `<mqtt-topic-prefix>/<name>`, where `name` is the daemon's instance name (defaults to hostname).

## discover

The `discover` helper walks SNMP `ifDescr` on one or more devices and prints `traffic-iface[<host>][<n>]=...` entries you can paste into the config. The bundle name defaults to the host argument — rename it however you like:

```
root@router:~# ./discover 192.168.0.128 192.168.0.107:private >> /etc/default/traffmon
```
