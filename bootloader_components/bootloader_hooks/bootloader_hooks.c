#include "esp_log.h"
#include "esp_rom_sys.h"
#include "bootloader_common.h"

/* Function used to tell the linker to include this file
 * with all its symbols.
 */
void bootloader_hooks_include(void){
}


void bootloader_before_init(void) {
}

void bootloader_after_init(void) {
    soc_reset_reason_t reset_reason = esp_rom_get_reset_reason(0);
    //log reset reason for debugging
    if (reset_reason == RESET_REASON_CORE_MWDT0 ||
        reset_reason == RESET_REASON_CORE_MWDT1 ||
        reset_reason == RESET_REASON_CORE_RTC_WDT ||
        reset_reason == RESET_REASON_CPU0_MWDT0 ||
        reset_reason == RESET_REASON_CPU0_RTC_WDT ||
        reset_reason == RESET_REASON_SYS_RTC_WDT ||
        reset_reason == RESET_REASON_CPU0_MWDT1 ||
        reset_reason == RESET_REASON_SYS_SUPER_WDT
        ) {
        // Detected a panic reset - log it and let the bootloader handle fallback
        ESP_LOGW("HOOK", "Watchdog Reset! Bootloader will fallback to factory partition.");
        bootloader_common_erase_part_type_data("", true);
    } else {
        return;
    }
}
