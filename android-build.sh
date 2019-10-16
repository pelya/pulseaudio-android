#!/bin/sh

BUILD_PARALLEL=false
NCPU=8
ARCH_LIST="arm64-v8a x86_64 x86 armeabi-v7a"
ARCH_LIST="arm64-v8a"

[ -e libtool-2.4.6.tar.gz ] || wget http://ftpmirror.gnu.org/libtool/libtool-2.4.6.tar.gz || exit 1
[ -e 12916e229c769da4929f6df7f038ab51cf0cb067.tar.gz ] || wget https://github.com/json-c/json-c/archive/12916e229c769da4929f6df7f038ab51cf0cb067.tar.gz || exit 1
[ -e libsndfile-1.0.25.tar.gz ] || wget http://www.mega-nerd.com/libsndfile/files/libsndfile-1.0.25.tar.gz || exit 1

[ -e configure ] || {
	env NOCONFIGURE=1 ./bootstrap.sh || exit 1
}

build() {

	ARCH=$1

	case $ARCH in
		arm64-v8a) TOOLCHAIN=aarch64-linux-android;;
		x86_64) TOOLCHAIN=x86_64-linux-android;;
		armeabi-v7a) TOOLCHAIN=arm-linux-androideabi;;
		x86) TOOLCHAIN=i686-linux-android;;
	esac

	mkdir -p $ARCH
	cd $ARCH

	[ -e libtool-master/install/lib/libltdl.so ] || {
		rm -rf libtool-master
		tar xvf ../libtool-master.tar.gz || exit 1
		cd libtool-master
		mkdir -p install
		./bootstrap || exit 1
		env CFLAGS=-DLT_DEBUG_LOADERS=1 \
			../../setCrossEnvironment-$ARCH.sh ./configure --host=$TOOLCHAIN --prefix=`pwd`/install || exit 1
		#env CFLAGS=-DLT_DEBUG_LOADERS=1 \
		#	./configure --prefix=`pwd`/install || exit 1
		make -j$NCPU V=1
		mkdir -p install/lib
		#../../setCrossEnvironment-$ARCH.sh sh -c \
		#	'$AR rcs install/lib/libltdl.a libltdl/.libs/*.o libltdl/loaders/.libs/*.o' || exit 1
		../../setCrossEnvironment-$ARCH.sh sh -c \
			'$CC $LDFLAGS -shared -o install/lib/libltdl.so libltdl/.libs/*.o libltdl/loaders/.libs/*.o' || exit 1
		make install-data || exit 1
		cd ..
	} || exit 1

	[ -e json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib/libjson-c.so ] || {
		rm -rf json-c-12916e229c769da4929f6df7f038ab51cf0cb067
		tar xvf ../12916e229c769da4929f6df7f038ab51cf0cb067.tar.gz || exit 1
		cd json-c-12916e229c769da4929f6df7f038ab51cf0cb067
		mkdir -p install
		env ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
		../../setCrossEnvironment-$ARCH.sh ./autogen.sh \
			--host=$TOOLCHAIN \
			--prefix=`pwd`/install \
			--disable-shared \
			--enable-static \
			--with-gnu-ld \
			|| exit 1
		make -j$NCPU || exit 1
		make install-data || exit 1
		mkdir -p install/lib
		#../../setCrossEnvironment-$ARCH.sh sh -c \
		#	'$AR rcs install/lib/libjson-c.a *.o' || exit 1
		../../setCrossEnvironment-$ARCH.sh sh -c \
			'$CC $LDFLAGS -shared -o install/lib/libjson-c.so *.o' || exit 1
		cd ..
	} || exit 1

	[ -e libsndfile-1.0.25/install/lib/libsndfile.so ] || {
		rm -rf libsndfile-1.0.25
		tar xvf ../libsndfile-1.0.25.tar.gz || exit 1
		cd libsndfile-1.0.25
		mkdir -p install
		cp -f ../../android-config.sub Cfg/config.sub
		cp -f ../../android-config.guess Cfg/config.guess
		autoreconf
		../../setCrossEnvironment-$ARCH.sh ./configure \
			--host=$TOOLCHAIN \
			--prefix=`pwd`/install \
			--disable-shared \
			--disable-external-libs \
			--disable-alsa \
			--enable-static \
			|| exit 1
		echo 'int main () {}' > programs/sndfile-play.c
		make -j$NCPU V=1 -k
		make install-data || exit 1
		mkdir -p install/lib
		#../../setCrossEnvironment-$ARCH.sh sh -c \
		#	'$AR rcs install/lib/libsndfile.a src/*.o src/*/*.o' || exit 1
		../../setCrossEnvironment-$ARCH.sh sh -c \
			'$CC $LDFLAGS -shared -o install/lib/libsndfile.so src/*.o src/*/*.o' || exit 1
		cd ..
	} || exit 1

	mkdir -p install

	[ -e Makefile ] || {
		env \
		CFLAGS=" \
			-I`pwd`/libtool-master/install/include \
			-I`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/include/json-c \
			-I`pwd`/libsndfile-1.0.25/install/include \
			-Werror=implicit-function-declaration" \
		LDFLAGS=" \
			-L`pwd`/libtool-master/install/lib \
			-L`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib \
			-L`pwd`/libsndfile-1.0.25/install/lib -pie" \
		LIBS="-ljson-c -lsndfile" \
		LIBJSON_CFLAGS=-I`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/include \
		LIBJSON_LIBS="-L`pwd`/json-c-12916e229c769da4929f6df7f038ab51cf0cb067/install/lib -ljson-c" \
		LIBSNDFILE_CFLAGS=-I`pwd`/libsndfile-1.0.25/install/include \
		LIBSNDFILE_LIBS="-L`pwd`/libsndfile-1.0.25/install/lib -lsndfile" \
		ALLOW_UNRESOLVED_SYMBOLS=1 \
		ac_cv_func_mkfifo=yes \
		ax_cv_PTHREAD_PRIO_INHERIT=no \
		ac_cv_header_langinfo_h=no \
		ac_cv_header_glob_h=no \
		../setCrossEnvironment-$ARCH.sh \
		  ../configure            \
		  --prefix=`pwd`/install  \
		  --host=$TOOLCHAIN       \
		  --disable-nls           \
		  --disable-rpath         \
		  --disable-neon-opt      \
		  --disable-static        \
		  --enable-shared         \
		  --disable-x11           \
		  --disable-tests         \
		  --disable-oss-output    \
		  --disable-oss-wrapper   \
		  --disable-coreaudio-output \
		  --disable-alsa          \
		  --disable-esound        \
		  --disable-solaris       \
		  --disable-glib2         \
		  --disable-gtk3          \
		  --disable-gconf         \
		  --disable-avahi         \
		  --disable-jack          \
		  --disable-asyncns       \
		  --disable-tcpwrap       \
		  --disable-lirc          \
		  --disable-dbus          \
		  --disable-bluez4        \
		  --disable-bluez5        \
		  --disable-udev          \
		  --disable-hal-compat    \
		  --disable-openssl       \
		  --disable-xen           \
		  --disable-systemd-daemon \
		  --disable-systemd-login \
		  --disable-systemd-journal \
		  --disable-manpages      \
		  --without-caps          \
		|| exit 1

		patch -p0 < ../libtool.patch || { rm Makefile ; exit 1 ; }
	} || exit 1

	make -j$NCPU V=1
	make install-strip || exit 1
	cd ..
}

for ARCH in $ARCH_LIST; do
	if $BUILD_PARALLEL; then
		build $ARCH &
	else
		build $ARCH
	fi
done

wait

for ARCH in $ARCH_LIST; do
	[ -e $ARCH/install/bin/pulseaudio ] || exit 1
done
exit 0
