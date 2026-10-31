#!/bin/sh
set -eu

wav=$1
wanted=$2
test -s "$wav" || { echo "sound: no audio was recorded"; exit 1; }
longest=$(od -An -v -t u2 -w4 -j 44 "$wav" | awk '
	{
		right = $2
		if (right >= 16384 && previous >= 16384 && right - 16384 == (previous - 16384 + 1) % 16384) run++
		else run = right >= 16384 ? 1 : 0
		if (run > best) best = run
		previous = right
	}
	END { print best + 0 }')
echo "sound: longest unbroken run of the reference tone: $longest frames"
test "$longest" -ge "$wanted"
