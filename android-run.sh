#!/bin/sh

PUSH=false
adb shell ls -l /data/local/tmp/pulseaudio | grep rwx || PUSH=true
[ -n "$1" ] && PUSH=true

if $PUSH; then
	adb shell 'sh -c "rm -r /data/local/tmp/*"'
	adb push busybox /data/local/tmp/

	adb push arm64-v8a/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib/libjson-c.so /data/local/tmp/
	adb push arm64-v8a/libsndfile-1.0.25/install/lib/libsndfile.so /data/local/tmp/
	adb push arm64-v8a/libtool-master/install/lib/libltdl.so /data/local/tmp/

	adb push arm64-v8a/install/bin/pulseaudio /data/local/tmp/
	adb push arm64-v8a/install/lib/libpulse.so /data/local/tmp/libpulse.so
	adb push arm64-v8a/install/lib/pulseaudio/libpulsecore-13.0.so /data/local/tmp/
	adb push arm64-v8a/install/lib/pulseaudio/libpulsecommon-13.0.so /data/local/tmp/
	adb push android-pulseaudio.conf /data/local/tmp/pulseaudio.conf
	for f in arm64-v8a/install/lib/pulse-13.0/modules/*.so; do
		adb push $f /data/local/tmp/
	done
else
	adb push arm64-v8a/install/lib/pulse-13.0/modules/module-opensles.so /data/local/tmp/
fi


trap_ctrlc () {
    # perform cleanup here
    echo
    echo "Ctrl-C caught...performing clean up"

    adb shell "killall pulseaudio"

    echo
    echo "clean up done"

    # exit shell script with error code 2
    # if omitted, shell script will continue execution
    exit 2
}

# initialise trap to call trap_ctrlc function
# when signal 2 (SIGINT) is received
trap "trap_ctrlc" 2

{
	sleep 2
	#env PULSE_SERVER=tcp:192.168.42.129:4713 paplay music.ogg
	env PULSE_SERVER=tcp:192.168.42.129:4713 pavucontrol
} &

adb shell "cd /data/local/tmp ; HOME=/data/local/tmp TMPDIR=/data/local/tmp LD_LIBRARY_PATH=/data/local/tmp \
	./pulseaudio --disable-shm -n -F pulseaudio.conf --dl-search-path=/data/local/tmp \
	--daemonize=false --use-pid-file=false --log-target=stderr --log-level=debug --exit-idle-time=31622400 --disallow-exit"

