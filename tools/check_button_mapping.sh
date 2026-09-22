#!/bin/sh
# Verify that each member of the S2_VEHICLE_BUTTON Kconfig choice selects the
# intended entry of the table in src/vehicle_button.c.
#
# The native test environment cannot cover this: test/sdkconfig.h pins one choice
# for the whole run, so the unit tests reach the other buttons through vbtn_get()
# and never exercise vbtn_selected()'s preprocessor chain. This script compiles
# that chain once per option against a synthetic sdkconfig.h instead.
#
# Usage: tools/check_button_mapping.sh     (from the project root; exit 0 = pass)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CC=${CC:-cc}

cat > "$work/main.c" <<'EOF'
#include <stdio.h>
#include "vehicle_button.h"
int main(void)
{
    const vbtn_def_t *s = vbtn_selected();
    printf("%s\n", s ? s->label : "NONE");
    return 0;
}
EOF

pass=0
fail=0
check() {
    symbols=$1
    expect=$2
    printf '#pragma once\n' > "$work/sdkconfig.h"
    for sym in $symbols; do printf '#define %s 1\n' "$sym" >> "$work/sdkconfig.h"; done
    $CC -std=gnu11 -Wall -Wextra -I"$root/include" -I"$root/src" -I"$root/src/gen" -I"$work" \
        "$work/main.c" "$root/src/vehicle_button.c" "$root/src/gen/s2_dbc_gen.c" -o "$work/m"
    got=$("$work/m")
    if [ "$got" = "$expect" ]; then
        printf '  ok   %-34s -> %s\n' "${symbols:-<none>}" "$got"
        pass=$((pass + 1))
    else
        printf '  FAIL %-34s -> expected "%s", got "%s"\n' "${symbols:-<none>}" "$expect" "$got"
        fail=$((fail + 1))
    fi
}

V=CONFIG_S2_DISPLAY_BUTTON_VEHICLE
check "$V CONFIG_S2_VEHICLE_BUTTON_INFO"        "info/scroll"
check "$V CONFIG_S2_VEHICLE_BUTTON_HORN"        "horn"
check "$V CONFIG_S2_VEHICLE_BUTTON_HIGHBEAM"    "high beam"
check "$V CONFIG_S2_VEHICLE_BUTTON_FRONT_BRAKE" "front brake"
check "$V CONFIG_S2_VEHICLE_BUTTON_REAR_BRAKE"  "rear brake"
check "$V CONFIG_S2_VEHICLE_BUTTON_HAZARDS"     "hazards"
check "$V CONFIG_S2_VEHICLE_BUTTON_CRUISE"      "cruise arm"
check "$V"                                      "info/scroll"
check "CONFIG_S2_DISPLAY_BUTTON_PIN"            "NONE"
check ""                                        "NONE"

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
