#!/bin/sh
set -eu

work=$(mktemp -d "${TMPDIR:-/tmp}/tunix-hda-sim.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
${HOSTCC:-cc} -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-unused-function -Ikernel/include \
	-DTUNIX_MODULE_NAME='"snd_hda"' testsuites/hda-sim/sim.c -o "$work/hda-sim"
"$work/hda-sim"
