# LVGL 9.6 deprecated LV_MEM_SIZE_KILOBYTES in favour of LV_MEM_SIZE (bytes).
# The deprecated symbol still exists in Kconfig and still reads back whatever
# sdkconfig sets, so a guard pointed at it reports a healthy 256 KiB while
# LVGL quietly allocates its own 64 KiB default. Measured on the physical
# panel 2026-09-16: boot wedged in lv_obj_class_create_obj's out-of-memory
# assert while building the WiFi setup screen's 196x196 QR canvas, with the
# task watchdog barking at IDLE0 every five seconds and no way out but USB.
# Guard the value LVGL actually reads, in the unit LVGL actually uses.
set(TORGET_LVGL_POOL_MIN_KIB 256)
math(EXPR TORGET_LVGL_POOL_MIN_BYTES "${TORGET_LVGL_POOL_MIN_KIB} * 1024")

function(torget_require_lvgl_pool actual_bytes)
  if("${actual_bytes}" STREQUAL "" OR NOT "${actual_bytes}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "LVGL pool size is missing or invalid: '${actual_bytes}'")
  endif()

  if(actual_bytes LESS TORGET_LVGL_POOL_MIN_BYTES)
    math(EXPR _actual_kib "${actual_bytes} / 1024")
    message(FATAL_ERROR
      "LVGL pool is ${_actual_kib} KiB; Torget requires at least "
      "${TORGET_LVGL_POOL_MIN_KIB} KiB. The generated sdkconfig is stale, or "
      "sets only the deprecated LV_MEM_SIZE_KILOBYTES: edit sdkconfig and set "
      "CONFIG_LV_MEM_SIZE=${TORGET_LVGL_POOL_MIN_BYTES} "
      "(matching sdkconfig.defaults), then run: "
      "idf.py reconfigure && idf.py build")
  endif()
endfunction()
