#include "SshUmdLoader/Domain.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace sshumdloader {
namespace {

std::tm ToTm(const MarketDataDate& date) {
  std::tm value = {};
  value.tm_year = date.year - 1900;
  value.tm_mon = date.month - 1;
  value.tm_mday = date.day;
  value.tm_hour = 12;
  value.tm_isdst = -1;
  return value;
}

}  // namespace

MarketDataDate MarketDataDate::Parse(const std::string& value) {
  MarketDataDate date;
  char dot1 = '\0';
  char dot2 = '\0';
  std::istringstream stream(value);
  stream >> date.year >> dot1 >> date.month >> dot2 >> date.day;
  if (!stream || dot1 != '.' || dot2 != '.' || !date.IsValid()) {
    throw std::invalid_argument("Invalid date format, expected YYYY.MM.DD: " +
                                value);
  }
  return date;
}

std::string MarketDataDate::ToString() const {
  std::ostringstream stream;
  stream << std::setfill('0') << std::setw(4) << year << "." << std::setw(2)
         << month << "." << std::setw(2) << day;
  return stream.str();
}

MarketDataDate MarketDataDate::NextDay() const {
  std::tm value = ToTm(*this);
  value.tm_mday += 1;
  const std::time_t normalized = std::mktime(&value);
  if (normalized == static_cast<std::time_t>(-1)) {
    throw std::runtime_error("Cannot increment date: " + ToString());
  }

  const std::tm* result = std::localtime(&normalized);
  return MarketDataDate{result->tm_year + 1900, result->tm_mon + 1,
                        result->tm_mday};
}

bool MarketDataDate::IsValid() const {
  std::tm value = ToTm(*this);
  const std::time_t normalized = std::mktime(&value);
  if (normalized == static_cast<std::time_t>(-1)) {
    return false;
  }

  const std::tm* result = std::localtime(&normalized);
  return result->tm_year + 1900 == year && result->tm_mon + 1 == month &&
         result->tm_mday == day;
}

bool operator<(const MarketDataDate& left, const MarketDataDate& right) {
  if (left.year != right.year) {
    return left.year < right.year;
  }
  if (left.month != right.month) {
    return left.month < right.month;
  }
  return left.day < right.day;
}

bool operator==(const MarketDataDate& left, const MarketDataDate& right) {
  return left.year == right.year && left.month == right.month &&
         left.day == right.day;
}

bool operator<=(const MarketDataDate& left, const MarketDataDate& right) {
  return left < right || left == right;
}

}  // namespace sshumdloader

