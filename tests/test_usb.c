#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "devices.h"

int main(void) {
    uint8_t cbw[31] = {0};
    cbw[15] = 0x28;       /* READ(10) */
    cbw[17] = 0x12;       /* LBA = 0x12345678 */
    cbw[18] = 0x34;
    cbw[19] = 0x56;
    cbw[20] = 0x78;
    cbw[22] = 0x00;       /* SCSI transfer length is big-endian */
    cbw[23] = 0x01;
    assert(usb_msc_be16(&cbw[22]) == 1);
    cbw[22] = 0x01;
    cbw[23] = 0x00;
    assert(usb_msc_be16(&cbw[22]) == 256);
    puts("usb tests: ok");
    return 0;
}
