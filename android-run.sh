#!/bin/sh

PUSH=false
adb shell ls -l /data/local/tmp/pulseaudio | grep rwx || PUSH=true
[ -n "$1" ] && PUSH=true

$PUSH && {
	adb shell 'sh -c "rm -r /data/local/tmp/*"'
	adb push armeabi-v7a/install/bin/pulseaudio /data/local/tmp/pulseaudio
	adb push armeabi-v7a/install/lib/libpulse.so.0.18.2 /data/local/tmp/
	adb shell ln -s libpulse.so.0.18.2 /data/local/tmp/libpulse.so.0
	adb shell ln -s libpulse.so.0.18.2 /data/local/tmp/libpulse.so
	adb push armeabi-v7a/install/lib/libpulsecore-7.0.so /data/local/tmp/
	adb push armeabi-v7a/install/lib/pulseaudio/libpulsecommon-7.0.so /data/local/tmp/
	adb push android-pulseaudio.conf /data/local/tmp/pulseaudio.conf
	for f in armeabi-v7a/install/lib/pulse-7.0/modules/*.so; do
		adb push $f /data/local/tmp/
	done
}

adb shell "cd /data/local/tmp ; HOME=/data/local/tmp TMPDIR=/data/local/tmp LD_LIBRARY_PATH=/data/local/tmp \
	./pulseaudio --disable-shm -n -F pulseaudio.conf --dl-search-path=/data/local/tmp \
	--daemonize=false --use-pid-file=false --log-target=stderr --log-level=debug"
