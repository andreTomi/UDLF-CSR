#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include <map>
#include <string>
#include <vector>

#include "FileIO.hpp"
#include "Type.hpp"
#include "Validator.hpp"

class ConfigParser {
 public:
  // Parses a configuration file into a map of key-value pairs
  static std::map<std::string, Type::ParamTypeVariants> parseConfigFile(const std::string &filename) {
    std::map<std::string, Type::ParamTypeVariants> config;
    std::vector<std::string> lines = FileIO::readFileToLines(filename);

    for (const std::string &line : lines) {
      int pos = line.find("=");
      if (pos != -1) {
        std::string key = line.substr(0, pos);
        std::string value = removeTrailingCR(line.substr(pos + 1));

        // Determine expected type from Validator rules
        auto ruleset = Validator::getRules();
        auto ruleIt = ruleset.find(key);
        if (ruleIt != ruleset.end()) {
          const Type::Param &rule = ruleIt->second;
          try {
            if (std::holds_alternative<int>(rule.defaultValue)) {
              config[key] = Type::toNumber<int>(value);
            } else if (std::holds_alternative<double>(rule.defaultValue)) {
              config[key] = Type::toNumber<double>(value);
            } else if (std::holds_alternative<bool>(rule.defaultValue)) {
              config[key] = Type::toBool(value);
            } else {
              config[key] = value;  // Default to string if no other match
            }
          } catch (...) {
            std::cerr << "Error parsing value for key: " << key << std::endl;
          }
        } else {
          std::cerr << "Unknown param " << key << " at line in config file: " << line << std::endl;
        }
      } else {
        std::cerr << "Invalid line in config file: " << line << std::endl;
      }
    }

    return config;
  }

 private:
  // Helper function to strip the trailing '\r' character from a string
  static std::string removeTrailingCR(const std::string &str) {
    if (str.empty()) return str;
    size_t pos = str.find_last_not_of("\r");
    return str.substr(0, pos + 1);
  }
};

#endif  // CONFIG_PARSER_HPP
