
#include <stdint.h>
#include "core_hal.h"
//#include "flash_mal.h"
#include "bootloader.h"


bool bootloader_requires_update()
{
    return false;
}

bool bootloader_update(const void*, unsigned)
{
    return false;
}

bool bootloader_update_if_needed()
{
    return false;
}

