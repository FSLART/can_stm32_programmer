#include <stdint.h>
#include <stdlib.h>
extern uint32_t _sdata, _edata, _sidata, _sbss, _ebss;
extern void initialise_monitor_handles(void);
extern int main(void);
void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    for (uint32_t *p = &_sdata; p < &_edata;) *p++ = *src++;
    for (uint32_t *p = &_sbss; p < &_ebss;) *p++ = 0;
    *(volatile uint32_t *)0xe000ed88 |= 0xfU << 20;
    initialise_monitor_handles();
    exit(main());
}
__attribute__((section(".vectors"), used))
const uintptr_t vectors[] = {0x20400000, (uintptr_t)Reset_Handler};
