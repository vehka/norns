#!/data/data/com.termux/files/usr/bin/bash
# stop norns, sclang/scsynth and jack
#
# match on the command line: norns and ws-wrapper are started by path, and
# the sidecar child renames itself to "sidecar [norns]"
stop() { # signal, pattern
    pkill "$1" -f "$2" 2>/dev/null
}

for pat in 'build/ws-wrapper/ws-wrapper' 'build/norns/norns' '^sclang' '^scsynth'; do
    stop -TERM "$pat"
done
sleep 1
# the sidecar ignores SIGTERM while blocked in nng; sweep up anything left
for pat in 'build/ws-wrapper/ws-wrapper' 'build/norns/norns' '^sidecar \[norns\]' '^sidecar$' '^sclang' '^scsynth'; do
    stop -KILL "$pat"
done
stop -TERM '^jack-aaudio'
stop -TERM '^jackd'
sleep 1
stop -KILL '^jackd'
stop -KILL '^jack-aaudio'
command -v termux-wake-unlock >/dev/null && termux-wake-unlock
exit 0
