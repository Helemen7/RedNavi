#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Helemen7 and RedNavi contributors

# Which registry properties and which matching classes WindowServer asks the kernel for while it starts (dtrace on the kernel's IOKit MIG routines, root), counted, into
# /Users/Shared/ws_props15.txt. WindowServer is killed after 6 s so that the whole start is traced. Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace15.sh >/dev/null 2>&1 &'
OUT=/Users/Shared/ws_props15.txt
ERR=/Users/Shared/ws_trace_err15.txt
D=/Users/Shared/ws_trace15.d
rm -f $OUT $ERR
echo "start $(date)" > $ERR
cat > $D <<'EOD'
fbt::is_io_registry_entry_get_property:entry /execname == "WindowServer"/ { @p["get_property", stringof(arg1)] = count(); }
fbt::is_io_registry_entry_get_property_bytes:entry /execname == "WindowServer"/ { @p["get_property_bytes", stringof(arg1)] = count(); }
fbt::is_io_registry_entry_get_property_recursively:entry /execname == "WindowServer"/ { @p["get_property_recursively", stringof(arg1)] = count(); }
fbt::is_io_registry_entry_get_properties_bin_buf:entry /execname == "WindowServer"/ { @p["get_properties_bin", "(all)"] = count(); }
fbt::is_io_service_get_matching_service:entry /execname == "WindowServer"/ { @m["get_matching_service"] = count(); }
fbt::is_io_service_get_matching_services_bin:entry /execname == "WindowServer"/ { @m["get_matching_services_bin"] = count(); }
fbt::is_io_registry_entry_from_path:entry /execname == "WindowServer"/ { @m["entry_from_path"] = count(); }
tick-75s { printa("%s %s %@d\n", @p); printa("%s %@d\n", @m); exit(0); }
EOD
dtrace -Z -q -o $OUT -s $D >> $ERR 2>&1 &
DT=$!
sleep 5
echo "killing WindowServer $(pgrep -x WindowServer) $(date)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
wait $DT
chmod 644 $OUT $ERR
