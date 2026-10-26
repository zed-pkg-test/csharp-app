%ReadCtx = type { ptr, ptr, i32, i32, i64 }

declare ptr @ores_native_select2_open()
declare i32 @ores_native_select2_register(ptr, i32)
declare i32 @ores_native_select2_arm(ptr, i32, ptr, ptr)
declare i32 @ores_native_select2_winner(ptr)
declare void @ores_native_select2_destroy(ptr)

declare ptr @ores_native_i64_channel1_open()
declare i32 @ores_native_i64_channel1_try_write(ptr, i64, ptr)
declare void @ores_native_i64_channel1_destroy(ptr)
declare i32 @ores_native_i64_channel1_subscribe_readable(
    ptr, ptr, ptr, ptr)
declare i32 @ores_native_i64_channel_subscription_active(ptr)
declare i32 @ores_native_i64_channel_subscription_close(ptr)
declare void @ores_native_i64_channel_subscription_destroy(ptr)
declare i32 @ores_native_select2_try_commit_i64_read(
    ptr, i32, ptr, ptr, ptr, ptr)

define void @select_read_ready(ptr %raw, ptr %permit) {
entry:
  %select.slot = getelementptr inbounds %ReadCtx, ptr %raw, i32 0, i32 0
  %channel.slot = getelementptr inbounds %ReadCtx, ptr %raw, i32 0, i32 1
  %index.slot = getelementptr inbounds %ReadCtx, ptr %raw, i32 0, i32 2
  %status.slot = getelementptr inbounds %ReadCtx, ptr %raw, i32 0, i32 3
  %value.slot = getelementptr inbounds %ReadCtx, ptr %raw, i32 0, i32 4
  %select = load ptr, ptr %select.slot
  %channel = load ptr, ptr %channel.slot
  %index = load i32, ptr %index.slot
  %won = call i32 @ores_native_select2_try_commit_i64_read(
      ptr %select, i32 %index, ptr %channel, ptr %permit,
      ptr %status.slot, ptr %value.slot)
  ret void
}

define void @revoke_read(ptr %subscription) {
entry:
  %closed = call i32 @ores_native_i64_channel_subscription_close(
      ptr %subscription)
  ret void
}

define i32 @main() {
entry:
  %channel = call ptr @ores_native_i64_channel1_open()
  %select = call ptr @ores_native_select2_open()
  %write.status = alloca i32, align 4
  %write.rc = call i32 @ores_native_i64_channel1_try_write(
      ptr %channel, i64 42, ptr %write.status)
  %registered = call i32 @ores_native_select2_register(
      ptr %select, i32 0)

  %ctx = alloca %ReadCtx, align 8
  %ctx.select = getelementptr inbounds %ReadCtx, ptr %ctx, i32 0, i32 0
  %ctx.channel = getelementptr inbounds %ReadCtx, ptr %ctx, i32 0, i32 1
  %ctx.index = getelementptr inbounds %ReadCtx, ptr %ctx, i32 0, i32 2
  %ctx.status = getelementptr inbounds %ReadCtx, ptr %ctx, i32 0, i32 3
  %ctx.value = getelementptr inbounds %ReadCtx, ptr %ctx, i32 0, i32 4
  store ptr %select, ptr %ctx.select
  store ptr %channel, ptr %ctx.channel
  store i32 0, ptr %ctx.index
  store i32 0, ptr %ctx.status
  store i64 0, ptr %ctx.value

  %subscription.slot = alloca ptr, align 8
  store ptr null, ptr %subscription.slot
  %subscribed = call i32 @ores_native_i64_channel1_subscribe_readable(
      ptr %channel, ptr @select_read_ready, ptr %ctx,
      ptr %subscription.slot)
  %subscription = load ptr, ptr %subscription.slot
  %armed = call i32 @ores_native_select2_arm(
      ptr %select, i32 0, ptr @revoke_read, ptr %subscription)

  %winner = call i32 @ores_native_select2_winner(ptr %select)
  %read.status = load i32, ptr %ctx.status
  %value = load i64, ptr %ctx.value
  %write.result = load i32, ptr %write.status
  %active = call i32 @ores_native_i64_channel_subscription_active(
      ptr %subscription)

  %ok.write.call = icmp eq i32 %write.rc, 1
  %ok.write.status = icmp eq i32 %write.result, 1
  %ok.register = icmp eq i32 %registered, 1
  %ok.subscribe = icmp eq i32 %subscribed, 1
  %ok.arm = icmp eq i32 %armed, 1
  %ok.winner = icmp eq i32 %winner, 0
  %ok.read.status = icmp eq i32 %read.status, 1
  %ok.value = icmp eq i64 %value, 42
  %ok.inactive = icmp eq i32 %active, 0

  %ok1 = and i1 %ok.write.call, %ok.write.status
  %ok2 = and i1 %ok1, %ok.register
  %ok3 = and i1 %ok2, %ok.subscribe
  %ok4 = and i1 %ok3, %ok.arm
  %ok5 = and i1 %ok4, %ok.winner
  %ok6 = and i1 %ok5, %ok.read.status
  %ok7 = and i1 %ok6, %ok.value
  %ok = and i1 %ok7, %ok.inactive

  call void @ores_native_i64_channel_subscription_destroy(ptr %subscription)
  call void @ores_native_i64_channel1_destroy(ptr %channel)
  call void @ores_native_select2_destroy(ptr %select)

  %exit = select i1 %ok, i32 0, i32 1
  ret i32 %exit
}
