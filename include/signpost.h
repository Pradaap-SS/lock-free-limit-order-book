#pragma once

// macOS Instruments signpost integration.
//
// Signposts mark named intervals that appear as colored regions in the
// Instruments timeline (Time Profiler, os_signpost instrument).
//
// Usage:
//   auto log = signpost::make_log("com.orderbook", "engine");
//   {
//       SIGNPOST_BEGIN(log, "add_order");
//       book.add_order(...);
//       SIGNPOST_END(log, "add_order");
//   }
//
// View in Instruments:
//   1. Open Instruments → File → New → os_signpost
//   2. Run your binary (or attach to process)
//   3. Named intervals appear as horizontal bars in the timeline

#if defined(__APPLE__)
#include <os/signpost.h>
#include <os/log.h>

namespace signpost {

inline os_log_t make_log(const char* subsystem, const char* category) {
    return os_log_create(subsystem, category);
}

} // namespace signpost

// Interval begin/end. name must be a string literal (os_signpost requirement).
#define SIGNPOST_BEGIN(log, name) \
    os_signpost_interval_begin(log, OS_SIGNPOST_ID_EXCLUSIVE, name)
#define SIGNPOST_END(log, name) \
    os_signpost_interval_end(log, OS_SIGNPOST_ID_EXCLUSIVE, name)

// Point event (instant marker on the timeline)
#define SIGNPOST_EVENT(log, name) \
    os_signpost_event_emit(log, OS_SIGNPOST_ID_EXCLUSIVE, name)

// RAII scope guard — name must be a string literal (os_signpost requirement).
// Usage: SIGNPOST_SCOPE(log, "add_order");
// Implemented as a macro so the literal propagates correctly.
#define SIGNPOST_SCOPE(log, name)                                        \
    struct _SG_##__LINE__ {                                              \
        os_log_t _l;                                                     \
        _SG_##__LINE__(os_log_t l) noexcept : _l(l) {                   \
            os_signpost_interval_begin(_l, OS_SIGNPOST_ID_EXCLUSIVE, name); \
        }                                                                \
        ~_SG_##__LINE__() noexcept {                                     \
            os_signpost_interval_end(_l, OS_SIGNPOST_ID_EXCLUSIVE, name); \
        }                                                                \
    } _sg_##__LINE__{log}

#else // Non-Apple: no-ops

namespace signpost {
struct FakeLog {};
inline FakeLog make_log(const char*, const char*) { return {}; }
} // namespace signpost

#define SIGNPOST_BEGIN(log, name)    (void)0
#define SIGNPOST_END(log, name)      (void)0
#define SIGNPOST_EVENT(log, name)    (void)0
#define SIGNPOST_SCOPE(log, name)    (void)0

#endif
