#include "zxlog/identity.h"

namespace zxlog::detail {
#if defined(__clang__)
__attribute__((no_sanitize("address")))
#endif
__attribute__((used, section("zxlog_identity"), aligned(1)))
extern const volatile std::uint8_t kLinkedBuildId[16] = {};
}  // namespace zxlog::detail
