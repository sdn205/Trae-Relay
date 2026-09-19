// Count persisted usage records for the current local day.
#pragma once
namespace stats {
struct TodayUsage {
    long long requests = 0;
    long long tokens = 0;
};
TodayUsage usageToday();
long long requestCountToday();
} // namespace stats
