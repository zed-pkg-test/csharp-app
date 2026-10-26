#pragma once
#include <stdint.h>
#include "oreslang/runtime/NativeSelectAbi.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct OresNativeI64Channel1 OresNativeI64Channel1;
typedef struct OresNativeI64ChannelSubscription OresNativeI64ChannelSubscription;
typedef struct OresNativeI64ChannelPermit OresNativeI64ChannelPermit;
typedef struct OresNativeI64ChannelBatch OresNativeI64ChannelBatch;

enum {
    ORES_NATIVE_I64_READ_WOULD_BLOCK = 0,
    ORES_NATIVE_I64_READ_VALUE = 1,
    ORES_NATIVE_I64_READ_EOF = 2
};
enum {
    ORES_NATIVE_I64_WRITE_WOULD_BLOCK = 0,
    ORES_NATIVE_I64_WRITE_WRITTEN = 1,
    ORES_NATIVE_I64_WRITE_CLOSED = 2
};

typedef void (*OresNativeI64ChannelNotifyFn)(
    void* context,
    const OresNativeI64ChannelPermit* permit);

OresNativeI64Channel1* ores_native_i64_channel1_open(void);
int ores_native_i64_channel1_close(OresNativeI64Channel1*);
int ores_native_i64_channel1_is_closed(const OresNativeI64Channel1*);
int ores_native_i64_channel1_try_write(
    OresNativeI64Channel1*, int64_t value, int* status);
int ores_native_i64_channel1_try_read(
    OresNativeI64Channel1*, int* status, int64_t* value);
void ores_native_i64_channel1_destroy(OresNativeI64Channel1*);

OresNativeI64ChannelBatch* ores_native_i64_channel_batch_open(void);
int ores_native_i64_channel_batch_dispatch(OresNativeI64ChannelBatch*);
int ores_native_i64_channel_batch_destroy(OresNativeI64ChannelBatch*);

int ores_native_i64_channel1_subscribe_readable(
    OresNativeI64Channel1*,
    OresNativeI64ChannelNotifyFn,
    void* context,
    OresNativeI64ChannelSubscription** out_subscription);
int ores_native_i64_channel1_subscribe_writable(
    OresNativeI64Channel1*,
    OresNativeI64ChannelNotifyFn,
    void* context,
    OresNativeI64ChannelSubscription** out_subscription);

int ores_native_i64_channel_subscription_active(
    const OresNativeI64ChannelSubscription*);
int ores_native_i64_channel_subscription_close(
    OresNativeI64ChannelSubscription*);
void ores_native_i64_channel_subscription_destroy(
    OresNativeI64ChannelSubscription*);

int ores_native_i64_channel1_try_commit_read(
    OresNativeI64Channel1*,
    const OresNativeI64ChannelPermit*,
    OresNativeI64ChannelBatch*,
    int* status,
    int64_t* value);
int ores_native_i64_channel1_try_commit_write(
    OresNativeI64Channel1*,
    const OresNativeI64ChannelPermit*,
    int64_t value,
    OresNativeI64ChannelBatch*,
    int* status);

int ores_native_select2_try_commit_i64_read(
    OresNativeSelect2*,
    uint32_t index,
    OresNativeI64Channel1*,
    const OresNativeI64ChannelPermit*,
    int* status,
    int64_t* value);
int ores_native_select2_try_commit_i64_write(
    OresNativeSelect2*,
    uint32_t index,
    OresNativeI64Channel1*,
    const OresNativeI64ChannelPermit*,
    int64_t value,
    int* status);

int ores_native_select2_read_i64_ready_now(
    OresNativeSelect2*,
    uint32_t index,
    OresNativeI64Channel1*,
    int* status,
    int64_t* value);

#ifdef __cplusplus
}
#endif
