#ifndef CPRR_HPP
#define CPRR_HPP

#include <algorithm>
#include <execution>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "DataProvider.hpp"
#include "Evaluation.hpp"
#include "Method.hpp"
#include "MethodFactory.hpp"
#include "Validator.hpp"

class Cprr : public Method {
 public:
  // Constructor initializes with the configuration and parameters
  explicit Cprr() : matrix(VirtualArray<uint32_t>::create(n, n, "matrix.txt")), Method() {}

 protected:
  // Overriding abstract methods from Method
  void preprocess() override {
    l = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_L"));
    k = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_K"));
    t = static_cast<uint32_t>(config.get<int>("PARAM_CPRR_T"));

    if (l > n) throw std::runtime_error("PARAM_CPRR_L can't be greater than PARAM_N");
    if (k > l) throw std::runtime_error("PARAM_CPRR_K can't be greater than PARAM_CPRR_L");
    dataProvider.setL(l);

    logMessage("[CPRR]: Initializing Data Structures...");

    std::cout << "[CPRR] L: " << l << std::endl;
    std::cout << "[CPRR] N: " << n << std::endl;
    std::cout << "[CPRR] N * L: " << n * l << std::endl;

    (*matrix).assign(0, n, n);
    imgRef.resize(n * l);
    posRef.resize(n * l);
    nRef.assign(n, 0);
  }

  void execute() override {
    logMessage("[CPRR]: Executing...");

    // Noniterative steps
    execFillPosMatrix();
    execSortRankedListsZero();

    // Iterative processing
    for (size_t iteration = 1; iteration <= t; ++iteration) {
      logMessage("[CPRR]: Iteration " + std::to_string(iteration));

      execCartProd();
      execReverseCartProd();
      logMessage("\tCPRR: Cleaning Reference List...");
      nRef.assign(n, 0);

      if (iteration != t) {
        execSortRankedListsZero();
      } else {
        execSortRankedLists();
      }
    }
  }

  void postprocess() override {
    clearAndRelease(imgRef);
    clearAndRelease(posRef);
    clearAndRelease(nRef);
    matrix.reset();
  }

 private:
  // Private helper methods specific to CPRR
  void execFillPosMatrix() {
    logMessage("\tCPRR: Filling Position Matrix...");
#pragma omp parallel for
    for (int i = 0; i < n; ++i) {
      kernelFillPosMatrix(i);
    }
  }

  void execSortRankedLists() {
    logMessage("\tCPRR: Sorting Ranked Lists...");
#pragma omp parallel for
    for (int i = 0; i < n; ++i) {
      kernelSortRankedLists(i);
    }
  }

  void execSortRankedListsZero() {
    logMessage("\tCPRR: Sorting Ranked Lists with Zero Reset...");
#pragma omp parallel for
    for (int i = 0; i < n; ++i) {
      kernelSortRankedListsZero(i);
    }
  }

  void execCartProd() {
    logMessage("\tCPRR: Executing Cartesian Product...");

    // Allocate and initialize row locks
    omp_lock_t* row_locks = new omp_lock_t[n];
    for (int i = 0; i < n; i++) {
      omp_init_lock(&row_locks[i]);
    }

#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      kernelCartProd(i, row_locks);
    }

    // Destroy and free row locks
    for (int i = 0; i < n; i++) {
      omp_destroy_lock(&row_locks[i]);
    }
    delete[] row_locks;
  }

  void execReverseCartProd() {
    logMessage("\tCPRR: Executing Reverse Cartesian Product...");

    // Allocate and initialize row locks
    omp_lock_t* row_locks = new omp_lock_t[n];
    for (int i = 0; i < n; i++) {
      omp_init_lock(&row_locks[i]);
    }

#pragma omp parallel for
    for (int i = 0; i < n; ++i) {
      kernelReverseCartProd(i, row_locks);
    }

    // Destroy and free row locks
    for (int i = 0; i < n; i++) {
      omp_destroy_lock(&row_locks[i]);
    }
    delete[] row_locks;
  }

  // Kernel functions for parallel tasks
  void kernelFillPosMatrix(uint32_t rk) {
    uint32_t lrk = l * rk;
    uint32_t nrk = n * rk;

#pragma omp simd
    for (size_t pos = 0; pos < l; pos++) {
      (*matrix)[nrk + (*rkLists)[lrk + pos]] += l - pos;
      (*matrix)[n * (*rkLists)[lrk + pos] + rk] += l - pos;
    }
  }

  void kernelSortRankedLists(uint32_t rk) {
    uint32_t LcurRL = l * rk;
    uint32_t cNcurRL = n * rk;

    // Create pairs of (score, index) for sorting
    std::vector<std::pair<uint32_t, uint32_t>> scoreIndexPairs(l);

    for (int j = 0; j < l; j++) {
      uint32_t index = cNcurRL + (*rkLists)[LcurRL + j];
      scoreIndexPairs[j] = {(*matrix)[index], (*rkLists)[LcurRL + j]};
    }

    // Sort in descending order by score using parallel execution
    std::sort(std::execution::par_unseq, scoreIndexPairs.begin(), scoreIndexPairs.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Update rkLists with sorted results
    for (int j = 0; j < l; j++) {
      (*rkLists)[LcurRL + j] = scoreIndexPairs[j].second;
    }

    // Setting query image at first position
    int i = 0;
    while ((i < l) && ((*rkLists)[LcurRL + i] != rk)) {
      i++;
    }
    if (i > 0) {
      if (i == l) i--;
      uint32_t aux = (*rkLists)[LcurRL + 0];
      (*rkLists)[LcurRL + 0] = (*rkLists)[LcurRL + i];
      (*rkLists)[LcurRL + i] = aux;
    }
  }

  void kernelSortRankedListsZero(uint32_t rk) {
    uint32_t LcurRL = l * rk;
    uint32_t cNcurRL = n * rk;

    // Create pairs of (score, index) for sorting
    std::vector<std::pair<uint32_t, uint32_t>> scoreIndexPairs(l);

    for (int j = 0; j < l; j++) {
      uint32_t index = cNcurRL + (*rkLists)[LcurRL + j];
      scoreIndexPairs[j] = {(*matrix)[index], (*rkLists)[LcurRL + j]};
      (*matrix)[index] = 0;  // Zero reset
    }

    // Sort in descending order by score using parallel execution
    std::sort(std::execution::par_unseq, scoreIndexPairs.begin(), scoreIndexPairs.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Update rkLists with sorted results
    for (int j = 0; j < l; j++) {
      (*rkLists)[LcurRL + j] = scoreIndexPairs[j].second;
    }

    // Setting query image at first position
    int i = 0;
    while ((i < l) && ((*rkLists)[LcurRL + i] != rk)) {
      i++;
    }
    if (i > 0) {
      if (i == l) i--;
      uint32_t aux = (*rkLists)[LcurRL + 0];
      (*rkLists)[LcurRL + 0] = (*rkLists)[LcurRL + i];
      (*rkLists)[LcurRL + i] = aux;
    }
  }

  void kernelCartProd(uint32_t rk, omp_lock_t* locks) {
    uint32_t set[k];
    set[0] = rk;

    uint32_t Li = l * rk;
    uint32_t imgNei, index;

    for (int j = 1; j < k; j++) {
      imgNei = (*rkLists)[Li + j];
      set[j] = imgNei;
      
      omp_set_lock(&locks[imgNei]);
      index = l * imgNei + nRef[imgNei];
      imgRef[index] = rk;
      posRef[index] = k - j;
      nRef[imgNei]++;
      omp_unset_lock(&locks[imgNei]);
    }

    uint32_t idx, idy, Nidx;
    uint32_t score;

    for (int j = 0; j < k; j++) {
      idx = set[j];
      Nidx = n * idx;
      
      omp_set_lock(&locks[idx]);
      for (int l = 0; l < k; l++) {
        idy = set[l];
        score = ((k - (j + 1) + 1) * (k - (l + 1) + 1));
        // Symmetric update optimization: just double the score and update only row 'idx'
        // This covers both [idx][idy] and [idy][idx] because the loop covers all pairs
        (*matrix)[Nidx + idy] += 2 * score;
      }
      omp_unset_lock(&locks[idx]);
    }
  }

  void kernelReverseCartProd(uint32_t rk, omp_lock_t* locks) {
    uint32_t size = nRef[rk];
    uint32_t Li = l * rk;
    uint32_t idx, idy, Nidx, index, index2;
    uint32_t score;

    for (int j = 0; j < size; j++) {
      index = Li + j;
      idx = imgRef[index];
      Nidx = n * idx;
      
      omp_set_lock(&locks[idx]);
      for (int l = 0; l < size; l++) {
        index2 = Li + l;
        idy = imgRef[index2];
        score = static_cast<uint32_t>(posRef[index]) * static_cast<uint32_t>(posRef[index2]);
        (*matrix)[Nidx + idy] += 2 * score; // Same 2x optimization logic
      }
      omp_unset_lock(&locks[idx]);
    }
  }

  // Method-specific attributes
  uint32_t k;  // Number of nearest neighbors
  uint32_t t;  // Number of iterations

  // Lists and references
  std::vector<uint32_t> imgRef;
  std::vector<uint32_t> posRef;
  std::vector<uint32_t> nRef;

  std::unique_ptr<VirtualArray<uint32_t>> matrix;

  static MethodRegister<Cprr> signInMethod;
};

#endif  // CPRR_HPP