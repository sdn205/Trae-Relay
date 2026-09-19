// Request counts share the usage record data source.
#include "common/Stats.h"
#include "accounts/AccountPool.h"

namespace stats {
TodayUsage usageToday() {
    TodayUsage result;
    result.requests = AccountPool::instance().usageCountToday(&result.tokens);
    return result;
}
long long requestCountToday() {
    return AccountPool::instance().usageCountToday();
}
} // namespace stats
