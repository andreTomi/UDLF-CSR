#include <iostream>
#include <map>
#include <string>

#include "Config.hpp"
#include "ConfigParser.hpp"
#include "ConfigProvider.hpp"
#include "DataLoader.hpp"
#include "Logger.hpp"
#include "MethodFactory.hpp"
#include "Type.hpp"
#include "Validator.hpp"

int main(int argc, char *argv[]) {
  std::string configFilePath;
  if (argc != 2) {
    configFilePath = "config.ini";  // default filename
  } else {
    configFilePath = argv[1];
  }

  try {
    std::map<std::string, Type::ParamTypeVariants> configData = ConfigParser::parseConfigFile(configFilePath);
    ConfigProvider::getInstance().initialize(configData);
    const auto &config = ConfigProvider::getInstance().getConfig();
    DataLoader::getInstance().initialize();
    Logger::getInstance().initialize();

    std::string methodName = config.method;
    std::cout.flush();
    std::cout << "Running Method: " << methodName << std::endl;

    auto method = MethodFactory::createMethod(methodName);
    method->run();

    method.reset();  // Clean up the method instance
    std::cout << "\nExecution Completed!" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
  }

  return 0;
}
