import re

with open('Source/src/Methods/hpp/RFE.hpp', 'r') as f:
    s = f.read()

old = '''  // ---------------------------------------------------------------- config --
  void preprocess() override {
    require_strict_build();

    k = static_cast<int>(config.get<int>("PARAM_RFE_K"));
    t = config.get<int>("PARAM_RFE_T");
    l = static_cast<uint32_t>(config.get<int>("PARAM_RFE_L"));
    pa = config.get<double>("PARAM_RFE_PA");
    performCCs = config.get<bool>("PARAM_RFE_PERFORM_CCS");

    // Legacy also has rerankByEmb / exportEmbeddings / paths -- not ported.
    if (config.get<bool>("PARAM_RFE_RERANK_BY_EMB") ||
        config.get<bool>("PARAM_RFE_EXPORT_EMBEDDINGS")) {
      throw std::runtime_error(
          "RFE: PARAM_RFE_RERANK_BY_EMB / EXPORT_EMBEDDINGS not ported");
    }

    // Contractual validations (legacy exit(1) -> throw)
    if (l > n) throw std::runtime_error("PARAM_RFE_L can't be greater than SIZE_DATASET");
    if (k > static_cast<int>(l)) throw std::runtime_error("RFE: need K <= L");
    if (k < 2) throw std::runtime_error("RFE: need K >= 2");

    // DataProvider stride == l (rkLists width == l, no L_MULT)
    dataProvider.setL(l);

    logMessage("[RFE] Initialized (strict build required; PERFORM_CCS refused if true)");
  }'''

new = '''  // ---------------------------------------------------------------- config --
  void preprocess() override {
    require_strict_build();

    k = static_cast<int>(config.get<int>("PARAM_RFE_K"));
    t = config.get<int>("PARAM_RFE_T");
    l = static_cast<uint32_t>(config.get<int>("PARAM_RFE_L"));
    pa = config.get<double>("PARAM_RFE_PA");
    performCCs = config.get<bool>("PARAM_RFE_PERFORM_CCS");

    // Legacy also has rerankByEmb / exportEmbeddings / paths -- not ported.
    if (config.get<bool>("PARAM_RFE_RERANK_BY_EMB") ||
        config.get<bool>("PARAM_RFE_EXPORT_EMBEDDINGS")) {
      throw std::runtime_error(
          "RFE: PARAM_RFE_RERANK_BY_EMB / EXPORT_EMBEDDINGS not ported");
    }

    // Contractual validations (legacy exit(1) -> throw)
    if (l > n) throw std::runtime_error("PARAM_RFE_L can't be greater than SIZE_DATASET");
    if (k > static_cast<int>(l)) throw std::runtime_error("RFE: need K <= L");
    if (k < 2) throw std::runtime_error("RFE: need K >= 2");

    // H5: Canary check -- verify strict build produces faithful ck_gram on synthetic fixture
    // This catches silent regime drift (e.g., fast-math linked in) before running on real data.
    // Uses the pinned fixture: N=200, L=40, k=10, t=2, pa=0.1, seed=42
    // Expected faithful ck_gram (post-run()): 0x46322e3d65fe6403
    {
      std::vector<int> rk = gen_fixture(42, 200, 40);
      RFECore canary(rk, 200, 40, 10, 2, 0.1);
      canary.run();
      constexpr unsigned long long FAITHFUL_CK_GRAM = 0x46322e3d65fe6403ULL;
      if (canary.ck_gram() != FAITHFUL_CK_GRAM) {
        throw std::runtime_error(
            "RFE canary FAILED: ck_gram=" + to_hex(canary.ck_gram()) +
            " expected " + to_hex(0x46322e3d65fe6403ULL) +
            " -- this binary was likely built with fast-math or wrong flags");
      }
    }

    // DataProvider stride == l (rkLists width == l, no L_MULT)
    dataProvider.setL(l);

    logMessage("[RFE] Initialized (strict build required; PERFORM_CCS refused if true)");
  }

private:
  // Fisher-Yates generator for canary fixture (same as test_rfe_parity.cpp)
  static std::vector<int> gen_fixture(int seed, int N, int L) {
    std::vector<int> rk((std::size_t)N * (std::size_t)L, 0);
    std::mt19937 rng((unsigned)seed);
    std::vector<int> perm((std::size_t)N);
    for (int i = 0; i < N; ++i) perm[(std::size_t)i] = i;
    for (int i = 0; i < N; ++i) {
      std::shuffle(perm.begin(), perm.end(), rng);
      auto it = std::find(perm.begin(), perm.end(), i);
      std::swap(perm[0], *it);
      for (int j = 0; j < L; ++j)
        rk[(std::size_t)i * (std::size_t)L + (std::size_t)j] = perm[(std::size_t)j];
    }
    return rk;
  }

  static std::string to_hex(unsigned long long v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "0x%016llx", v);
    return std::string(buf);
  }

  int k = 20;
  int t = 2;
  uint32_t l = 400;
  double pa = 0.1;
  bool performCCs = false;

  static MethodRegister<RFE> signInMethod;'''

with open('Source/src/Methods/hpp/RFE.hpp', 'r') as f:
    s = f.read()

if old in s:
    s = s.replace(old, new, 1)
    print('Replaced preprocess')
else:
    print('NOT FOUND')

with open('Source/src/Methods/hpp/RFE.hpp', 'w') as f:
    f.write(s)
print('Done')