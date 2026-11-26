// ncview++ — CF-convention time decoding (all CF calendars).
#pragma once
#include <cstdint>
#include <string>

namespace ncvpp {

enum class Calendar { Standard, ProlepticGregorian, Julian, NoLeap, AllLeap, Day360 };

struct DateTime {
    int64_t year = 0;
    int month = 1, day = 1, hour = 0, minute = 0;
    double second = 0;
};

class CFTime {
public:
    // Parses CF units like "days since 1850-01-01 00:00:00". Returns false if not a time axis.
    bool parse(const std::string& units, const std::string& calendar);
    bool valid() const { return ok_; }
    DateTime decode(double value) const;
    std::string format(double value) const;   // "YYYY-MM-DD HH:MM[:SS]"
    Calendar calendar() const { return cal_; }
    // Converts a value in these units to the units of `to` (both valid, same calendar).
    double convertTo(double value, const CFTime& to) const {
        return ((double)(refDay_ - to.refDay_) * 86400.0 + (refSecOfDay_ - to.refSecOfDay_) +
                value * unitSeconds_) / to.unitSeconds_;
    }

    // Exposed for tests.
    static int64_t dayNumber(Calendar c, int64_t y, int m, int d);
    static void fromDayNumber(Calendar c, int64_t n, int64_t& y, int& m, int& d);

private:
    bool ok_ = false;
    Calendar cal_ = Calendar::Standard;
    double unitSeconds_ = 86400.0;
    int64_t refDay_ = 0;       // day number of reference date in this calendar
    double refSecOfDay_ = 0;   // seconds past midnight of reference
};

} // namespace ncvpp
