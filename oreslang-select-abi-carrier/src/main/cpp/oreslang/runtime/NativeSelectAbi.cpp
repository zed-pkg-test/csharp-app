#include "oreslang/runtime/NativeSelectAbi.h"
#include "oreslang/runtime/NativeSelectArbiter.hpp"

#include <array>
#include <new>
#include <optional>

using Arbiter = oreslang::runtime::NativeSelectArbiter<2>;
using Registration = Arbiter::Registration;

struct OresNativeSelect2 {
    Arbiter arbiter;
    std::array<std::optional<Registration>, 2> registrations;
};

namespace {
thread_local const OresNativeSelect2* active_commit = nullptr;

bool invalid(const OresNativeSelect2* select) noexcept {
    return select == nullptr || active_commit == select;
}

template <class Fn>
int guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (...) {
        return -1;
    }
}
}  // namespace

extern "C" OresNativeSelect2* ores_native_select2_open(void) {
    try {
        return new (std::nothrow) OresNativeSelect2();
    } catch (...) {
        return nullptr;
    }
}

extern "C" int ores_native_select2_register(
        OresNativeSelect2* select, uint32_t index) {
    if (invalid(select) || index >= 2) return -1;
    return guarded([&] {
        if (select->registrations[index]) return -1;
        select->registrations[index].emplace(
            select->arbiter.register_case(index));
        return 1;
    });
}

extern "C" int ores_native_select2_arm(
        OresNativeSelect2* select,
        uint32_t index,
        OresNativeSelectRevokeFn revoke,
        void* context) {
    if (invalid(select) || index >= 2 || revoke == nullptr) return -1;
    return guarded([&] {
        auto& registration = select->registrations[index];
        if (!registration) return -1;
        registration->arm([revoke, context] { revoke(context); });
        return 1;
    });
}

extern "C" int ores_native_select2_try_win(
        OresNativeSelect2* select, uint32_t index) {
    if (invalid(select) || index >= 2) return -1;
    return guarded([&] {
        auto& registration = select->registrations[index];
        if (!registration) return -1;
        return registration->try_win() ? 1 : 0;
    });
}

extern "C" int ores_native_select2_try_win_if(
        OresNativeSelect2* select,
        uint32_t index,
        OresNativeSelectCommitFn commit,
        void* context) {
    if (invalid(select) || index >= 2 || commit == nullptr) return -1;
    return guarded([&] {
        auto& registration = select->registrations[index];
        if (!registration) return -1;
        const bool won = registration->try_win_if([&] {
            struct Restore {
                const OresNativeSelect2*& slot;
                const OresNativeSelect2* prior;
                ~Restore() { slot = prior; }
            } restore{active_commit, active_commit};
            active_commit = select;
            return commit(context) != 0;
        });
        return won ? 1 : 0;
    });
}

extern "C" int ores_native_select2_cancel(OresNativeSelect2* select) {
    if (invalid(select)) return -1;
    return guarded([&] { return select->arbiter.cancel() ? 1 : 0; });
}

extern "C" int ores_native_select2_winner(
        const OresNativeSelect2* select) {
    if (invalid(select)) return -2;
    try {
        auto winner = select->arbiter.winner();
        return winner ? static_cast<int>(*winner) : -1;
    } catch (...) {
        return -2;
    }
}

extern "C" int ores_native_select2_is_cancelled(
        const OresNativeSelect2* select) {
    if (invalid(select)) return -1;
    try {
        return select->arbiter.is_cancelled() ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int ores_native_select2_has_cleanup_failure(
        const OresNativeSelect2* select) {
    if (invalid(select)) return -1;
    try {
        return select->arbiter.has_cleanup_failure() ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

extern "C" void ores_native_select2_destroy(OresNativeSelect2* select) {
    if (select == nullptr || active_commit == select) return;
    try {
        delete select;
    } catch (...) {
    }
}
