#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeSelect2 OresNativeSelect2;
typedef int (*OresNativeSelectCommitFn)(void*);
typedef void (*OresNativeSelectRevokeFn)(void*);
OresNativeSelect2* ores_native_select2_open(void);
int ores_native_select2_register(OresNativeSelect2*, uint32_t);
int ores_native_select2_arm(OresNativeSelect2*, uint32_t,
    OresNativeSelectRevokeFn, void*);
int ores_native_select2_try_win(OresNativeSelect2*, uint32_t);
int ores_native_select2_try_win_if(OresNativeSelect2*, uint32_t,
    OresNativeSelectCommitFn, void*);
int ores_native_select2_cancel(OresNativeSelect2*);
int ores_native_select2_winner(const OresNativeSelect2*);
int ores_native_select2_is_cancelled(const OresNativeSelect2*);
int ores_native_select2_has_cleanup_failure(const OresNativeSelect2*);
void ores_native_select2_destroy(OresNativeSelect2*);
#ifdef __cplusplus
}
#endif
