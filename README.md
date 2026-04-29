```
root@router:~# cat /etc/default/traffmon
name=router

heartbeat-period=60

traffic-poll-period=60
traffic-iface[1]=switch.lan:public:1:uplink
traffic-iface[2]=switch.lan:public:2:server

mqtt-server=mqtt://localhost
mqtt-client=traffmon
mqtt-topic-prefix=system/traffic

root@router:~# systemctl restart traffmon && mosquitto_sub -t system/traffic/#
{ "timestamp": 1761738933, "hostname": "router", "name": "router", "event": "startup", "success": true, "message": "daemon started" }
{ "timestamp": 1761738995, "hostname": "router", "name": "router", "event": "traffic", "success": true, "message": "switch.lan/uplink 60s rx:12.3MB(1.72Mbps) tx:4.5MB(0.63Mbps)", "device": "switch.lan", "interface": "uplink", ... }
{ "timestamp": 1761738999, "hostname": "router", "name": "router", "event": "heartbeat", "success": true, "message": "daemon active (1)", "traffic_enabled": true, "traffic_targets": 2 }
```

## discover

The `discover` helper walks SNMP `ifDescr` on one or more devices and prints `traffic-iface[N]=...` entries you can paste into the config:

```
root@router:~# ./discover 192.168.0.128 192.168.0.107:private >> /etc/default/traffmon
```
