#include "cftime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ncvpp {

static int64_t floordiv(int64_t a, int64_t b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

static const int kCum365[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
static const int kCum366[13] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};

// Julian Day Number, Gregorian or Julian rules (valid for years > -4800).
static int64_t jdnGregorian(int64_t Y, int M, int D) {
    int64_t a = (14 - M) / 12, y = Y + 4800 - a, m = M + 12 * a - 3;
    return D + (153 * m + 2) / 5 + 365 * y + floordiv(y, 4) - floordiv(y, 100) + floordiv(y, 400) - 32045;
}
static int64_t jdnJulian(int64_t Y, int M, int D) {
    int64_t a = (14 - M) / 12, y = Y + 4800 - a, m = M + 12 * a - 3;
    return D + (153 * m + 2) / 5 + 365 * y + floordiv(y, 4) - 32083;
}
static void fromJdn(int64_t J, bool gregorian, int64_t& Y, int& M, int& D) {
    int64_t f = J + 1401;
    if (gregorian) f += (((4 * J + 274277) / 146097) * 3) / 4 - 38;
    int64_t e = 4 * f + 3, g = (e % 1461) / 4, h = 5 * g + 2;
    D = int((h % 153) / 5 + 1);
    M = int(((h / 153 + 2) % 12) + 1);
    Y = e / 1461 - 4716 + (12 + 2 - M) / 12;
}

int64_t CFTime::dayNumber(Calendar c, int64_t y, int m, int d) {
    switch (c) {
    case Calendar::Standard:
        if (y > 1582 || (y == 1582 && (m > 10 || (m == 10 && d >= 15)))) return jdnGregorian(y, m, d);
        return jdnJulian(y, m, d);
    case Calendar::ProlepticGregorian: return jdnGregorian(y, m, d);
    case Calendar::Julian: return jdnJulian(y, m, d);
    case Calendar::NoLeap: return y * 365 + kCum365[m - 1] + d - 1;
    case Calendar::AllLeap: return y * 366 + kCum366[m - 1] + d - 1;
    case Calendar::Day360: return y * 360 + (m - 1) * 30 + d - 1;
    }
    return 0;
}

void CFTime::fromDayNumber(Calendar c, int64_t n, int64_t& y, int& m, int& d) {
    auto fixed = [&](int len, const int* cum) {
        y = floordiv(n, len);
        int doy = int(n - y * len);
        m = 1;
        while (m < 12 && doy >= cum[m]) ++m;
        d = doy - cum[m - 1] + 1;
    };
    switch (c) {
    case Calendar::Standard: fromJdn(n, n >= 2299161, y, m, d); break;
    case Calendar::ProlepticGregorian: fromJdn(n, true, y, m, d); break;
    case Calendar::Julian: fromJdn(n, false, y, m, d); break;
    case Calendar::NoLeap: fixed(365, kCum365); break;
    case Calendar::AllLeap: fixed(366, kCum366); break;
    case Calendar::Day360:
        y = floordiv(n, 360);
        m = int((n - y * 360) / 30) + 1;
        d = int((n - y * 360) % 30) + 1;
        break;
    }
}

static std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool CFTime::parse(const std::string& unitsIn, const std::string& calendarIn) {
    ok_ = false;
    std::string units = lower(trim(unitsIn));
    size_t p = units.find(" since ");
    if (p == std::string::npos) return false;
    std::string unit = trim(units.substr(0, p));
    std::string ref = trim(units.substr(p + 7));

    std::string cal = lower(trim(calendarIn));
    if (cal.empty() || cal == "standard" || cal == "gregorian") cal_ = Calendar::Standard;
    else if (cal == "proleptic_gregorian") cal_ = Calendar::ProlepticGregorian;
    else if (cal == "julian") cal_ = Calendar::Julian;
    else if (cal == "noleap" || cal == "365_day") cal_ = Calendar::NoLeap;
    else if (cal == "all_leap" || cal == "366_day") cal_ = Calendar::AllLeap;
    else if (cal == "360_day") cal_ = Calendar::Day360;
    else cal_ = Calendar::Standard;

    const double tropicalYearDays = 365.242198781;
    double yearDays = cal_ == Calendar::Day360 ? 360.0 : cal_ == Calendar::NoLeap ? 365.0
                    : cal_ == Calendar::AllLeap ? 366.0 : tropicalYearDays;
    if (!unit.empty() && unit.back() == 's' && unit.size() > 1) unit.pop_back();
    if (unit == "second" || unit == "sec" || unit == "s") unitSeconds_ = 1;
    else if (unit == "minute" || unit == "min") unitSeconds_ = 60;
    else if (unit == "hour" || unit == "hr" || unit == "h") unitSeconds_ = 3600;
    else if (unit == "day" || unit == "d") unitSeconds_ = 86400;
    else if (unit == "week") unitSeconds_ = 7 * 86400;
    else if (unit == "month") unitSeconds_ = yearDays / 12.0 * 86400;
    else if (unit == "year" || unit == "yr") unitSeconds_ = yearDays * 86400;
    else if (unit == "millisecond" || unit == "msec" || unit == "ms") unitSeconds_ = 1e-3;
    else return false;

    // Reference date: [-]Y-M-D[(T| )h[:m[:s[.f]]]][Z|±hh[:mm]]
    const char* c = ref.c_str();
    char* end = nullptr;
    long long Y = std::strtoll(c, &end, 10);
    if (end == c || *end != '-') return false;
    long Mo = std::strtol(end + 1, &end, 10);
    if (*end != '-') return false;
    long D = std::strtol(end + 1, &end, 10);
    double h = 0, mi = 0, s = 0;
    while (*end == ' ' || *end == 'T' || *end == 't') ++end;
    if (std::isdigit((unsigned char)*end)) {
        h = std::strtod(end, &end);
        if (*end == ':') { mi = std::strtod(end + 1, &end);
            if (*end == ':') s = std::strtod(end + 1, &end); }
    }
    if (Mo < 1 || Mo > 12 || D < 1 || D > 31) return false;
    refDay_ = dayNumber(cal_, Y, (int)Mo, (int)D);
    refSecOfDay_ = h * 3600 + mi * 60 + s;
    ok_ = true;
    return true;
}

DateTime CFTime::decode(double value) const {
    DateTime dt;
    double secs = refSecOfDay_ + value * unitSeconds_;
    double dayf = std::floor(secs / 86400.0);
    double sod = secs - dayf * 86400.0;
    sod = std::round(sod * 1000.0) / 1000.0;   // ms resolution
    if (sod >= 86400.0) { sod -= 86400.0; dayf += 1; }
    int64_t y; int m, d;
    fromDayNumber(cal_, refDay_ + (int64_t)dayf, y, m, d);
    dt.year = y; dt.month = m; dt.day = d;
    dt.hour = int(sod / 3600);
    dt.minute = int((sod - dt.hour * 3600) / 60);
    dt.second = sod - dt.hour * 3600 - dt.minute * 60;
    return dt;
}

std::string CFTime::format(double value) const {
    if (!ok_ || !std::isfinite(value)) return "?";
    DateTime t = decode(value);
    char buf[64];
    int sec = (int)std::lround(t.second);
    if (sec == 0)
        std::snprintf(buf, sizeof buf, "%04lld-%02d-%02d %02d:%02d", (long long)t.year, t.month, t.day, t.hour, t.minute);
    else
        std::snprintf(buf, sizeof buf, "%04lld-%02d-%02d %02d:%02d:%02d", (long long)t.year, t.month, t.day, t.hour, t.minute, sec);
    return buf;
}

} // namespace ncvpp
