#!/bin/sh
# Assert that composite-cli was built with the features this image advertises.
#
# `composite-cli --version` reports only the project version, so it cannot distinguish an image
# with the OTLP exporter compiled in from one without it. That is not hypothetical: the 0.5.0-rc.1
# images shipped with OpenTelemetry, DPDK and TLS all compiled out, and every smoke test passed.
# This checks the capability surface itself.
#
# Usage: check-features.sh <ON|OFF>   # whether DPDK is expected
set -eu

expect_dpdk="${1:-OFF}"
features="$(composite-cli --features)"

require() {
    if ! printf '%s\n' "$features" | grep -qx "$1"; then
        echo "FEATURE CHECK FAILED: expected '$1' in composite-cli --features" >&2
        echo "got: [$features]" >&2
        exit 1
    fi
}

refuse() {
    if printf '%s\n' "$features" | grep -qx "$1"; then
        echo "FEATURE CHECK FAILED: '$1' present but this image must not advertise it" >&2
        echo "got: [$features]" >&2
        exit 1
    fi
}

# Every image, every variant.
require opentelemetry
require openssl

if [ "$expect_dpdk" = "ON" ]; then
    require dpdk
else
    refuse dpdk
fi

echo "feature check OK (dpdk=${expect_dpdk}): ${features}" | tr '\n' ' '
echo
