export PATH=$PATH:/sbin:/usr/sbin

if [ -d /r ]; then
	R=/r
fi

PORTALS=$SRCDIR/../../portals
LUSTRE=$SRCDIR/../../obd

PTLCTL=$PORTALS/linux/utils/ptlctl
DEBCTL=$PORTALS/linux/utils/debugctl
ACCEPTOR=$PORTALS/linux/utils/acceptor

OBDCTL=$LUSTRE/utils/obdctl

LOOPNUM=0; export LOOPNUM
if [ -b /dev/loop0 ]; then
	LOOP=/dev/loop
else
	if [ -b /dev/loop/0 ]; then
		LOOP=/dev/loop/
	else
		echo "Cannot find /dev/loop0 or /dev/loop/0" 1>&2 && exit -1
	fi
fi

# Return the next unused loop device on stdout and in the $LOOPDEV
# environment variable.
next_loop_dev() {
	NEXT=
	while [ -b ${LOOP}${LOOPNUM} ]; do
		LOOPDEV=${LOOP}${LOOPNUM}
		losetup ${LOOPDEV} > /dev/null 2>&1 || NEXT=${LOOPDEV}
		LOOPNUM=`expr ${LOOPNUM} + 1`
		[ "$NEXT" ] && echo ${NEXT} && break
	done
}

# Create a new filesystem.  If we are using a loopback device, we check
# for existing "template" filesystems instead of creating a new one,
# because it is _much_ faster to gunzip the empty filesystem instead of
# creating a new one from scratch.  Conversely, if we are creating a
# filesystem on a device we use mkfs, because that only writes sparsely
# to the device.  The empty filesystems are also highly compressed (1000:1)
# so they don't take too much space.
new_fs () {
 	EFILE="$1_$3.gz"
	MKFS="mkfs.$1"
	MKFSOPT="-b 4096"

	[ "$1" = "ext3" ] && MKFS="mkfs.ext2 -j"

	if [ -b "$2" ]; then
		[ $# -lt 2 -o $# -gt 3 ] && \
			echo "usage: $0 <fstype> <file> [size]" 1>&2 && exit -1

		PM="/proc/mounts"
		[ -f "$PM" ] || PM="/etc/mtab"

		grep "$2 " $PM 1>&2 && echo "$0: $2 is in $PM!" 1>&2 && exit -1

		$MKFS $MKFSOPT $2 $3 || exit -1
		LOOPDEV=$2	# Not really a loop device
	else
		[ $# -ne 3 ] && \
			echo "usage: $0 <fstype> <file> <size>" 1>&2 && exit -1

		if [ -f "$EFILE" ]; then
			echo "using existing filesystem $EFILE for $2"
			zcat "$EFILE" > $2 || exit -1
			sync
		else
			echo "creating new filesystem on $2"
			dd if=/dev/zero of=$2 bs=1k count=$3 1>&2 || exit -1
			$MKFS $MKFSOPT -F $2 1>&2 || exit -1
		fi
		LOOPDEV=`next_loop_dev`
		losetup ${LOOPDEV} $2 1>&2 || exit -1
	fi
}

# Set up to use an existing filesystem.  We take the same parameters as
# new_fs, even though we only use the <file> parameter, to make it easy
# to convert between new_fs and old_fs in testing scripts.
old_fs () {
	[ -e $2 ] || exit -1

	if [ -b "$2" ]; then
		LOOPDEV=$2	# Not really a loop device
	else
		LOOPDEV=`next_loop_dev`
		losetup ${LOOPDEV} $2 1>&2 || exit -1
	fi
}

list_mods() {
	$DEBCTL modules > $R/tmp/ogdb
	echo "The GDB module script is in /tmp/ogdb"
}

setup_portals() {
	if [ -z "$NETWORK" -o -z "$LOCALHOST" -o -z "$SERVER" ]; then
		echo "$0: NETWORK or LOCALHOST or SERVER is not set" 1>&2
		exit -1
	fi

	[ -c /dev/portals ] || mknod /dev/portals c 10 240

	insmod $PORTALS/linux/oslib/portals.o || exit -1

	case $NETWORK in
	elan)	if [ "$PORT" ]; then
			echo "$0: NETWORK is elan but PORT is set" 1>&2
			exit -1
		fi
		insmod $PORTALS/linux/qswnal/kqswnal.o
		;;
	tcp)	if [ -z "$PORT" ]; then
			echo "$0: NETWORK is tcp but PORT is not set" 1>&2
			exit -1
		fi
		insmod $PORTALS/linux/socknal/ksocknal.o || exit -1
		$ACCEPTOR $PORT
		;;
	*) 	echo "$0: unknown NETWORK \'$NETWORK\'" 1>&2
		exit -1
		;;
	esac

	$PTLCTL <<- EOF
	setup $NETWORK
	mynid $LOCALHOST
	connect $SERVER $PORT
	add_uuid self
	add_uuid mds
	add_uuid ost
	quit
	EOF
}

setup_lustre() {
	[ -c /dev/obd ] || mknod /dev/obd c 10 241

	insmod $LUSTRE/class/obdclass.o || exit -1
	insmod $LUSTRE/rpc/ptlrpc.o || exit -1
	insmod $LUSTRE/ldlm/ldlm.o || exit -1
	insmod $LUSTRE/ext2obd/obdext2.o || exit -1
	insmod $LUSTRE/filterobd/obdfilter.o || exit -1
	insmod $LUSTRE/ost/ost.o || exit -1
	insmod $LUSTRE/osc/osc.o || exit -1
	insmod $LUSTRE/obdecho/obdecho.o || exit -1
	insmod $LUSTRE/mds/mds.o || exit -1
	insmod $LUSTRE/mdc/mdc.o || exit -1
	insmod $LUSTRE/llight/llite.o || exit -1

	list_mods

	[ -d /mnt/lustre ] || mkdir /mnt/lustre
}

setup_ldlm() {
	[ -c /dev/portals ] || mknod /dev/portals c 10 240

	insmod $PORTALS/linux/oslib/portals.o || exit -1

	insmod $LUSTRE/class/obdclass.o || exit -1
	insmod $LUSTRE/ldlm/ldlm.o || exit -1

	list_mods
}
