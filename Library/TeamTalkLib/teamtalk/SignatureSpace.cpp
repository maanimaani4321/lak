#include "SecurityCheck.h"

extern "C" {
    __attribute__((visibility("default"), used, section(".data"))) 
    volatile const char WAVE_FORMAT_EXTENSIBLE_METADATA_BLOCK[SIG_PLACEHOLDER_SIZE] = 
        SIG_MAGIC_HEADER "0000000000000000000000000000000000000000000000000000000000000000";
}