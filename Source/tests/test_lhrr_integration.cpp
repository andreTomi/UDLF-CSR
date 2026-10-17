// Framework-level LHRR_SPARSE integration gate. Each invocation is a fresh
// process because ConfigProvider/DataProvider/EventQueue are singletons.
#include "Config/hpp/ConfigProvider.hpp"
#include "Core/hpp/DataLoader.hpp"
#include "Methods/hpp/LHRRSparse.hpp"
#include "Methods/hpp/MethodFactory.hpp"
#include "Config/hpp/ConfigParser.hpp"
#include "Utils/hpp/Logger.hpp"
#include "Utils/hpp/Time.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

MethodRegister<LHRRSparse> LHRRSparse::signInMethod("LHRR_SPARSE");

namespace {

uint32_t read_le32(std::istream& in) {
  unsigned char bytes[4];
  if (!in.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
    throw std::runtime_error("short snapshot");
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t read_le64(std::istream& in) {
  unsigned char bytes[8];
  if (!in.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
    throw std::runtime_error("short snapshot");
  uint64_t value = 0;
  for (unsigned int i = 0; i < 8; ++i)
    value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  return value;
}

std::vector<uint32_t> read_expected(const std::string& path, size_t n, size_t l) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open expected snapshot: " + path);
  char magic[4];
  if (!in.read(magic, sizeof(magic)) || std::string(magic, 4) != "SIMP")
    throw std::runtime_error("invalid expected snapshot");
  if (read_le32(in) != 1) throw std::runtime_error("unsupported snapshot version");
  (void)read_le32(in);
  (void)read_le32(in);
  (void)read_le32(in);
  (void)read_le32(in);
  if (read_le64(in) != n || read_le64(in) != l || read_le64(in) == 0)
    throw std::runtime_error("expected snapshot dimensions mismatch");
  const uint64_t count = read_le64(in);
  if ((read_le32(in) & 0xffu) != 1 || count != n * l)
    throw std::runtime_error("expected snapshot is not a uint32 ranking");
  std::vector<uint32_t> values(count);
  for (uint32_t& value : values) value = read_le32(in);
  return values;
}

std::vector<uint32_t> read_output(const std::string& path, size_t n, size_t l) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open generated ranking: " + path);
  std::vector<uint32_t> values;
  values.reserve(n * l);
  uint64_t value = 0;
  for (size_t row = 0; row < n; ++row) {
    for (size_t col = 0; col < l; ++col) {
      if (!(in >> value) || value > UINT32_MAX)
        throw std::runtime_error("generated ranking has invalid token");
      values.push_back(static_cast<uint32_t>(value));
    }
    std::string remainder;
    std::getline(in, remainder);
  }
  return values;
}

void compare(const std::vector<uint32_t>& actual,
             const std::vector<uint32_t>& expected, size_t l) {
  if (actual.size() != expected.size()) throw std::runtime_error("ranking size mismatch");
  for (size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] != expected[i])
      throw std::runtime_error("framework ranking mismatch at row " +
                               std::to_string(i / l) + ", position " +
                               std::to_string(i % l));
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 10 && argc != 11) {
      std::fprintf(stderr,
                   "usage: test_lhrr_integration N L K T INPUT EXPECTED.simp WORKDIR MODE FIXTURE [THREADS]\n");
      return 2;
    }
    const size_t n = std::stoull(argv[1]);
    const size_t l = std::stoull(argv[2]);
    const int k = std::stoi(argv[3]);
    const int t = std::stoi(argv[4]);
    const std::string input = std::filesystem::absolute(argv[5]).string();
    const std::string expected_path = std::filesystem::absolute(argv[6]).string();
    const std::filesystem::path workdir = std::filesystem::absolute(argv[7]);
    const std::string mode = argv[8];
    const std::string fixture = argv[9];
    const int threads = argc == 11 ? std::stoi(argv[10]) : 1;
    if (mode != "framework") throw std::runtime_error("mode must be framework");
    std::filesystem::create_directories(workdir);
    // A legacy INI intentionally omits the newly introduced thread setting.
    // Parsing and initializing it must supply the serial default without
    // throwing, preserving old configurations.
    const auto config_path = workdir / "method.ini";
    {
      std::ofstream config_file(config_path);
      config_file << "UDL_TASK = UDL\n"
                  << "UDL_METHOD = LHRR_SPARSE\n"
                  << "SIZE_DATASET = " << n << "\n"
                  << "INPUT_FILE = " << input << "\n"
                  << "INPUT_FILE_FORMAT = RK\n"
                  << "INPUT_RK_FORMAT = NUM\n"
                  << "INPUT_MATRIX_TYPE = SIM\n"
                  << "MATRIX_TO_RK_SORTING = HEAP\n"
                  << "INPUT_FILE_LIST = " << (workdir / "images.txt").string() << "\n"
                  << "INPUT_FILE_CLASSES = " << (workdir / "classes.txt").string() << "\n"
                  << "INPUT_IMAGES_PATH = " << workdir.string() << "/\n"
                  << "OUTPUT_FILE = TRUE\n"
                  << "OUTPUT_FILE_FORMAT = RK\n"
                  << "OUTPUT_MATRIX_TYPE = SIM\n"
                  << "OUTPUT_RK_FORMAT = NUM\n"
                  << "OUTPUT_FILE_PATH = " << (workdir / "matrix.txt").string() << "\n"
                  << "OUTPUT_LOG_FILE_PATH = " << (workdir / "run.log").string() << "\n"
                  << "EFFICIENCY_EVAL = FALSE\n"
                  << "EFFECTIVENESS_EVAL = FALSE\n"
                  << "EFFECTIVENESS_COMPUTE_MAP = FALSE\n"
                  << "EFFECTIVENESS_COMPUTE_PRECISIONS = FALSE\n"
                  << "EFFECTIVENESS_COMPUTE_RECALL = FALSE\n"
                  << "EFFECTIVENESS_RECALLS_TO_COMPUTE = \n"
                  << "EFFECTIVENESS_PRECISIONS_TO_COMPUTE = \n"
                  << "PARAM_LHRR_SPARSE_L = " << l << "\n"
                  << "PARAM_LHRR_SPARSE_K = " << k << "\n"
                  << "PARAM_LHRR_SPARSE_T = " << t << "\n";
      if (threads != 1)
        config_file << "PARAM_LHRR_SPARSE_THREADS = " << threads << "\n";
    }
    const auto parsed = ConfigParser::parseConfigFile(config_path.string());
    const auto parsed_method = parsed.find("UDL_METHOD");
    if (parsed_method == parsed.end() ||
        std::get<std::string>(parsed_method->second) != "LHRR_SPARSE")
      throw std::runtime_error("INI parser did not accept UDL_METHOD=LHRR_SPARSE");
    const auto parsed_threads = parsed.find("PARAM_LHRR_SPARSE_THREADS");
    if (threads == 1 && parsed_threads != parsed.end())
      throw std::runtime_error("legacy INI unexpectedly contains thread parameter");
    ConfigProvider::getInstance().initialize(parsed);
    const int configured_threads =
        ConfigProvider::getInstance().getConfig().get<int>("PARAM_LHRR_SPARSE_THREADS");
    if (threads == 1 && configured_threads != 1)
      throw std::runtime_error("legacy INI did not default LHRR threads to one");
    if (configured_threads != threads)
      throw std::runtime_error("INI thread configuration did not resolve to expected value");

    std::filesystem::create_directories(workdir / "output");
    DataLoader::getInstance().initialize();
    Logger::getInstance().initialize();
    DataProvider& data = DataProvider::getInstance();
    data.setL(static_cast<uint32_t>(l));
    data.getRkLists().assignFromTextFile(input, n, l);
    const auto expected = read_expected(expected_path, n, l);

    const auto& loaded_config = ConfigProvider::getInstance().getConfig();
    std::ofstream image_list(loaded_config.input->inputFileList);
    std::ofstream class_list(loaded_config.input->inputFileClasses);
    for (size_t row = 0; row < n; ++row) {
      image_list << row << ".jpg\n";
      class_list << row << ".jpg:" << (row % 2) << '\n';
    }
    image_list.close();
    class_list.close();

    const auto old_cwd = std::filesystem::current_path();
    std::filesystem::current_path(workdir);
    auto method = MethodFactory::createMethod("LHRR_SPARSE");
    method->run();
    const auto actual = read_output((workdir / "output/reranked_lists.txt").string(), n, l);
    std::filesystem::current_path(old_cwd);
    compare(actual, expected, l);
    std::printf("LHRR_FRAMEWORK,PASS,fixture=%s,n=%zu,L=%zu,k=%d,t=%d,threads=%d,ids=%zu\n",
                fixture.c_str(), n, l, k, t, threads, actual.size());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "LHRR_FRAMEWORK,FAIL,%s\n", error.what());
    return 1;
  }
}