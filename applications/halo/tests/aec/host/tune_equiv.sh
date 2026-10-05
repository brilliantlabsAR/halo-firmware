#!/bin/bash
# Runtime tuning == compile-time override: for each case, run the FDAF
# host checks once with a -D override and once with the default build plus
# the equivalent AEC_TUNE runtime set (applied through audio_aec_tune_set
# before the first block), and require byte-identical output. The aec_tune
# checks (23) are excluded: they read the defaults, which differ by design.
set -e
cd "$(dirname "$0")"
M=../../../../../modules/halo
OUT=$(mktemp -d)
CFLAGS=(-O2 -Wall -I. -I "$M/include" -DCONFIG_HALO_AUDIO_AEC_TAPS=1024
	-DCONFIG_HALO_LOG_LEVEL=3 -DCONFIG_HALO_AUDIO_AEC_FDAF=1
	-DCONFIG_HALO_AUDIO_AEC_FDAF_PARTS=3)
build() { gcc "${CFLAGS[@]}" "$@" "$M/src/audio_aec.c" test_aec.c -lm; }

build -o "$OUT/def"
fail=0
# "runtime set|-D override"
for c in \
	"rearm_ms=160|-DAEC_SUP_ONSET_REARM_MS=160" \
	"gate_kappa=0.3|-DAEC_SUP_GATE_KAPPA=0.3f" \
	"gate_edge_ratio=1.8,gate_hang_ms=600|-DAEC_SUP_GATE_EDGE_RATIO=1.8f -DAEC_SUP_GATE_HANG=30" \
	"sup_beta=1.5|" ; do
	rt=${c%%|*}
	d=${c#*|}
	# shellcheck disable=SC2086
	build -o "$OUT/ovr" $d
	"$OUT/ovr" | grep -v '^aec_tune' > "$OUT/a.txt" || true
	AEC_TUNE=$rt "$OUT/def" | grep -v '^aec_tune' > "$OUT/b.txt" || true
	c19=$(grep '^mid-reply pauses' "$OUT/b.txt" | sed 's/  */ /g')
	if cmp -s "$OUT/a.txt" "$OUT/b.txt"; then
		echo "SAME  $rt == ${d:-defaults}: $c19"
	else
		echo "DIFF  $rt != ${d:-defaults}"
		diff "$OUT/a.txt" "$OUT/b.txt" | head -20
		fail=1
	fi
done
rm -rf "$OUT"
exit $fail
