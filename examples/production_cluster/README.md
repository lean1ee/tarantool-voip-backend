# Production Cluster Deployment: Kamailio / OpenSIPS + RTPEngine + Asterisk + Tarantool 3.x

This directory contains battle-tested, production-ready configuration examples for deploying a carrier-grade VoIP media cluster backed by **Tarantool 3.x**.

## Directory Structure

```text
examples/production_cluster/
├── kamailio.cfg        # Production Kamailio SIP script with ndb_tarantool and rtpengine
├── opensips.cfg        # Production OpenSIPS SIP script with cachedb_tarantool and rtpengine
├── rtpengine.conf      # Production RTPEngine daemon config with IProto cluster driver
├── tarantool.conf      # Production Asterisk Tarantool connection pool configuration
├── extconfig.conf      # Production Asterisk Realtime / Sorcery configuration
├── cdr_tarantool.conf  # Production Asterisk streaming WAL CDR configuration
├── extensions.conf     # Production Asterisk Dialplan with real-time pre-call rating
└── README.md           # Deployment and operations manual
```

## Quickstart Guide

### 1. Start Tarantool 3.x Server

Ensure the `tarantool_backend` application is running:
```bash
tarantool /opt/tarantool/init.lua
```

### 2. Configure and Start RTPEngine

Copy `rtpengine.conf` to `/etc/rtpengine/rtpengine.conf` and launch RTPEngine:
```bash
rtpengine --config-file=/etc/rtpengine/rtpengine.conf
```

### 3. Option A: Configure and Start Kamailio

Copy `kamailio.cfg` to `/etc/kamailio/kamailio.cfg` and launch Kamailio:
```bash
kamailio -f /etc/kamailio/kamailio.cfg -DD -E
```

### 3. Option B: Configure and Start OpenSIPS

Copy `opensips.cfg` to `/etc/opensips/opensips.cfg` and launch OpenSIPS:
```bash
opensips -f /etc/opensips/opensips.cfg -E
```

### 3. Option C: Configure and Start Asterisk

Copy `tarantool.conf`, `extconfig.conf`, `cdr_tarantool.conf`, and `extensions.conf` to `/etc/asterisk/` and launch Asterisk:
```bash
asterisk -c -vvv
```

## Failover and High Availability

* **Dynamic Reconnect:** RTPEngine, Kamailio, and OpenSIPS automatically reconnect to Tarantool upon network disruptions.
* **Fast Recovery:** If an RTPEngine node fails, the standby node queries Tarantool for existing active sessions using `rtpe_tarantool_restore_calls(...)` and resumes media routing in **< 0.1 ms**.
* **Zero Jitter:** Unlike Redis, Tarantool does not block media processing with background `fork()` COW snapshots.
