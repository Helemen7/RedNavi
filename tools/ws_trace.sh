#!/bin/sh
# Traces which registry properties WindowServer reads while it starts (dtrace, root), to /Users/Shared/ws_props.txt, with dtrace's own messages in /Users/Shared/ws_trace_err.txt.
# Run: sudo sh -c 'nohup sh /Users/Shared/ws_trace.sh >/dev/null 2>&1 &'   (a launchctl job is refused dtrace: "requires additional privileges"; the nohup child survives the end of the login session)
OUT=/Users/Shared/ws_props.txt
ERR=/Users/Shared/ws_trace_err.txt
rm -f $OUT $ERR
echo "start $(date)" > $ERR
dtrace -q -o $OUT -n '
fbt::is_io_registry_entry_get_property_bytes:entry /execname == "WindowServer"/ { printf("bytes %s\n", stringof(arg1)); }
fbt::is_io_registry_entry_get_property:entry /execname == "WindowServer"/ { printf("prop %s\n", stringof(arg1)); }
fbt::is_io_registry_entry_get_property_recursively:entry /execname == "WindowServer"/ { printf("recursively %s plane %s\n", stringof(arg2), stringof(arg1)); }
fbt::is_io_registry_entry_get_property_bin_buf:entry /execname == "WindowServer"/ { printf("bin_buf %s | %s\n", stringof(arg1), stringof(arg2)); }
fbt::is_io_registry_entry_get_property_bin:entry /execname == "WindowServer"/ { printf("bin %s | %s\n", stringof(arg1), stringof(arg2)); }
fbt::is_io_registry_entry_get_properties_bin_buf:entry /execname == "WindowServer"/ { printf("all_bin_buf\n"); }
fbt::is_io_service_open_extended:entry /execname == "WindowServer"/ { printf("open type %d\n", (int)arg2); }
fbt::*newUserClient*:entry /execname == "WindowServer"/ { printf("newUserClient %s type %d\n", probefunc, (int)arg3); }
' >> $ERR 2>&1 &
DT=$!
sleep 6
echo "killing WindowServer $(pgrep -x WindowServer)" >> $ERR
pkill -9 -x WindowServer >> $ERR 2>&1
sleep 80
kill $DT
chmod 644 $OUT $ERR
