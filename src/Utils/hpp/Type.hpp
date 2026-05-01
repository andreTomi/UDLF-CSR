#ifndef TYPE_HPP
#define TYPE_HPP

#include <cstdlib>  // For std::strtol, std::strtod
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

class Type {
 public:
  using ParamTypeVariants = std::variant<int, double, bool, std::string>;

  struct Param {
    ParamTypeVariants defaultValue;
    std::vector<ParamTypeVariants> expectedValues;
    bool required;
  };
  static bool toBool(const std::string &str) {
    std::locale loc;
    std::string s = "";
    for (std::string::size_type i = 0; i < str.length(); ++i)
      s += std::tolower(str[i], loc);

    return (s == "true" || s == "1");
  }
  static bool isInteger(const std::string &str) {
    if (str.empty() || ((!isdigit(str[0])) && (str[0] != '-') && (str[0] != '+')))
      return false;

    char *p;
    strtol(str.c_str(), &p, 10);
    return (*p == 0);
  }
  static bool isIntegerPos(const std::string &str) {
    if (str.empty() || ((!isdigit(str[0])) && (str[0] != '+')))
      return false;

    char *p;
    strtol(str.c_str(), &p, 10);
    return (*p == 0);
  }
  static bool isNumeric(const std::string &str) {
    std::istringstream iss(str);
    float f;
    iss >> std::noskipws >> f;
    return iss.eof() && !iss.fail();
  }
  static bool isBoolean(const std::string &str) {
    std::locale loc;
    std::string s = "";
    for (std::string::size_type i = 0; i < str.length(); ++i)
      s += std::tolower(str[i], loc);

    return (s == "true" || s == "false" || s == "1" || s == "0");
  }

  // Template function to convert string to a number
  template <typename T>
  static T toNumber(const std::string &str) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
      try {
        size_t pos;
        long long val = std::stoll(str, &pos);

        if (pos != str.size()) {
          throw std::invalid_argument("Trailing characters after number: " + str);
        }

        // Ensure the number fits within the range of the target type
        if (val < std::numeric_limits<T>::min() || val > std::numeric_limits<T>::max()) {
          throw std::out_of_range("Value out of range: " + str);
        }

        return static_cast<T>(val);
      } catch (const std::exception &e) {
        throw std::runtime_error("Invalid integer conversion: " + str);
      }
    } else if constexpr (std::is_floating_point_v<T>) {
      try {
        size_t pos;
        double val = std::stod(str, &pos);

        if (pos != str.size()) {
          throw std::invalid_argument("Trailing characters after number: " + str);
        }

        return static_cast<T>(val);
      } catch (const std::exception &e) {
        throw std::runtime_error("Invalid floating-point conversion: " + str);
      }
    } else {
      static_assert(std::is_same_v<T, void>, "Unsupported type for toNumber");
    }
  }
};

#endif  // TYPE_HPP
