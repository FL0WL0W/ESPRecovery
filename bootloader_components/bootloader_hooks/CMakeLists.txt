# Bootloader component - register the hooks
idf_component_register(SRCS "bootloader_hooks.c"
                        PRIV_REQUIRES bootloader_support)