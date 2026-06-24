#include "SecurityCheck.h"

extern "C" {
    __attribute__((visibility("default"), used, section(".data"))) 
    volatile const char g_embedded_signature[SIG_PLACEHOLDER_SIZE] = 
        SIG_MAGIC_HEADER "0000000000000000000000000000000000000000000000000000000000000000";
}