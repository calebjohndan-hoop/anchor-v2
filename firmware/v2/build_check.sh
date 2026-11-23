#!/bin/bash
# Compile and link an Arduino sketch for "ESP32S3 Dev Module" (16MB flash, OPI PSRAM, USB CDC on boot)
# with the ESP32 Arduino core 3.0.7, without arduino-cli. Usage: build_check.sh <sketch_dir>
set -e
A=${ANCHOR_TOOLS:?set ANCHOR_TOOLS to the folder with core/, libs/ and xtensa-esp32s3-elf/}
CORE=$A/core/esp32-3.0.7
SDK=$A/libs/esp32-arduino-libs/esp32s3
BIN=$A/xtensa-esp32s3-elf/bin
SK=$(cd "$1" && pwd); NAME=$(basename "$SK"); LIB="${2:-}"
B=$A/build/$NAME; CB=$A/build/_core
mkdir -p "$B" "$CB"
MEM=qio_opi
DEFS="-DF_CPU=240000000L -DARDUINO=10812 -DARDUINO_ESP32S3_DEV -DARDUINO_ARCH_ESP32 -DARDUINO_BOARD=\"ESP32S3_DEV\" -DARDUINO_VARIANT=\"esp32s3\" -DARDUINO_PARTITION_app3M_fat9M_16MB -DARDUINO_HOST_OS=\"linux\" -DARDUINO_FQBN=\"esp32:esp32:esp32s3\" -DESP32=ESP32 -DCORE_DEBUG_LEVEL=0 -DBOARD_HAS_PSRAM -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MSC_ON_BOOT=0 -DARDUINO_USB_DFU_ON_BOOT=0"
INC="@$SDK/flags/defines -iprefix $SDK/include/ @$SDK/flags/includes -I$SDK/$MEM/include -I$CORE/cores/esp32 -I$CORE/variants/esp32s3"
CXX="$BIN/xtensa-esp32s3-elf-g++"; CC="$BIN/xtensa-esp32s3-elf-gcc"
compile() { # src obj
  case "$1" in
    *.c) $CC -MMD -c @$SDK/flags/c_flags -w -Os -Werror=return-type $DEFS $INC -I"$SK" "$1" -o "$2" ;;
    *.S) $CC -c -x assembler-with-cpp @$SDK/flags/S_flags -w -Os $DEFS $INC "$1" -o "$2" ;;
    *)   $CXX -MMD -c @$SDK/flags/cpp_flags -w -Os -Werror=return-type $DEFS $INC -I"$SK" "$1" -o "$2" ;;
  esac
}
export -f compile; export CC CXX SDK DEFS INC SK
# core (once)
if [ ! -f "$CB/core.a" ]; then
  find $CORE/cores/esp32 -name '*.c' -o -name '*.cpp' -o -name '*.S' | while read f; do
    o="$CB/$(echo ${f#$CORE/cores/esp32/} | tr '/' '_').o"; echo "$f $o"; done | xargs -P 8 -n 2 bash -c 'compile "$0" "$1"'
  $BIN/xtensa-esp32s3-elf-ar rcs "$CB/core.a" "$CB"/*.o
fi
# optional library (for example an Edge Impulse export): compile its src/ once
if [ -n "$LIB" ]; then
  LB=$A/build/_lib_$(basename $LIB); mkdir -p $LB
  INC="$INC -I$LIB/src"; export INC
  if [ ! -f "$LB/lib.a" ]; then
    find $LIB/src \( -name '*.c' -o -name '*.cpp' -o -name '*.cc' \) | grep -v "/examples/\|/test/" | while read f; do
      o="$LB/$(echo ${f#$LIB/src/} | tr '/' '_').o"; echo "$f $o"; done | xargs -P 8 -n 2 bash -c 'case "$0" in *.cc) cp "$0" "${1%.o}.cpp"; compile "${1%.o}.cpp" "$1";; *) compile "$0" "$1";; esac'
    $BIN/xtensa-esp32s3-elf-ar rcs "$LB/lib.a" "$LB"/*.o
  fi
  EXTRA_A="$LB/lib.a"
fi
# sketch: .ino -> .cpp with Arduino.h first
cp "$SK/$NAME.ino" "$B/$NAME.ino.cpp"; sed -i '1i #include <Arduino.h>' "$B/$NAME.ino.cpp"
compile "$B/$NAME.ino.cpp" "$B/$NAME.o"
for f in "$SK"/*.cpp "$SK"/*.c; do [ -f "$f" ] && compile "$f" "$B/$(basename $f).o"; done
# link
$CXX @$SDK/flags/ld_flags @$SDK/flags/ld_scripts "-Wl,--Map=$B/$NAME.map" -L$SDK/lib -L$SDK/ld -L$SDK/$MEM -Wl,--wrap=esp_panic_handler \
  -Wl,--start-group "$B"/*.o $EXTRA_A "$CB/core.a" @$SDK/flags/ld_libs -Wl,--end-group -Wl,-EL -o "$B/$NAME.elf"
$BIN/xtensa-esp32s3-elf-size -A "$B/$NAME.elf" | grep -E "^\.(flash\.text|flash\.rodata|dram0\.data|dram0\.bss|iram0\.text)"
echo "BUILD OK: $B/$NAME.elf"
