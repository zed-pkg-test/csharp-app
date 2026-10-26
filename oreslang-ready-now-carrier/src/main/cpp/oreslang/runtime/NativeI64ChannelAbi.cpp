#include "oreslang/runtime/NativeI64ChannelAbi.h"
#include "oreslang/runtime/NativeSelectableI64Channel.hpp"

#include <memory>
#include <new>
#include <optional>

using Channel = oreslang::runtime::NativeSelectableI64Channel<1, 64>;
using Subscription = Channel::Subscription;
using Batch = Channel::NotificationBatch;

struct OresNativeI64Channel1 {
    Channel channel;
};

struct OresNativeI64ChannelSubscription {
    std::optional<Subscription> subscription;
};

struct OresNativeI64ChannelPermit {
    OresNativeI64Channel1* owner = nullptr;
    const Subscription* subscription = nullptr;
};

struct OresNativeI64ChannelBatch {
    Batch batch;
};

namespace {

template <class Fn>
int guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (...) {
        return -1;
    }
}

int read_status(Channel::ReadStatus status) noexcept {
    switch (status) {
        case Channel::ReadStatus::WouldBlock:
            return ORES_NATIVE_I64_READ_WOULD_BLOCK;
        case Channel::ReadStatus::Value:
            return ORES_NATIVE_I64_READ_VALUE;
        case Channel::ReadStatus::Eof:
            return ORES_NATIVE_I64_READ_EOF;
    }
    return ORES_NATIVE_I64_READ_WOULD_BLOCK;
}

int write_status(Channel::WriteStatus status) noexcept {
    switch (status) {
        case Channel::WriteStatus::WouldBlock:
            return ORES_NATIVE_I64_WRITE_WOULD_BLOCK;
        case Channel::WriteStatus::Written:
            return ORES_NATIVE_I64_WRITE_WRITTEN;
        case Channel::WriteStatus::Closed:
            return ORES_NATIVE_I64_WRITE_CLOSED;
    }
    return ORES_NATIVE_I64_WRITE_WOULD_BLOCK;
}

int subscribe(
        OresNativeI64Channel1* channel,
        OresNativeI64ChannelNotifyFn notify,
        void* context,
        OresNativeI64ChannelSubscription** out_subscription,
        bool readable) noexcept {
    if (!channel || !notify || !out_subscription) return -1;
    *out_subscription = nullptr;

    return guarded([&] {
        auto holder =
            std::make_unique<OresNativeI64ChannelSubscription>();

        auto callback = [channel, notify, context](
                const Subscription& subscription) {
            const OresNativeI64ChannelPermit permit{
                channel, &subscription};
            notify(context, &permit);
        };

        Subscription token = readable
            ? channel->channel.subscribe_readable(callback)
            : channel->channel.subscribe_writable(callback);

        holder->subscription.emplace(std::move(token));
        *out_subscription = holder.release();
        return 1;
    });
}

}  // namespace

extern "C" OresNativeI64Channel1* ores_native_i64_channel1_open(void) {
    try {
        return new (std::nothrow) OresNativeI64Channel1();
    } catch (...) {
        return nullptr;
    }
}

extern "C" int ores_native_i64_channel1_close(
        OresNativeI64Channel1* channel) {
    if (!channel) return -1;
    return guarded([&] { return channel->channel.close() ? 1 : 0; });
}

extern "C" int ores_native_i64_channel1_is_closed(
        const OresNativeI64Channel1* channel) {
    if (!channel) return -1;
    return guarded([&] { return channel->channel.closed() ? 1 : 0; });
}

extern "C" int ores_native_i64_channel1_try_write(
        OresNativeI64Channel1* channel,
        int64_t value,
        int* status) {
    if (!channel || !status) return -1;
    return guarded([&] {
        if (channel->channel.closed()) {
            *status = ORES_NATIVE_I64_WRITE_CLOSED;
            return 1;
        }
        if (channel->channel.try_write(value)) {
            *status = ORES_NATIVE_I64_WRITE_WRITTEN;
            return 1;
        }
        *status = channel->channel.closed()
            ? ORES_NATIVE_I64_WRITE_CLOSED
            : ORES_NATIVE_I64_WRITE_WOULD_BLOCK;
        return 1;
    });
}

extern "C" int ores_native_i64_channel1_try_read(
        OresNativeI64Channel1* channel,
        int* status,
        int64_t* value) {
    if (!channel || !status || !value) return -1;
    return guarded([&] {
        const auto result = channel->channel.try_read_result();
        *status = read_status(result.status);
        *value = result.value;
        return 1;
    });
}

extern "C" void ores_native_i64_channel1_destroy(
        OresNativeI64Channel1* channel) {
    try {
        delete channel;
    } catch (...) {
    }
}

extern "C" OresNativeI64ChannelBatch*
ores_native_i64_channel_batch_open(void) {
    try {
        return new (std::nothrow) OresNativeI64ChannelBatch();
    } catch (...) {
        return nullptr;
    }
}

extern "C" int ores_native_i64_channel_batch_dispatch(
        OresNativeI64ChannelBatch* batch) {
    if (!batch) return -1;
    batch->batch.dispatch();
    return 1;
}

extern "C" int ores_native_i64_channel_batch_destroy(
        OresNativeI64ChannelBatch* batch) {
    if (!batch) return -1;
    if (!batch->batch.empty()) return 0;
    try {
        delete batch;
        return 1;
    } catch (...) {
        return -1;
    }
}

extern "C" int ores_native_i64_channel1_subscribe_readable(
        OresNativeI64Channel1* channel,
        OresNativeI64ChannelNotifyFn notify,
        void* context,
        OresNativeI64ChannelSubscription** out_subscription) {
    return subscribe(
        channel, notify, context, out_subscription, true);
}

extern "C" int ores_native_i64_channel1_subscribe_writable(
        OresNativeI64Channel1* channel,
        OresNativeI64ChannelNotifyFn notify,
        void* context,
        OresNativeI64ChannelSubscription** out_subscription) {
    return subscribe(
        channel, notify, context, out_subscription, false);
}

extern "C" int ores_native_i64_channel_subscription_active(
        const OresNativeI64ChannelSubscription* subscription) {
    if (!subscription || !subscription->subscription) return -1;
    return guarded([&] {
        return subscription->subscription->active() ? 1 : 0;
    });
}

extern "C" int ores_native_i64_channel_subscription_close(
        OresNativeI64ChannelSubscription* subscription) {
    if (!subscription || !subscription->subscription) return -1;
    return guarded([&] {
        return subscription->subscription->close() ? 1 : 0;
    });
}

extern "C" void ores_native_i64_channel_subscription_destroy(
        OresNativeI64ChannelSubscription* subscription) {
    if (!subscription) return;
    try {
        if (subscription->subscription)
            (void)subscription->subscription->close();
        delete subscription;
    } catch (...) {
    }
}

extern "C" int ores_native_i64_channel1_try_commit_read(
        OresNativeI64Channel1* channel,
        const OresNativeI64ChannelPermit* permit,
        OresNativeI64ChannelBatch* batch,
        int* status,
        int64_t* value) {
    if (!channel || !permit || !permit->subscription ||
        permit->owner != channel || !batch || !status || !value)
        return -1;

    return guarded([&] {
        Channel::ReadResult result;
        const bool committed = channel->channel.try_commit_read(
            *permit->subscription, result, batch->batch);
        *status = read_status(result.status);
        *value = result.value;
        return committed ? 1 : 0;
    });
}

extern "C" int ores_native_i64_channel1_try_commit_write(
        OresNativeI64Channel1* channel,
        const OresNativeI64ChannelPermit* permit,
        int64_t value,
        OresNativeI64ChannelBatch* batch,
        int* status) {
    if (!channel || !permit || !permit->subscription ||
        permit->owner != channel || !batch || !status)
        return -1;

    return guarded([&] {
        auto result = Channel::WriteStatus::WouldBlock;
        const bool committed = channel->channel.try_commit_write(
            *permit->subscription, value, result, batch->batch);
        *status = write_status(result);
        return committed ? 1 : 0;
    });
}


namespace {
struct SelectReadCommitContext {
    OresNativeI64Channel1* channel;
    const OresNativeI64ChannelPermit* permit;
    OresNativeI64ChannelBatch* batch;
    int* status;
    int64_t* value;
    bool failed = false;
};

int select_read_commit(void* raw) noexcept {
    auto* context = static_cast<SelectReadCommitContext*>(raw);
    const int result = ores_native_i64_channel1_try_commit_read(
        context->channel, context->permit, context->batch,
        context->status, context->value);
    if (result < 0) {
        context->failed = true;
        return 0;
    }
    return result == 1 ? 1 : 0;
}

struct SelectWriteCommitContext {
    OresNativeI64Channel1* channel;
    const OresNativeI64ChannelPermit* permit;
    int64_t value;
    OresNativeI64ChannelBatch* batch;
    int* status;
    bool failed = false;
};

int select_write_commit(void* raw) noexcept {
    auto* context = static_cast<SelectWriteCommitContext*>(raw);
    const int result = ores_native_i64_channel1_try_commit_write(
        context->channel, context->permit, context->value,
        context->batch, context->status);
    if (result < 0) {
        context->failed = true;
        return 0;
    }
    return result == 1 ? 1 : 0;
}
}  // namespace

extern "C" int ores_native_select2_try_commit_i64_read(
        OresNativeSelect2* select,
        uint32_t index,
        OresNativeI64Channel1* channel,
        const OresNativeI64ChannelPermit* permit,
        int* status,
        int64_t* value) {
    if (!select || !channel || !permit || !status || !value)
        return -1;
    *status = ORES_NATIVE_I64_READ_WOULD_BLOCK;
    *value = 0;

    OresNativeI64ChannelBatch batch;
    SelectReadCommitContext context{
        channel, permit, &batch, status, value, false};
    const int won = ores_native_select2_try_win_if(
        select, index, select_read_commit, &context);
    batch.batch.dispatch();
    if (context.failed || won < 0) return -1;
    return won;
}

extern "C" int ores_native_select2_try_commit_i64_write(
        OresNativeSelect2* select,
        uint32_t index,
        OresNativeI64Channel1* channel,
        const OresNativeI64ChannelPermit* permit,
        int64_t value,
        int* status) {
    if (!select || !channel || !permit || !status)
        return -1;
    *status = ORES_NATIVE_I64_WRITE_WOULD_BLOCK;

    OresNativeI64ChannelBatch batch;
    SelectWriteCommitContext context{
        channel, permit, value, &batch, status, false};
    const int won = ores_native_select2_try_win_if(
        select, index, select_write_commit, &context);
    batch.batch.dispatch();
    if (context.failed || won < 0) return -1;
    return won;
}


namespace {
struct ReadyNowReadContext {
    OresNativeSelect2* select = nullptr;
    uint32_t index = 0;
    OresNativeI64Channel1* channel = nullptr;
    int* status = nullptr;
    int64_t* value = nullptr;
    int result = 0;
};

void ready_now_read(
        void* raw,
        const OresNativeI64ChannelPermit* permit) noexcept {
    auto* context = static_cast<ReadyNowReadContext*>(raw);
    context->result = ores_native_select2_try_commit_i64_read(
        context->select,
        context->index,
        context->channel,
        permit,
        context->status,
        context->value);
}
}  // namespace

extern "C" int ores_native_select2_read_i64_ready_now(
        OresNativeSelect2* select,
        uint32_t index,
        OresNativeI64Channel1* channel,
        int* status,
        int64_t* value) {
    if (!select || !channel || !status || !value || index >= 2)
        return -1;

    *status = ORES_NATIVE_I64_READ_WOULD_BLOCK;
    *value = 0;
    ReadyNowReadContext context{
        select, index, channel, status, value, 0};
    OresNativeI64ChannelSubscription* subscription = nullptr;
    const int subscribed =
        ores_native_i64_channel1_subscribe_readable(
            channel,
            ready_now_read,
            &context,
            &subscription);
    if (subscribed != 1 || !subscription)
        return -1;

    const int closed =
        ores_native_i64_channel_subscription_close(subscription);
    ores_native_i64_channel_subscription_destroy(subscription);
    if (closed < 0 || context.result < 0)
        return -1;
    return context.result;
}
