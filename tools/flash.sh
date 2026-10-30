#!/bin/sh
# Reflash a running n2k_usb_can adapter over USB from Linux, no button needed.
#
#   tools/flash.sh firmware.bin [console-port]
#
# firmware.bin is the merged image (offset 0x0). console-port defaults to the
# adapter's CDC console under /dev/serial/by-id/. Needs esptool and stty, and
# permission to open the serial ports (dialout group, or run with sudo).
# Set ESPTOOL_ARGS=--no-stub where esptool ships without its flasher stub
# (Debian's esptool package).
#
# Steps:
#   1. 1200-baud touch on the console: the firmware reboots into the ROM
#      bootloader, which appears as an Espressif USB-Serial/JTAG port.
#   2. esptool writes the image.
#   3. Clear RTC_CNTL_OPTION1_REG (force download boot). It lives in the RTC
#      domain and survives a reset, so without this the chip keeps booting
#      into the bootloader.
#
# Both esptool runs use --before default_reset. On the USB-Serial/JTAG port
# DTR overrides the boot mode and RTS resets the chip; Linux raises DTR when
# it opens the port, so unless esptool drives the lines itself the final
# reset lands back in the bootloader (or does not reset at all).

set -eu

IMAGE=${1:?usage: flash.sh firmware.bin [console-port]}
CONSOLE=${2:-$(ls /dev/serial/by-id/*n2k_usb_can*-if01 2>/dev/null | head -n1 || true)}
ESPTOOL=$(command -v esptool || command -v esptool.py || true)
ESPTOOL_ARGS=${ESPTOOL_ARGS:-}
RTC_CNTL_OPTION1_REG=0x6000812C

[ -f "$IMAGE" ] || { echo "no such file: $IMAGE" >&2; exit 1; }
[ -n "$ESPTOOL" ] || { echo "esptool not found" >&2; exit 1; }

find_rom_port() {
    ls /dev/serial/by-id/*Espressif_USB_JTAG_serial_debug_unit* 2>/dev/null | head -n1
}

ROM_PORT=$(find_rom_port || true)
if [ -z "$ROM_PORT" ]; then
    [ -n "$CONSOLE" ] || { echo "adapter console not found; pass the port" >&2; exit 1; }
    echo "touch $CONSOLE at 1200 baud"
    stty -F "$CONSOLE" 1200
    for _ in $(seq 1 20); do
        sleep 0.5
        ROM_PORT=$(find_rom_port || true)
        [ -n "$ROM_PORT" ] && break
    done
fi
[ -n "$ROM_PORT" ] || { echo "bootloader did not appear" >&2; exit 1; }
echo "bootloader on $ROM_PORT"

# shellcheck disable=SC2086
"$ESPTOOL" --chip esp32s3 -p "$ROM_PORT" $ESPTOOL_ARGS --before default_reset --after no_reset \
    write_flash 0x0 "$IMAGE"
# shellcheck disable=SC2086
"$ESPTOOL" --chip esp32s3 -p "$ROM_PORT" $ESPTOOL_ARGS --before default_reset --after hard_reset \
    write_mem "$RTC_CNTL_OPTION1_REG" 0
echo "done"
