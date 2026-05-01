#ifndef METHOD_FACTORY_HPP
#define METHOD_FACTORY_HPP

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "Config.hpp"
#include "Method.hpp"
#include "Validator.hpp"

class MethodFactory {
 public:
  // Creates a method instance dynamically
  static std::unique_ptr<Method> createMethod(const std::string &methodName) {
    std::cout << "Available Methods in Registry: ";
    for (const auto &pair : getRegistry()) {
      std::cout << pair.first << " ";
    }
    std::cout << std::endl;

    auto it = getRegistry().find(methodName);
    if (it != getRegistry().end()) {
      return it->second();
    }
    throw std::runtime_error("Unknown method: " + methodName);
  }

 private:
  // Registry to map method names to constructors
  static std::map<std::string, std::function<std::unique_ptr<Method>()>> &getRegistry() {
    static std::map<std::string, std::function<std::unique_ptr<Method>()>> registry;
    return registry;
  }

  // Method classes will call this at startup to register themselves
  template <typename T>
  friend class MethodRegister;
};

// Helper class to register methods at compile time
template <typename T>
class MethodRegister {
 public:
  MethodRegister(const std::string &methodName) {
    MethodFactory::getRegistry()[methodName] = []() {
      return std::make_unique<T>();
    };
  }
};

#endif  // METHOD_FACTORY_HPP
