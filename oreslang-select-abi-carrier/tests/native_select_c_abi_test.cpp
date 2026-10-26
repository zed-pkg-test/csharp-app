#include "oreslang/runtime/NativeSelectAbi.h"
#include <cassert>

struct Counts {
    OresNativeSelect2* select = nullptr;
    int revokes = 0;
    int commits = 0;
    int reentry = 99;
};

static void revoke_count(void* context) {
    auto* counts = static_cast<Counts*>(context);
    ++counts->revokes;
}

static void revoke_reentrant(void* context) {
    auto* counts = static_cast<Counts*>(context);
    ++counts->revokes;
    counts->reentry = ores_native_select2_cancel(counts->select);
}

static int commit_false(void* context) {
    auto* counts = static_cast<Counts*>(context);
    ++counts->commits;
    return 0;
}

static int commit_true_with_reentry_probe(void* context) {
    auto* counts = static_cast<Counts*>(context);
    ++counts->commits;
    counts->reentry = ores_native_select2_cancel(counts->select);
    return 1;
}

static void conditional_winner_revokes_loser() {
    OresNativeSelect2* select = ores_native_select2_open();
    assert(select != nullptr);
    Counts first{select};
    Counts second{select};

    assert(ores_native_select2_register(select, 0) == 1);
    assert(ores_native_select2_register(select, 1) == 1);
    assert(ores_native_select2_register(select, 1) == -1);
    assert(ores_native_select2_arm(select, 0, revoke_count, &first) == 1);
    assert(ores_native_select2_arm(select, 1, revoke_reentrant, &second) == 1);

    assert(ores_native_select2_try_win_if(
        select, 0, commit_false, &first) == 0);
    assert(first.commits == 1);
    assert(ores_native_select2_winner(select) == -1);

    assert(ores_native_select2_try_win_if(
        select, 1, commit_true_with_reentry_probe, &second) == 1);
    assert(second.commits == 1);
    assert(second.reentry == -1);
    assert(first.revokes == 1);
    assert(second.revokes == 0);
    assert(ores_native_select2_winner(select) == 1);
    assert(ores_native_select2_cancel(select) == 0);
    assert(ores_native_select2_has_cleanup_failure(select) == 0);
    ores_native_select2_destroy(select);
}

static void late_arm_is_revoked_after_loss() {
    OresNativeSelect2* select = ores_native_select2_open();
    assert(select != nullptr);
    Counts winner{select};
    Counts loser{select};
    assert(ores_native_select2_register(select, 0) == 1);
    assert(ores_native_select2_register(select, 1) == 1);
    assert(ores_native_select2_try_win(select, 0) == 1);
    assert(ores_native_select2_arm(select, 1, revoke_count, &loser) == 1);
    assert(loser.revokes == 1);
    assert(ores_native_select2_arm(select, 0, revoke_count, &winner) == 1);
    assert(winner.revokes == 0);
    ores_native_select2_destroy(select);
}

static void cancellation_and_destroy_revoke_pending() {
    {
        OresNativeSelect2* select = ores_native_select2_open();
        Counts a{select};
        Counts b{select};
        assert(ores_native_select2_register(select, 0) == 1);
        assert(ores_native_select2_register(select, 1) == 1);
        assert(ores_native_select2_arm(select, 0, revoke_count, &a) == 1);
        assert(ores_native_select2_arm(select, 1, revoke_count, &b) == 1);
        assert(ores_native_select2_cancel(select) == 1);
        assert(a.revokes == 1 && b.revokes == 1);
        assert(ores_native_select2_is_cancelled(select) == 1);
        assert(ores_native_select2_winner(select) == -1);
        ores_native_select2_destroy(select);
    }
    {
        OresNativeSelect2* select = ores_native_select2_open();
        Counts a{select};
        Counts b{select};
        assert(ores_native_select2_register(select, 0) == 1);
        assert(ores_native_select2_register(select, 1) == 1);
        assert(ores_native_select2_arm(select, 0, revoke_count, &a) == 1);
        assert(ores_native_select2_arm(select, 1, revoke_count, &b) == 1);
        ores_native_select2_destroy(select);
        assert(a.revokes == 1 && b.revokes == 1);
    }
}

static void invalid_inputs_fail_closed() {
    assert(ores_native_select2_register(nullptr, 0) == -1);
    assert(ores_native_select2_arm(nullptr, 0, revoke_count, nullptr) == -1);
    assert(ores_native_select2_try_win(nullptr, 0) == -1);
    assert(ores_native_select2_try_win_if(nullptr, 0, commit_false, nullptr) == -1);
    assert(ores_native_select2_cancel(nullptr) == -1);
    assert(ores_native_select2_winner(nullptr) == -2);

    OresNativeSelect2* select = ores_native_select2_open();
    assert(select != nullptr);
    assert(ores_native_select2_register(select, 2) == -1);
    assert(ores_native_select2_arm(select, 0, nullptr, nullptr) == -1);
    assert(ores_native_select2_try_win_if(select, 0, nullptr, nullptr) == -1);
    ores_native_select2_destroy(select);
}

int main() {
    conditional_winner_revokes_loser();
    late_arm_is_revoked_after_loss();
    cancellation_and_destroy_revoke_pending();
    invalid_inputs_fail_closed();
    return 0;
}
