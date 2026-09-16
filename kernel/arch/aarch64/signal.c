#include <stdint.h>

#include "../../include/pmm.h"
#include "../../include/process_arch.h"
#include "../../include/vmm.h"
#include "../../include/vmm_arch.h"

#define INSTRUCTION_MOV_X8_RT_SIGRETURN 0xD2801168U
#define INSTRUCTION_SVC_0 0xD4000001U

static uint64_t trampoline_physical;

int arch_map_signal_trampoline(uint64_t cr3) {
    if (!trampoline_physical) {
        uint64_t physical = (uint64_t)pmm_alloc_page();
        if (!physical) return -1;
        uint32_t *code = (uint32_t *)vmm_phys_to_virt(physical);
        for (unsigned index = 0; index < 1024U; index++) code[index] = INSTRUCTION_SVC_0;
        code[0] = INSTRUCTION_MOV_X8_RT_SIGRETURN;
        code[1] = INSTRUCTION_SVC_0;
        vmm_arch_sync_executable(physical);
        trampoline_physical = physical;
    }
    int status = vmm_map_page_in(cr3, ARCH_SIGNAL_TRAMPOLINE, trampoline_physical,
                                 PAGE_USER | PAGE_DEVICE);
    return status == -2 ? 0 : status;
}
