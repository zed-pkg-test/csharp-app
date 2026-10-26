#include "oreslang/runtime/NativeI64ChannelAbi.h"
#include "oreslang/runtime/NativeSelectAbi.h"
#include <cassert>
#include <cstdint>

struct ReadCase {
    OresNativeSelect2* select = nullptr;
    OresNativeI64Channel1* channel = nullptr;
    uint32_t index = 0;
    int callbacks = 0;
    int wins = 0;
    int status = -1;
    int64_t value = 0;
};

static void readable(void* raw, const OresNativeI64ChannelPermit* permit) {
    auto* arm = static_cast<ReadCase*>(raw);
    ++arm->callbacks;
    const int won = ores_native_select2_try_commit_i64_read(
        arm->select, arm->index, arm->channel, permit,
        &arm->status, &arm->value);
    assert(won >= 0);
    if (won == 1) ++arm->wins;
}

static void revoke_subscription(void* raw) {
    auto* subscription =
        static_cast<OresNativeI64ChannelSubscription*>(raw);
    const int result =
        ores_native_i64_channel_subscription_close(subscription);
    assert(result == 0 || result == 1);
}

static void losing_read_does_not_consume() {
    auto* select = ores_native_select2_open();
    auto* first = ores_native_i64_channel1_open();
    auto* second = ores_native_i64_channel1_open();
    assert(select && first && second);

    int write_status = -1;
    assert(ores_native_i64_channel1_try_write(first, 11, &write_status) == 1);
    assert(write_status == ORES_NATIVE_I64_WRITE_WRITTEN);
    assert(ores_native_i64_channel1_try_write(second, 22, &write_status) == 1);
    assert(write_status == ORES_NATIVE_I64_WRITE_WRITTEN);

    assert(ores_native_select2_register(select, 0) == 1);
    assert(ores_native_select2_register(select, 1) == 1);

    ReadCase first_case{select, first, 0};
    ReadCase second_case{select, second, 1};
    OresNativeI64ChannelSubscription* first_wait = nullptr;
    OresNativeI64ChannelSubscription* second_wait = nullptr;

    assert(ores_native_i64_channel1_subscribe_readable(
        first, readable, &first_case, &first_wait) == 1);
    assert(first_wait != nullptr);
    assert(ores_native_select2_arm(
        select, 0, revoke_subscription, first_wait) == 1);

    assert(ores_native_i64_channel1_subscribe_readable(
        second, readable, &second_case, &second_wait) == 1);
    assert(second_wait != nullptr);
    assert(ores_native_select2_arm(
        select, 1, revoke_subscription, second_wait) == 1);

    assert(ores_native_select2_winner(select) == 0);
    assert(first_case.callbacks == 1 && first_case.wins == 1);
    assert(first_case.status == ORES_NATIVE_I64_READ_VALUE);
    assert(first_case.value == 11);
    assert(second_case.callbacks == 1 && second_case.wins == 0);
    assert(ores_native_i64_channel_subscription_active(first_wait) == 0);
    assert(ores_native_i64_channel_subscription_active(second_wait) == 0);

    int read_status = -1;
    int64_t value = 0;
    assert(ores_native_i64_channel1_try_read(
        second, &read_status, &value) == 1);
    assert(read_status == ORES_NATIVE_I64_READ_VALUE && value == 22);

    ores_native_i64_channel_subscription_destroy(first_wait);
    ores_native_i64_channel_subscription_destroy(second_wait);
    ores_native_i64_channel1_destroy(first);
    ores_native_i64_channel1_destroy(second);
    ores_native_select2_destroy(select);
}

struct WriteCase {
    OresNativeSelect2* select = nullptr;
    OresNativeI64Channel1* channel = nullptr;
    int callbacks = 0;
    int wins = 0;
    int status = -1;
};

static void writable(void* raw, const OresNativeI64ChannelPermit* permit) {
    auto* arm = static_cast<WriteCase*>(raw);
    ++arm->callbacks;
    const int won = ores_native_select2_try_commit_i64_write(
        arm->select, 0, arm->channel, permit, 8, &arm->status);
    assert(won >= 0);
    if (won == 1) ++arm->wins;
}

static void close_wakes_selected_writer_as_closed() {
    auto* select = ores_native_select2_open();
    auto* channel = ores_native_i64_channel1_open();
    assert(select && channel);
    int status = -1;
    assert(ores_native_i64_channel1_try_write(channel, 7, &status) == 1);
    assert(status == ORES_NATIVE_I64_WRITE_WRITTEN);
    assert(ores_native_select2_register(select, 0) == 1);

    WriteCase arm{select, channel};
    OresNativeI64ChannelSubscription* wait = nullptr;
    assert(ores_native_i64_channel1_subscribe_writable(
        channel, writable, &arm, &wait) == 1);
    assert(wait != nullptr);
    assert(arm.callbacks == 0);
    assert(ores_native_select2_arm(
        select, 0, revoke_subscription, wait) == 1);

    assert(ores_native_i64_channel1_close(channel) == 1);
    assert(ores_native_select2_winner(select) == 0);
    assert(arm.callbacks == 1 && arm.wins == 1);
    assert(arm.status == ORES_NATIVE_I64_WRITE_CLOSED);

    int read_status = -1;
    int64_t value = 0;
    assert(ores_native_i64_channel1_try_read(
        channel, &read_status, &value) == 1);
    assert(read_status == ORES_NATIVE_I64_READ_VALUE && value == 7);
    assert(ores_native_i64_channel1_try_read(
        channel, &read_status, &value) == 1);
    assert(read_status == ORES_NATIVE_I64_READ_EOF);

    ores_native_i64_channel_subscription_destroy(wait);
    ores_native_i64_channel1_destroy(channel);
    ores_native_select2_destroy(select);
}

struct BatchProbe {
    OresNativeI64Channel1* channel = nullptr;
    OresNativeI64ChannelBatch* batch = nullptr;
    int committed = -1;
    int status = -1;
    int64_t value = 0;
    int writer_notices = 0;
};

static void writer_notice(
        void* raw, const OresNativeI64ChannelPermit*) {
    auto* probe = static_cast<BatchProbe*>(raw);
    ++probe->writer_notices;
}

static void commit_read_to_batch(
        void* raw, const OresNativeI64ChannelPermit* permit) {
    auto* probe = static_cast<BatchProbe*>(raw);
    probe->committed = ores_native_i64_channel1_try_commit_read(
        probe->channel, permit, probe->batch,
        &probe->status, &probe->value);
}

static void pending_batch_cannot_be_discarded() {
    auto* channel = ores_native_i64_channel1_open();
    auto* batch = ores_native_i64_channel_batch_open();
    assert(channel && batch);
    int status = -1;
    assert(ores_native_i64_channel1_try_write(channel, 5, &status) == 1);
    assert(status == ORES_NATIVE_I64_WRITE_WRITTEN);

    BatchProbe probe{channel, batch};
    OresNativeI64ChannelSubscription* writer = nullptr;
    OresNativeI64ChannelSubscription* reader = nullptr;
    assert(ores_native_i64_channel1_subscribe_writable(
        channel, writer_notice, &probe, &writer) == 1);
    assert(probe.writer_notices == 0);
    assert(ores_native_i64_channel1_subscribe_readable(
        channel, commit_read_to_batch, &probe, &reader) == 1);

    assert(probe.committed == 1);
    assert(probe.status == ORES_NATIVE_I64_READ_VALUE);
    assert(probe.value == 5);
    assert(probe.writer_notices == 0);
    assert(ores_native_i64_channel_batch_destroy(batch) == 0);
    assert(ores_native_i64_channel_batch_dispatch(batch) == 1);
    assert(probe.writer_notices == 1);
    assert(ores_native_i64_channel_batch_destroy(batch) == 1);

    ores_native_i64_channel_subscription_destroy(reader);
    ores_native_i64_channel_subscription_destroy(writer);
    ores_native_i64_channel1_destroy(channel);
}

static void direct_close_and_eof() {
    auto* channel = ores_native_i64_channel1_open();
    assert(channel);
    int status = -1;
    int64_t value = 0;

    assert(ores_native_i64_channel1_try_read(
        channel, &status, &value) == 1);
    assert(status == ORES_NATIVE_I64_READ_WOULD_BLOCK);
    assert(ores_native_i64_channel1_try_write(channel, 3, &status) == 1);
    assert(status == ORES_NATIVE_I64_WRITE_WRITTEN);
    assert(ores_native_i64_channel1_close(channel) == 1);
    assert(ores_native_i64_channel1_close(channel) == 0);
    assert(ores_native_i64_channel1_try_write(channel, 4, &status) == 1);
    assert(status == ORES_NATIVE_I64_WRITE_CLOSED);
    assert(ores_native_i64_channel1_try_read(
        channel, &status, &value) == 1);
    assert(status == ORES_NATIVE_I64_READ_VALUE && value == 3);
    assert(ores_native_i64_channel1_try_read(
        channel, &status, &value) == 1);
    assert(status == ORES_NATIVE_I64_READ_EOF);
    ores_native_i64_channel1_destroy(channel);
}

int main() {
    direct_close_and_eof();
    losing_read_does_not_consume();
    close_wakes_selected_writer_as_closed();
    pending_batch_cannot_be_discarded();
    return 0;
}
