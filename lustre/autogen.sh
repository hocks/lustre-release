#!/bin/bash

# taken from gnome-common/macros2/autogen.sh
compare_versions() {
    ch_min_version=$1
    ch_actual_version=$2
    ch_status=0
    IFS="${IFS=         }"; ch_save_IFS="$IFS"; IFS="."
    set $ch_actual_version
    for ch_min in $ch_min_version; do
        ch_cur=`echo $1 | sed 's/[^0-9].*$//'`; shift # remove letter suffixes
        if [ -z "$ch_min" ]; then break; fi
        if [ -z "$ch_cur" ]; then ch_status=1; break; fi
        if [ $ch_cur -gt $ch_min ]; then break; fi
        if [ $ch_cur -lt $ch_min ]; then ch_status=1; break; fi
    done
    IFS="$ch_save_IFS"
    return $ch_status
}

error_msg() {
	echo "$cmd is $1.  version $required is required to build Lustre."

	if [ -e /usr/lib/autolustre/bin/$cmd ]; then
		cat >&2 <<-EOF
		You apparently already have Lustre-specific autoconf/make RPMs
		installed on your system at /usr/lib/autolustre/share/$cmd.
		Please set your PATH to point to those versions:

		export PATH="/usr/lib/autolustre/bin:\$PATH"
		EOF
	else
		cat >&2 <<-EOF
		CFS provides RPMs which can be installed alongside your
		existing autoconf/make RPMs, if you are nervous about
		upgrading.  See

		ftp://ftp.lustre.org/pub/other/autolustre/README.autolustre

		You may be able to download newer version from:

		http://ftp.gnu.org/gnu/$cmd/$cmd-$required.tar.gz
	EOF
	fi
	[ "$cmd" = "autoconf" -a "$required" = "2.57" ] && cat >&2 <<EOF

or for RH9 systems you can use:

ftp://fr2.rpmfind.net/linux/redhat/9/en/os/i386/RedHat/RPMS/autoconf-2.57-3.noarch.rpm
EOF
	[ "$cmd" = "automake" -a "$required" = "1.7.8" ] && cat >&2 <<EOF

or for RH9 systems you can use:

ftp://fr2.rpmfind.net/linux/fedora/core/1/i386/os/Fedora/RPMS/automake-1.7.8-1.noarch.rpm
EOF
	exit 1
}

check_version() {
    local cmd
    local required
    local version

    cmd=$1
    required=$2
    echo -n "checking for $cmd $required... "
    if ! $cmd --version >/dev/null ; then
	error_msg "missing"
    fi
    version=$($cmd --version | awk "BEGIN { IGNORECASE=1 } /$cmd \(GNU $cmd\)/ { print \$4 }")
    echo "found $version"
    if ! compare_versions "$required" "$version" ; then
	error_msg "too old"
    fi
}

check_version automake "1.7.8"
check_version autoconf "2.57"
echo "Running aclocal..."
aclocal
echo "Running autoheader..."
autoheader
echo "Running automake..."
automake -a -c
echo "Running autoconf..."
autoconf

