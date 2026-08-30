#!/bin/sh
set -e
D=/tmp/asym_rig
T=/opensips
mkdir -p $D
rm -f $D/*.sock $D/*.db $D/*.log

python3 -c "import sqlite3; c=sqlite3.connect('$D/shared.db'); c.execute('CREATE TABLE IF NOT EXISTS cachedb_perf (collection TEXT, pkey TEXT, pvalue BLOB, expires INTEGER);'); c.close()"

# -------------------------------------------------------------
# 1. OpenSIPS Node 1 & Node 2 with cachedb_perf (P2P Mesh)
# -------------------------------------------------------------
for i in 1 2; do
cat > $D/perf_n$i.cfg <<EOF
log_level=2
max_while_loops=1000000
stderror_enabled=yes
syslog_enabled=no
udp_workers=8
socket=udp:127.0.0.1:508$i
socket=bin:127.0.0.1:558$i
mpath="$T/modules/"
loadmodule "proto_udp.so"
loadmodule "proto_bin.so"
loadmodule "db_sqlite.so"
loadmodule "clusterer.so"
loadmodule "mi_datagram.so"
loadmodule "cachedb_perf.so"
loadmodule "sl.so"

modparam("clusterer", "db_mode", 0)
modparam("clusterer", "my_node_id", $i)
modparam("clusterer", "my_node_info", "cluster_id=1, url=bin:127.0.0.1:558$i")
modparam("clusterer", "neighbor_node_info", "cluster_id=1, node_id=$((3-i)), url=bin:127.0.0.1:558$((3-i))")
modparam("mi_datagram", "socket_name", "$D/mi_perf$i.sock")
modparam("cachedb_perf", "cache_collections", "sync=14")
modparam("cachedb_perf", "cachedb_url", "perf:///sync")
modparam("cachedb_perf", "db_url", "sqlite://$D/shared.db")
modparam("cachedb_perf", "sync_cluster_id", 1)
modparam("cachedb_perf", "replicate_collections", "sync")
modparam("cachedb_perf", "pull_timeout_ms", 100)
modparam("cachedb_perf", "pull_negative_ms", 100)
modparam("cachedb_perf", "pull_on_miss", 1)

route {
    if (\$hdr(X-Store-Dialog) == "1") {
        cache_store("perf", "\$ci", "dialog-active-\$ci", 300);
        sl_send_reply(200, "Stored on Node $i");
        exit;
    }
    if (\$hdr(X-Fetch-Dialog) == "1") {
        if (cache_fetch("perf", "\$ci", \$var(v))) {
            sl_send_reply(200, "Resolved on Node $i");
        } else {
            sl_send_reply(404, "Not Found on Node $i");
        }
        exit;
    }
    sl_send_reply(200, "OK");
    exit;
}
EOF
done

# Start cachedb_perf Nodes
cd $T
./opensips -f $D/perf_n1.cfg -F > $D/perf_n1.log 2>&1 &
./opensips -f $D/perf_n2.cfg -F > $D/perf_n2.log 2>&1 &

sleep 3
echo "[+] 2x OpenSIPS cachedb_perf nodes started on 5081 & 5082"
