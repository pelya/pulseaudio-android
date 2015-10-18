#!/bin/sh

adb shell ls -l /data/local/tmp/pulseaudio | grep rwx || {
	adb push pulseaudio-armeabi-v7a /data/local/tmp/pulseaudio
	adb push android-pulseaudio.conf /data/local/tmp/pulseaudio.conf
}


adb shell "cd /data/local/tmp ; HOME=/data/local/tmp ./pulseaudio --disable-shm -n -F pulseaudio.conf --use-pid-file=false --log-target=stderr --log-level=debug --daemonize=false"
