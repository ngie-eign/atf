#!/bin/sh
#
# Step 2: run `make distcheck`.
#
# `make distcheck` builds the tests, runs them, and subsequently performs a
# style check on the code.
#
# SPDX-License-Identifier: BSD-2-Clause

set -eux

: ${AS_ROOT=no}
: ${CC=cc}
: ${CXX=c++}
: ${EXTRA_DISTCHECK_CONFIGURE_ARGS=}

NPROC=$(nproc 2>/dev/null || getconf NPROCESSORS_ONLN 2>/dev/null || echo 1)

f=
f="${f} ATF_BUILD_CC='${CC}'"
f="${f} ATF_BUILD_CXX='${CXX}'"
if [ -n "${EXTRA_DISTCONFIGURE_ARGS:-}" ]; then
    f="${f} ${EXTRA_DISTCONFIGURE_ARGS}"
fi

kyua_conf="$(mktemp kyua-XXXXXXXX.conf)" || exit
trap 'rm -f "${kyua_conf}"' EXIT INT TERM

sudo=

cat >"${kyua_conf}" <<EOF
syntax(2)

unprivileged_user = 'nobody'
EOF

if [ "x${AS_ROOT}" = xyes ]; then
    sudo="sudo -H"
fi
precmd="${sudo} env KYUA_TEST_CONFIG_FILE=${kyua_conf} PATH='${PATH}'"

if ! ${precmd} make distcheck DISTCHECK_CONFIGURE_FLAGS="${f}" -j${NPROC}; then
    cat atf-*/_build/sub/config.log
    exit 1
fi

# vim: syntax=sh:expandtab:shiftwidth=4:softtabstop=4
