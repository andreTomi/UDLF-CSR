// Differential profiler: frozen serial oracle versus current CPRR candidate.
// This is intentionally separate from the production registry and benchmark
// build flags. K1 remains serial in both engines; its mirrored writes are not
// safe to parallelize by source row without a gather or explicit reduction.
#include "../../Source/src/Methods/hpp/CprrSparseCore.hpp"
#include "CprrSparseSerialOracle.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <unordered_set>
#include <unistd.h>
#include <vector>

#if defined(_OPENMP)
#include <omp.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
  size_t n = 0, L = 0, k = 0, t = 0, repeats = 1;
  uint64_t seed = 1;
  std::string distribution = "random";
  std::string input;
  std::string engine = "both";
  std::string output;
  std::string data_seed = "1";
  std::string commit = "unknown";
  size_t expected_threads = 1;
  bool mutation_probe = false;
  std::string kernel_probe;
};

struct Metrics {
  size_t max_nref = 0;
  uint64_t sum_nref_squared = 0;
  double max_abs_accumulator = 0.0;
  size_t vector_capacity_bytes = 0;
  size_t peak_vector_capacity_bytes = 0;
};

struct ProfileRow {
  std::string engine, phase;
  size_t iteration = 0;
  double seconds = 0.0;
  double kernel_seconds = 0.0;
  Metrics metrics;
};

struct RunResult {
  std::vector<uint32_t> lists;
  std::vector<ProfileRow> rows;
  double wall_seconds = 0.0;
  double kernel_seconds = 0.0;
};

struct CandidateEngine {
  using State = cprr::SparseState;
  static State make(std::vector<uint32_t> a, size_t n, size_t L,
                    size_t k, size_t t) { return cprr::make_state(std::move(a), n, L, k, t, L); }
  static void k1(State& s) { cprr::k1_fillPos(s); }
  static void k2(State& s) { cprr::k2_sortZero(s); }
  static void k3(State& s) { cprr::k3_cartProd(s); }
  static void k4(State& s) { cprr::k4_reverseCartProd(s); }
  static void k5(State& s) { cprr::k5_cleanRef(s); }
  static void k6(State& s) { cprr::k6_sort(s); }
};

struct OracleEngine {
  using State = cprr_serial_oracle::SparseState;
  static State make(const std::vector<uint32_t>& a, size_t n, size_t L,
                    size_t k, size_t t) {
    return cprr_serial_oracle::make_state(a, n, L, k, t, L);
  }
  static void k1(State& s) { cprr_serial_oracle::k1_fillPos(s); }
  static void k2(State& s) { cprr_serial_oracle::k2_sortZero(s); }
  static void k3(State& s) { cprr_serial_oracle::k3_cartProd(s); }
  static void k4(State& s) { cprr_serial_oracle::k4_reverseCartProd(s); }
  static void k5(State& s) { cprr_serial_oracle::k5_cleanRef(s); }
  static void k6(State& s) { cprr_serial_oracle::k6_sort(s); }
};

void usage(std::ostream& out) {
  out << "Usage: benchmark_serial --n N --L L --k K --t T [options]\n"
      << "Options:\n"
      << "  --seed S                    Deterministic generator seed (default 1)\n"
      << "  --distribution random|hub   Synthetic graph shape (default random)\n"
      << "  --input FILE               Read n*L ranking IDs from a text file instead\n"
      << "  --engine both|candidate|oracle  Select measured engine (default both)\n"
      << "  --repeats N                 Full oracle/candidate repetitions (default 1)\n"
      << "  --output FILE               CSV output (default stdout)\n"
      << "  --commit HASH               Source revision identifier\n"
      << "  --expected-threads N        Benchmark harness thread target (default 1)\n"
      << "  --kernel-probe K3|K4|K2|K6 Benchmark one existing stage per mode\n"
      << "  --mutation-probe            Confirm exact-list gate detects a planted mismatch\n"
      << "  --help                      Show this help\n";
}

size_t parse_size(const std::string& value, const char* name) {
  size_t consumed = 0;
  const unsigned long long parsed = std::stoull(value, &consumed);
  if (consumed != value.size()) throw std::runtime_error(std::string("invalid ") + name);
  if (parsed > std::numeric_limits<size_t>::max())
    throw std::runtime_error(std::string(name) + " exceeds size_t");
  return static_cast<size_t>(parsed);
}

Arguments parse_args(int argc, char** argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--help") { usage(std::cout); std::exit(0); }
    if (option == "--mutation-probe") { args.mutation_probe = true; continue; }
    if (i + 1 >= argc) throw std::runtime_error("missing value for " + option);
    const std::string value = argv[++i];
    if (option == "--n") args.n = parse_size(value, "n");
    else if (option == "--L") args.L = parse_size(value, "L");
    else if (option == "--k") args.k = parse_size(value, "k");
    else if (option == "--t") args.t = parse_size(value, "t");
    else if (option == "--repeats") args.repeats = parse_size(value, "repeats");
    else if (option == "--seed") {
      size_t consumed = 0;
      args.seed = std::stoull(value, &consumed);
      if (consumed != value.size()) throw std::runtime_error("invalid seed");
      args.data_seed = value;
    }
    else if (option == "--distribution") args.distribution = value;
    else if (option == "--input") args.input = value;
    else if (option == "--engine") args.engine = value;
    else if (option == "--output") args.output = value;
    else if (option == "--commit") args.commit = value;
    else if (option == "--expected-threads") args.expected_threads = parse_size(value, "expected threads");
    else if (option == "--kernel-probe") args.kernel_probe = value;
    else throw std::runtime_error("unknown option " + option);
  }
  if (args.n == 0 || args.L == 0 || args.k == 0 || args.t == 0)
    throw std::runtime_error("--n, --L, --k and --t are required and must be > 0");
  if (args.L > args.n || args.k > args.L || args.n > UINT32_MAX)
    throw std::runtime_error("require L <= n, k <= L and n <= UINT32_MAX");
  if (args.repeats == 0) throw std::runtime_error("--repeats must be > 0");
  if (args.expected_threads == 0) throw std::runtime_error("--expected-threads must be > 0");
  if (args.distribution != "random" && args.distribution != "hub")
    throw std::runtime_error("--distribution must be random or hub");
  if (args.engine != "both" && args.engine != "candidate" && args.engine != "oracle")
    throw std::runtime_error("--engine must be both, candidate, or oracle");
  if (!args.kernel_probe.empty() && args.kernel_probe != "K2" && args.kernel_probe != "K3" &&
      args.kernel_probe != "K4" && args.kernel_probe != "K6")
    throw std::runtime_error("--kernel-probe must be K2, K3, K4 or K6");
  if (!args.kernel_probe.empty() && args.engine == "both")
    throw std::runtime_error("--kernel-probe requires --engine candidate or oracle");
  if (args.mutation_probe && args.engine != "both")
    throw std::runtime_error("--mutation-probe requires --engine both");
  if (args.n > std::numeric_limits<size_t>::max() / args.L)
    throw std::runtime_error("n*L overflows size_t");
  return args;
}

void append_unique_random(std::vector<uint32_t>& row,
                          std::unordered_set<uint32_t>& seen,
                          size_t n, size_t target, std::mt19937_64& random) {
  std::uniform_int_distribution<uint32_t> pick(0, static_cast<uint32_t>(n - 1));
  while (row.size() < target) {
    const uint32_t candidate = pick(random);
    if (seen.insert(candidate).second) row.push_back(candidate);
    // For dense rows, switch to a shuffled complement to avoid coupon-collector
    // behavior. This branch also covers L == n.
    if (row.size() < target && seen.size() > n / 4) {
      std::vector<uint32_t> remaining;
      remaining.reserve(n - seen.size());
      for (size_t id = 0; id < n; ++id)
        if (!seen.count(static_cast<uint32_t>(id))) remaining.push_back(static_cast<uint32_t>(id));
      std::shuffle(remaining.begin(), remaining.end(), random);
      for (uint32_t id : remaining) {
        if (row.size() == target) break;
        if (seen.insert(id).second) row.push_back(id);
      }
    }
  }
}

std::vector<uint32_t> generate_input(const Arguments& args) {
  std::mt19937_64 random(args.seed);
  std::vector<uint32_t> input(args.n * args.L);
  const uint32_t hub = static_cast<uint32_t>(args.n - 1);
  for (size_t r = 0; r < args.n; ++r) {
    std::vector<uint32_t> row;
    row.reserve(args.L);
    std::unordered_set<uint32_t> seen;
    seen.reserve(args.L * 2 + 1);
    const uint32_t self = static_cast<uint32_t>(r);
    row.push_back(self);
    seen.insert(self);
    if (args.distribution == "hub" && r != hub && args.k > 1) {
      row.push_back(hub);
      seen.insert(hub);
    }
    append_unique_random(row, seen, args.n, args.L, random);
    if (args.distribution == "random") std::shuffle(row.begin(), row.end(), random);
    if (row.size() != args.L) throw std::runtime_error("synthetic generator produced a short row");
    std::copy(row.begin(), row.end(), input.begin() + r * args.L);
  }
  return input;
}

std::vector<uint32_t> read_input(const Arguments& args) {
  if (args.input.empty()) return generate_input(args);
  std::ifstream input(args.input);
  if (!input) throw std::runtime_error("cannot open input file " + args.input);
  std::vector<uint32_t> values;
  values.reserve(args.n * args.L);
  uint64_t value = 0;
  while (input >> value) {
    if (value > UINT32_MAX) throw std::runtime_error("input ID exceeds uint32_t");
    values.push_back(static_cast<uint32_t>(value));
    if (values.size() > args.n * args.L)
      throw std::runtime_error("input contains more than n*L IDs");
  }
  if (!input.eof() || values.size() != args.n * args.L)
    throw std::runtime_error("input token count differs from n*L");
  return values;
}

uint64_t saturating_square_sum(const std::vector<size_t>& refs) {
  uint64_t sum = 0;
  for (size_t count : refs) {
    if (count != 0 && count > std::numeric_limits<uint64_t>::max() / count)
      return std::numeric_limits<uint64_t>::max();
    const uint64_t square = static_cast<uint64_t>(count) * count;
    if (square > std::numeric_limits<uint64_t>::max() - sum)
      return std::numeric_limits<uint64_t>::max();
    sum += square;
  }
  return sum;
}

template <class State>
Metrics collect_metrics(const State& state) {
  Metrics metrics;
  for (size_t refs : state.nRef) metrics.max_nref = std::max(metrics.max_nref, refs);
  metrics.sum_nref_squared = saturating_square_sum(state.nRef);
  for (float value : state.values) {
    const double magnitude = std::fabs(static_cast<double>(value));
    if (magnitude > metrics.max_abs_accumulator) metrics.max_abs_accumulator = magnitude;
  }
  metrics.vector_capacity_bytes =
      state.lists.capacity() * sizeof(uint32_t) + state.col_idx.capacity() * sizeof(uint32_t) +
      state.values.capacity() * sizeof(float) + state.rev_src.capacity() * sizeof(size_t) +
      state.rev_pos.capacity() * sizeof(size_t) + state.rev_rowptr.capacity() * sizeof(size_t) +
      state.nRef.capacity() * sizeof(size_t);
  metrics.peak_vector_capacity_bytes = metrics.vector_capacity_bytes;
  return metrics;
}

template <class State, class Function>
void timed_stage(const std::string& engine, const std::string& phase,
                 size_t iteration, State& state, Function function,
                 std::vector<ProfileRow>& rows) {
  const auto start = Clock::now();
  function(state);
  const auto stop = Clock::now();
  size_t vector_capacity_bytes =
      state.lists.capacity() * sizeof(uint32_t) + state.col_idx.capacity() * sizeof(uint32_t) +
      state.values.capacity() * sizeof(float) + state.rev_src.capacity() * sizeof(size_t) +
      state.rev_pos.capacity() * sizeof(size_t) + state.rev_rowptr.capacity() * sizeof(size_t) +
      state.nRef.capacity() * sizeof(size_t);
  for (const auto& prior : rows)
    if (prior.engine == engine)
      vector_capacity_bytes = std::max(vector_capacity_bytes, prior.metrics.peak_vector_capacity_bytes);
  ProfileRow row;
  row.engine = engine;
  row.phase = phase;
  row.iteration = iteration;
  row.seconds = std::chrono::duration<double>(stop - start).count();
  row.kernel_seconds = row.seconds;
  row.metrics = collect_metrics(state);
  row.metrics.peak_vector_capacity_bytes = vector_capacity_bytes;
  rows.push_back(std::move(row));
}

template <class Engine>
RunResult run_engine(std::vector<uint32_t> input, const Arguments& args,
                     const std::string& name) {
  RunResult result;
  const auto run_start = Clock::now();
  auto state = Engine::make(std::move(input), args.n, args.L, args.k, args.t);
  timed_stage(name, "K1_fillPos", 0, state, Engine::k1, result.rows);
  timed_stage(name, "K2_sortZero_initial", 0, state, Engine::k2, result.rows);
  for (size_t iteration = 1; iteration <= args.t; ++iteration) {
    timed_stage(name, "K3_cartProd", iteration, state, Engine::k3, result.rows);
    timed_stage(name, "K4_reverseCartProd", iteration, state, Engine::k4, result.rows);
    timed_stage(name, "K5_cleanRef", iteration, state, Engine::k5, result.rows);
    if (iteration == args.t)
      timed_stage(name, "K6_sort_final", iteration, state, Engine::k6, result.rows);
    else
      timed_stage(name, "K2_sortZero_iteration", iteration, state, Engine::k2, result.rows);
  }
  result.lists = std::move(state.lists);
  const auto run_stop = Clock::now();
  result.wall_seconds = std::chrono::duration<double>(run_stop - run_start).count();
  for (const auto& row : result.rows) result.kernel_seconds += row.kernel_seconds;
  return result;
}

template <class Engine>
std::vector<ProfileRow> run_kernel_probe(const std::vector<uint32_t>& input,
                                         const Arguments& args,
                                         const std::string& engine_name) {
  std::vector<ProfileRow> rows;
  auto state = Engine::make(input, args.n, args.L, args.k, args.t);
  Engine::k1(state);
  Engine::k2(state);
  if (args.kernel_probe == "K3") {
    timed_stage(engine_name, "K3_cartProd", 1, state, Engine::k3, rows);
  } else if (args.kernel_probe == "K4") {
    Engine::k3(state);
    // K4 requires K3's reverse CSR, but K3 timing itself is excluded here.
    timed_stage(engine_name, "K4_reverseCartProd", 1, state, Engine::k4, rows);
  } else {
    Engine::k3(state);
    Engine::k4(state);
    Engine::k5(state);
    if (args.kernel_probe == "K2")
      timed_stage(engine_name, "K2_sortZero_probe", 1, state, Engine::k2, rows);
    else
      timed_stage(engine_name, "K6_sort_probe", 1, state, Engine::k6, rows);
  }
  return rows;
}

size_t process_hwm_kib() {
  struct rusage usage_data {};
  if (getrusage(RUSAGE_SELF, &usage_data) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<size_t>(usage_data.ru_maxrss / 1024);
#else
  return static_cast<size_t>(usage_data.ru_maxrss);
#endif
}

int configured_threads() {
#if defined(_OPENMP)
  return omp_get_max_threads();
#else
  return 1;
#endif
}

void write_csv_header(std::ostream& out, const Arguments& args) {
  out << "# cprr_sparse_profile_version=1\n"
      << "# commit=" << args.commit << ",compiler=" << __VERSION__
      << ",n=" << args.n << ",L=" << args.L << ",k=" << args.k << ",t=" << args.t
      << ",seed=" << args.seed << ",distribution=" << args.distribution
      << ",input=" << (args.input.empty() ? "synthetic" : args.input)
      << ",seed_text=" << args.data_seed
      << ",engine=" << args.engine << ",actual_threads=" << configured_threads()
      << ",expected_threads=" << args.expected_threads
      << ",cpu_affinity_count=" << sysconf(_SC_NPROCESSORS_ONLN) << "\n"
      << "# hwm_kib is process high-water RSS for the requested run(s); live/peak vector capacities are tracked per engine\n"
      << "record,repeat,engine,iteration,phase,kernel_seconds,timing_region_seconds,max_nref,sum_nref_squared,max_abs_accumulator,vector_capacity_bytes,peak_vector_capacity_bytes,pair_hwm_kib\n";
}

void write_csv_rows(std::ostream& out, const std::vector<ProfileRow>& rows,
                    size_t repeat, size_t pair_hwm) {
  for (const auto& row : rows) {
    out << "stage," << repeat << ',' << row.engine << ',' << row.iteration << ',' << row.phase << ','
        << std::setprecision(9) << row.kernel_seconds << ',' << row.seconds << ',' << row.metrics.max_nref << ','
        << row.metrics.sum_nref_squared << ',' << std::setprecision(9)
        << row.metrics.max_abs_accumulator << ',' << row.metrics.vector_capacity_bytes
        << ',' << row.metrics.peak_vector_capacity_bytes << ',' << pair_hwm << '\n';
  }
}

void write_summary(std::ostream& out, const std::string& engine, size_t repeat,
                   double kernel_seconds, double wall_seconds, size_t pair_hwm) {
  out << "summary," << repeat << ',' << engine << ",0,total_kernel_seconds,"
      << std::setprecision(9) << kernel_seconds << ",0,0,0,0,0,0," << pair_hwm << '\n'
      << "summary," << repeat << ',' << engine << ",0,engine_wall_seconds,"
      << std::setprecision(9) << wall_seconds << ",0,0,0,0,0,0," << pair_hwm << '\n';
}

bool same_lists(const std::vector<uint32_t>& candidate,
                const std::vector<uint32_t>& oracle) {
  return candidate == oracle;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Arguments args = parse_args(argc, argv);
    std::vector<uint32_t> input = read_input(args);
    cprr::validate_input_I4(input.data(), args.n, args.L, args.k, "CPRR_PROFILE");
    cprr_serial_oracle::validate_input_I4(input.data(), args.n, args.L, args.k,
                                          "CPRR_PROFILE_ORACLE");

    std::ofstream file;
    std::ostream* output = &std::cout;
    if (!args.output.empty()) {
      file.open(args.output, std::ios::out | std::ios::trunc);
      if (!file) throw std::runtime_error("cannot open output file " + args.output);
      output = &file;
    }
    write_csv_header(*output, args);

    if (!args.kernel_probe.empty()) {
      for (size_t repeat = 1; repeat <= args.repeats; ++repeat) {
        std::vector<ProfileRow> rows;
        if (args.engine == "candidate")
          rows = run_kernel_probe<CandidateEngine>(input, args, "candidate");
        else
          rows = run_kernel_probe<OracleEngine>(input, args, "serial_oracle");
        write_csv_rows(*output, rows, repeat, process_hwm_kib());
      }
      if (!*output) throw std::runtime_error("failed writing kernel probe output");
      return 0;
    }

    for (size_t repeat = 1; repeat <= args.repeats; ++repeat) {
      RunResult candidate;
      RunResult oracle;
      if (args.engine != "oracle")
        candidate = run_engine<CandidateEngine>(args.engine == "candidate" ?
                                                   std::move(input) : input,
                                               args, "candidate");
      if (args.engine != "candidate")
        oracle = run_engine<OracleEngine>(input, args, "serial_oracle");
      const size_t pair_hwm = process_hwm_kib();
      if (args.engine == "both" && !same_lists(candidate.lists, oracle.lists)) {
        size_t mismatch = 0;
        while (mismatch < candidate.lists.size() && candidate.lists[mismatch] == oracle.lists[mismatch]) ++mismatch;
        std::cerr << "CPRR_PROFILE,DIFF,repeat=" << repeat << ",row=" << mismatch / args.L
                  << ",position=" << mismatch % args.L << '\n';
        return 3;
      }
      if (args.mutation_probe) {
        std::vector<uint32_t> mutated = candidate.lists;
        if (mutated.size() < 2) throw std::runtime_error("mutation probe needs at least two ranking IDs");
        std::swap(mutated[0], mutated[1]);
        if (same_lists(mutated, oracle.lists))
          throw std::runtime_error("mutation probe failed to detect planted ranking mismatch");
      }
      std::vector<ProfileRow> rows;
      if (args.engine != "oracle")
        rows.insert(rows.end(), candidate.rows.begin(), candidate.rows.end());
      if (args.engine != "candidate")
        rows.insert(rows.end(), oracle.rows.begin(), oracle.rows.end());
      write_csv_rows(*output, rows, repeat, pair_hwm);
      if (args.engine != "oracle")
        write_summary(*output, "candidate", repeat, candidate.kernel_seconds,
                      candidate.wall_seconds, pair_hwm);
      if (args.engine != "candidate")
        write_summary(*output, "serial_oracle", repeat, oracle.kernel_seconds,
                      oracle.wall_seconds, pair_hwm);
      const size_t id_count = args.engine == "oracle" ? oracle.lists.size() : candidate.lists.size();
      std::cerr << "CPRR_PROFILE," << (args.engine == "both" ? "DIFF_PASS" : "PROFILE_ONLY")
                << ",repeat=" << repeat << ",engine=" << args.engine << ",ids=" << id_count
                << ",pair_hwm_kib=" << pair_hwm
                << ",mutation_probe=" << (args.mutation_probe ? "detected" : "not_requested") << '\n';
    }
    if (!*output) throw std::runtime_error("failed writing profile output");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "CPRR_PROFILE,ERROR," << error.what() << '\n';
    return 1;
  }
}