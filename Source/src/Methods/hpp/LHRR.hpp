#ifndef LHRR_HPP
#define LHRR_HPP

#include <math.h>
#include <omp.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Method.hpp"
#include "MethodFactory.hpp"

class LHRR : public Method {
 public:
  explicit LHRR() : matrix(VirtualArray<uint32_t>::create(n, n, "matrix.txt")), Method() {}

 protected:
  void preprocess() override {
    l = config.get<int>("PARAM_LHRR_L");
    k = config.get<int>("PARAM_LHRR_K");
    t = config.get<int>("PARAM_LHRR_T");
    if (l > n) {
      std::cout << "L can't be greater than N" << std::endl;
      std::cout << "Aborting..." << std::endl;
      exit(1);
    }
    if (k > l) {
      std::cout << "K can't be greater than L" << std::endl;
      std::cout << "Aborting..." << std::endl;
      exit(1);
    }
    dataProvider.setL(l);
    (*matrix).assign(0, n, n);
  }

  void execute() override {
    execFillPosMatrix();
    execSortRankedLists();

    int iteration = 1;
    while (iteration <= t) {
      std::cout << std::endl
                << "  -> Iteration " << iteration << std::endl;
      hyperGraphIteration();
      iteration++;
    }
  }

  void postprocess() override {
    clearAndRelease(hyperEdges);
    clearAndRelease(revHyperEdges);
    clearAndRelease(confid);
    clearAndRelease(tmpList);
    clearAndRelease(imgList);
    clearAndRelease(imgListRev);
    clearAndRelease(valList);
    clearAndRelease(valListRev);
    matrix.reset();
  }

 private:
  void initializeGraphDataStructures() {
    hyperEdges.clear();
    revHyperEdges.clear();
    confid.clear();

    hyperEdges.resize(n);
    revHyperEdges.resize(n);
    confid.resize(n);

    tmpList.clear();
    tmpList.resize(n);

    imgList.clear();
    imgListRev.clear();
    valList.clear();
    valListRev.clear();

    imgList.resize(n);
    imgListRev.resize(n);
    valList.resize(n);
    valListRev.resize(n);
  }

  void hyperGraphIteration() {
    initializeGraphDataStructures();
    loadHyperEdges();
    compressHE();
    loadRevHyperEdges();
    resetDB(1);
    computeCartesianProductHyperEdges();
    computeHyperEdgesSimilarities();
    computeReciprocalHyperEdgesSimilarities();
    computeDBBySimilarities();
  }

  void compressHE() {
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      // init structures
      std::vector<int> tmpImgList;
      std::vector<double> tmpValList;
      std::vector<float> tmpArr(n, 0.0f); // Replaced stack array with vector

      // fill array
      int j = 0;
      for (const int& img : imgList[i]) {
        if ((tmpArr[img] == 0) && (valList[i][j] != 0)) {
          tmpImgList.push_back(img);
        }
        tmpArr[img] += valList[i][j];
        j++;
      }

      // fill values
      for (const int& img : tmpImgList) {
        tmpValList.push_back(tmpArr[img]);
      }

      // bind new lists
      imgList[i] = tmpImgList;
      valList[i] = tmpValList;
    }
  }

  void computeCartesianProductHyperEdges() {
    std::cout << "\n\t + Cartesian Product Hyper Edges ...\n";

    // Allocate and initialize row locks
    omp_lock_t* row_locks = new omp_lock_t[n];
    for (int i = 0; i < n; i++) {
      omp_init_lock(&row_locks[i]);
    }

#pragma omp parallel for
    for (int img = 0; img < n; img++) {
      double conf = confid[img];
      auto curHEdge = hyperEdges[img];
      for (auto entry1 : curHEdge) {
        int img1 = entry1.first;
        double value1 = entry1.second;
        
        omp_set_lock(&row_locks[img1]);
        for (auto entry2 : curHEdge) {
          int img2 = entry2.first;
          double value2 = entry2.second;
          uint32_t index = ((uint32_t)n) * img1 + img2;
          double inc = conf * value1 * value2;
          
          if ((*matrix)[index] == 0) {
            tmpList[img1].push_back(img2);
            (*matrix)[index] = 1;
          }
          (*matrix)[index] += inc;
        }
        omp_unset_lock(&row_locks[img1]);
      }
    }

    // Destroy and free row locks
    for (int i = 0; i < n; i++) {
      omp_destroy_lock(&row_locks[i]);
    }
    delete[] row_locks;
  }

  void sortTmpList(int qimg) {
    std::vector<std::pair<int, float>> rkTmp;
    for (int i = 0; i < tmpList[qimg].size(); i++) {
      int img = tmpList[qimg][i];
      uint32_t index = ((uint32_t)n) * qimg + img;
      rkTmp.push_back(std::make_pair(img, (*matrix)[index]));
    }

    // Use parallel sort instead of manual sorting
    std::sort(std::execution::par_unseq, rkTmp.begin(), rkTmp.end(),
              [](const std::pair<int, float>& x, const std::pair<int, float>& y) {
                return x.second > y.second;
              });

    for (int i = 0; i < tmpList[qimg].size(); i++) {
      tmpList[qimg][i] = rkTmp[i].first;
    }
  }

  void computeHyperEdgesSimilarities() {
    std::cout << "\n\t + Similarity between Hyper Edges ...\n";
#pragma omp parallel for
    for (int qimg = 0; qimg < n; qimg++) {
      int j = 0;
      std::vector<float> tmpArr(n, 0.0f); // Replaced stack array
      
      for (const int& img : imgList[qimg]) {
        tmpArr[img] = valList[qimg][j];
        j++;
      }

      for (int j = 1; j < l; j++) {
        int img = (*rkLists)[l * qimg + j];

        double simValue = 0;
        int o = 0;
        for (const int& e : imgList[img]) {
          simValue += tmpArr[e] * valList[img][o];
          o++;
        }

        uint32_t index = ((uint32_t)n) * qimg + img;
        double curValue = (*matrix)[index];
        double newValue = curValue * simValue;

        (*matrix)[index] = newValue;
      }
    }
  }

  void computeReciprocalHyperEdgesSimilarities() {
    std::cout << "\n\t + Similarity between Reverse and Hyper Edges ...\n";
#pragma omp parallel for
    for (int qimg = 0; qimg < n; qimg++) {
      int j = 0;
      std::vector<float> tmpArr(n, 0.0f); // Replaced stack array
      
      for (const int& img : imgListRev[qimg]) {
        tmpArr[img] = valListRev[qimg][j];
        j++;
      }

      for (int j = 1; j < l; j++) {
        int img = (*rkLists)[l * qimg + j];

        double simValue = 0;
        int o = 0;
        for (const int& e : imgList[img]) {
          simValue += tmpArr[e] * valList[img][o];
          o++;
        }

        uint32_t index = ((uint32_t)n) * qimg + img;
        double curValue = (*matrix)[index];
        double newValue = curValue * simValue;
        (*matrix)[index] = newValue;
      }
    }
  }

  double multiplyMaps(std::vector<std::pair<int, double>>& he1, std::vector<std::pair<int, double>>& he2) {
    double acumValue = 0;
    for (auto entry1 : he1) {
      int img = entry1.first;
      int pos = searchPairByKey(img, he2);
      if (pos != -1) {
        double v1 = entry1.second;
        double v2 = he2[pos].second;
        acumValue += v1 * v2;
      }
    }
    return acumValue;
  }

  void computeDBBySimilarities() {
    std::cout << "\n\t + Computing New DB by Similarities ...\n";
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      sortTmpList(i);
    }

    execSortRankedLists();
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      joinRks(i);
    }
  }

  void resetDB(int value) {
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < l; j++) {
        uint32_t index = ((uint32_t)n) * i + (*rkLists)[l * i + j];
        (*matrix)[index] = value;
      }
    }
  }

  void loadHyperEdges() {
    std::cout << "\n\t + Creating HyperEdges ...\n";
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      createHyperEdge(i);
    }
  }

  void createHyperEdge(int img) {
    includeHyperEdgeValue(img, img, 1);

    for (int o = 0; o < k; o++) {
      int imgo = (*rkLists)[l * img + o];
      int poso = o + 1;
      for (int j = 0; j < k; j++) {
        int imgj = (*rkLists)[l * imgo + j];
        int posj = j + 1;
        double w = weightPosition(poso) * weightPosition(posj);
        includeHyperEdgeValue(img, imgj, w);
      }
    }

    // Use parallel sort
    std::sort(std::execution::par_unseq, hyperEdges[img].begin(), hyperEdges[img].end(),
              [](const std::pair<int, double>& x, const std::pair<int, double>& y) {
                return x.second > y.second;
              });

    // compute confidence
    for (int i = 0; i < k; i++) {
      confid[img] += hyperEdges[img][i].second;
    }
  }

  void loadRevHyperEdges() {
    std::cout << "\n\t + Creating Reverse HyperEdges ...\n";

    // Allocate and initialize row locks
    omp_lock_t* row_locks = new omp_lock_t[n];
    for (int i = 0; i < n; i++) {
      omp_init_lock(&row_locks[i]);
    }

#pragma omp parallel for
    for (int img = 0; img < n; img++) {
      for (auto entry : hyperEdges[img]) {
        int curImg = entry.first;
        double curValue = entry.second;

        omp_set_lock(&row_locks[curImg]);
        imgListRev[curImg].push_back(img);
        valListRev[curImg].push_back(curValue);
        omp_unset_lock(&row_locks[curImg]);
      }
    }

    // Destroy and free row locks
    for (int i = 0; i < n; i++) {
      omp_destroy_lock(&row_locks[i]);
    }
    delete[] row_locks;
  }

  double weightPosition(int pos) {
    double logValue = log(pos) / log(k);
    return (1.0 - logValue);
  }

  int searchPairByKey(int key, std::vector<std::pair<int, double>>& hyperEdge) {
    int i = 0;
    for (auto it : hyperEdge) {
      if (it.first == key) {
        return i;
      }
      i++;
    }
    return -1;
  }

  void includeHyperEdgeValue(int i, int j, double value) {
    double curValue = 0;
    int pos = searchPairByKey(j, hyperEdges[i]);
    if (pos != -1) {
      curValue = hyperEdges[i][pos].second;
      hyperEdges[i][pos].second = curValue + value;
    } else {
      hyperEdges[i].push_back(std::make_pair(j, value));
    }

    // add elements
    imgList[i].push_back(j);
    valList[i].push_back(value);
  }

  void execFillPosMatrix() {
    std::cout << "\n\t Fill Distance Matrix with Positions ... \n";

    // Allocate and initialize row locks
    omp_lock_t* row_locks = new omp_lock_t[n];
    for (int i = 0; i < n; i++) {
      omp_init_lock(&row_locks[i]);
    }

#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      kernelFillPosMatrix(i, row_locks);
    }

    // Destroy and free row locks
    for (int i = 0; i < n; i++) {
      omp_destroy_lock(&row_locks[i]);
    }
    delete[] row_locks;
  }

  void execSortRankedLists() {
    std::cout << "\n\t Sort Ranked Lists ... \n";
#pragma omp parallel for
    for (int i = 0; i < n; i++) {
      kernelSortRankedLists(i);
    }
  }

  void kernelFillPosMatrix(int rk, omp_lock_t* locks) {
    uint32_t lrk = ((uint32_t)l) * rk;
    uint32_t nrk = ((uint32_t)n) * rk;
    uint32_t img;

    for (int pos = 0; pos < l; pos++) {
      img = (*rkLists)[lrk + pos];
      
      omp_set_lock(&locks[rk]);
      (*matrix)[nrk + img] += l - pos;
      omp_unset_lock(&locks[rk]);

      omp_set_lock(&locks[img]); // Lock row img
      (*matrix)[n * img + rk] += l - pos;
      omp_unset_lock(&locks[img]);
    }
  }

  void kernelSortRankedLists(int rk) {
    uint32_t LcurRL = ((uint32_t)l) * rk;
    uint32_t cNcurRL = ((uint32_t)n) * rk;

    // Create vector of pairs for parallel sorting
    std::vector<std::pair<float, int>> sortData;
    sortData.reserve(l);

    uint32_t index;
    for (int j = 0; j < l; j++) {
      index = cNcurRL + (*rkLists)[LcurRL + j];
      sortData.push_back({(*matrix)[index], (*rkLists)[LcurRL + j]});
    }

    // Use parallel sort instead of insertion sort
    std::sort(std::execution::par_unseq, sortData.begin() + 1, sortData.end(),
              [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                return a.first > b.first;
              });

    // Copy back the sorted data
    for (int j = 1; j < l; j++) {
      (*rkLists)[LcurRL + j] = sortData[j].second;
    }

    // Setting query image at first position
    int i = 0;
    while (((*rkLists)[LcurRL + i] != rk) && (i < l)) {
      i++;
    }
    if (i > 0) {
      int aux = (*rkLists)[LcurRL + 0];
      (*rkLists)[LcurRL + 0] = (*rkLists)[LcurRL + i];
      (*rkLists)[LcurRL + i] = aux;
    }
  }

  void joinRks(int qimg) {
    if (tmpList[qimg].size() == 0) {
      return;
    }

    float a1[l];
    uint32_t LcurRL = ((uint32_t)l) * qimg;
    uint32_t cNcurRL = ((uint32_t)n) * qimg;

    float a2[tmpList[qimg].size()];

    uint32_t index;
    for (int j = 0; j < l; j++) {
      index = cNcurRL + (*rkLists)[LcurRL + j];
      a1[j] = (*matrix)[index];
    }

    std::vector<int> rkTmp;
    rkTmp.clear();

    for (int j = 0; j < tmpList[qimg].size(); j++) {
      index = cNcurRL + tmpList[qimg][j];
      a2[j] = (*matrix)[index];
      rkTmp.push_back(tmpList[qimg][j]);
    }

    int j = 0;
    for (int i = 1; i < l; i++) {
      if (a2[j] > a1[i]) {
        for (int o = l - 1; o > i; o--) {
          (*rkLists)[LcurRL + o] = (*rkLists)[LcurRL + o - 1];
        }
        (*rkLists)[LcurRL + i] = tmpList[qimg][j];
        j++;
        if (j >= tmpList[qimg].size()) {
          break;
        }
      }
    }
  }

  // method parameters
  int k;  // number of nearest neighbors
  int t;  // number of iterations

  std::vector<std::vector<std::pair<int, double>>> hyperEdges;
  std::vector<std::vector<std::pair<int, double>>> revHyperEdges;
  std::vector<double> confid;

  std::vector<std::vector<int>> tmpList;

  std::vector<std::vector<int>> imgList;
  std::vector<std::vector<int>> imgListRev;
  std::vector<std::vector<double>> valList;
  std::vector<std::vector<double>> valListRev;

  std::unique_ptr<VirtualArray<uint32_t>> matrix;

  static MethodRegister<LHRR> signInMethod;
};

#endif  // LHRR_HPP