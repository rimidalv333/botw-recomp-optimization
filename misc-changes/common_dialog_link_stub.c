/*
 * Host-link stub for PS5 libSceCommonDialog.
 * This function is never executed on the console.
 * It exists so the native converter records the system-library import.
 */
#include <stdint.h>

int32_t sceCommonDialogInitialize(void)
{
    return -1;
}
