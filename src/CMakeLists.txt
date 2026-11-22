idf_component_register(
    SRCS "main.c" "datalog.c" "ble.c" "usb_cmd.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES bt ulp nvs_flash esp_driver_gpio esp_driver_usb_serial_jtag esp_timer esp_hw_support
)

# Build the LP-core program in ../ulp and embed it. Its globals are exported
# to the HP side as ulp_<name> via the generated ulp_main.h.
set(ulp_app_name ulp_main)
set(ulp_sources "../ulp/main.c")
set(ulp_exp_dep_srcs "datalog.c" "main.c")
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
