#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
output_dir="$script_dir/datasheets"
mkdir -p "$output_dir"

download_pdf() {
    local url="$1"
    local name="$2"
    local destination="$output_dir/$name"
    echo "Downloading $name"
    curl --fail --location --retry 2 --output "$destination" "$url"
    if [[ "$(head -c 5 "$destination")" != "%PDF-" ]]; then
        echo "Downloaded file is not a PDF: $destination" >&2
        exit 1
    fi
}

download_pdf \
    'https://content.u-blox.com/sites/default/files/products/documents/NEO-6_DataSheet_%28GPS.G6-HW-09005%29.pdf' \
    'ublox-neo-6-datasheet.pdf'
download_pdf \
    'https://content.u-blox.com/sites/default/files/products/documents/u-blox6_ReceiverDescrProtSpec_%28GPS.G6-SW-10018%29_Public.pdf' \
    'ublox-6-protocol-specification.pdf'
download_pdf \
    'https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Datasheet.pdf' \
    'mpu-6050-product-specification.pdf'
download_pdf \
    'https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf' \
    'mpu-6050-register-map.pdf'
download_pdf \
    'https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf' \
    'bmp280-datasheet.pdf'
