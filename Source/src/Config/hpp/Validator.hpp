#ifndef VALIDATOR_HPP
#define VALIDATOR_HPP

#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Type.hpp"

class Validator {
 public:
  Validator() = default;

  static const std::map<std::string, Type::Param>& getRules() {
    static std::map<std::string, Type::Param> rules = {
        // General settings
        {"UDL_TASK", {"UDL", {"UDL", "FUSION"}, true}},
        {"UDL_METHOD", {"NONE", {"NONE", "CPRR", "CPRR_SPARSE", "RLRECOM", "RLSIM", "CONTEXTRR", "RECKNNGRAPH", "RKGRAPH", "CORGRAPH", "LHRR", "LHRR_SPARSE", "BFSTREE", "RDPAC", "RFE"}, true}},

        // Input settings
        {"SIZE_DATASET", {1400, {}, true}},
        {"INPUT_FILE_FORMAT", {"AUTO", {"AUTO", "MATRIX", "RK"}, true}},
        {"INPUT_MATRIX_TYPE", {"DIST", {"DIST", "SIM"}, true}},
        {"INPUT_RK_FORMAT", {"NUM", {"NUM", "STR"}, true}},
        {"MATRIX_TO_RK_SORTING", {"HEAP", {"HEAP", "INSERTION"}, true}},
        {"NUM_INPUT_FUSION_FILES", {0, {}, false}},
        {"INPUT_FILE", {"input.txt", {}, false}},
        {"INPUT_FILE_LIST", {"list.txt", {}, false}},
        {"INPUT_FILE_CLASSES", {"classes.txt", {}, false}},
        {"INPUT_IMAGES_PATH", {"images/", {}, false}},

        // Output settings
        {"OUTPUT_FILE", {true, {}, false}},
        {"OUTPUT_FILE_FORMAT", {"RK", {"RK", "MATRIX"}, false}},
        {"OUTPUT_MATRIX_TYPE", {"DIST", {"DIST", "SIM"}, false}},
        {"OUTPUT_RK_FORMAT", {"NUM", {"NUM", "STR", "HTML", "ALL"}, false}},
        {"OUTPUT_FILE_PATH", {"output", {}, false}},
        {"OUTPUT_HTML_RK_PER_FILE", {1, {}, false}},
        {"OUTPUT_HTML_RK_SIZE", {20, {}, false}},
        {"OUTPUT_HTML_RK_COLORS", {true, {}, false}},
        {"OUTPUT_HTML_RK_BEFORE_AFTER", {true, {}, false}},
        {"OUTPUT_LOG_FILE_PATH", {"log.txt", {}, false}},

        // Evaluation settings
        {"EFFICIENCY_EVAL", {true, {}, false}},
        {"EFFECTIVENESS_EVAL", {true, {}, false}},
        {"EFFECTIVENESS_COMPUTE_PRECISIONS", {true, {}, false}},
        {"EFFECTIVENESS_COMPUTE_MAP", {true, {}, false}},
        {"EFFECTIVENESS_COMPUTE_RECALL", {true, {}, false}},
        {"EFFECTIVENESS_RECALLS_TO_COMPUTE", {"4,5,10,20,40", {}, false}},
        {"EFFECTIVENESS_PRECISIONS_TO_COMPUTE", {"4,5,10,15,20,30,50,100", {}, false}},

        // BFSTREE Parameters
        {"PARAM_BFSTREE_L", {1400, {}, false}},
        {"PARAM_BFSTREE_K", {20, {}, false}},
        {"PARAM_BFSTREE_CORRELATION_METRIC", {"RBO", {"RBO"}, false}},

        // CONTEXTRR Parameters
        {"PARAM_CONTEXTRR_L", {25, {}, false}},
        {"PARAM_CONTEXTRR_K", {7, {}, false}},
        {"PARAM_CONTEXTRR_T", {5, {}, false}},
        {"PARAM_CONTEXTRR_NBYK", {1, {}, false}},
        {"PARAM_CONTEXTRR_OPTIMIZATIONS", {false, {}, false}},

        // CORGRAPH Parameters
        {"PARAM_CORGRAPH_L", {200, {}, false}},
        {"PARAM_CORGRAPH_K", {5, {}, false}},
        {"PARAM_CORGRAPH_THRESHOLD_START", {0.35, {}, false}},
        {"PARAM_CORGRAPH_THRESHOLD_END", {1.0, {}, false}},
        {"PARAM_CORGRAPH_THRESHOLD_INC", {0.005, {}, false}},
        {"PARAM_CORGRAPH_CORRELATION", {"PEARSON", {"PEARSON", "RBO"}, false}},

        // CPRR Parameters
        {"PARAM_CPRR_SPARSE_L", {400, {}, false}},
        {"PARAM_CPRR_SPARSE_K", {20, {}, false}},
        {"PARAM_CPRR_SPARSE_T", {2, {}, false}},

        // LHRR Parameters
        {"PARAM_LHRR_SPARSE_L", {1400, {}, false}},
        {"PARAM_LHRR_SPARSE_K", {18, {}, false}},
        {"PARAM_LHRR_SPARSE_T", {2, {}, false}},
        {"PARAM_LHRR_SPARSE_THREADS", {1, {}, false}},

        // CPRR Parameters
        {"PARAM_CPRR_EIGEN_L", {400, {}, false}},
        {"PARAM_CPRR_EIGEN_K", {20, {}, false}},
        {"PARAM_CPRR_EIGEN_T", {2, {}, false}},

        // LHRR Parameters
        {"PARAM_LHRR_EIGEN_L", {1400, {}, false}},
        {"PARAM_LHRR_EIGEN_K", {18, {}, false}},
        {"PARAM_LHRR_EIGEN_T", {2, {}, false}},
        
        // CPRR Parameters
        {"PARAM_CPRR_BLAS_L", {400, {}, false}},
        {"PARAM_CPRR_BLAS_K", {20, {}, false}},
        {"PARAM_CPRR_BLAS_T", {2, {}, false}},

        // LHRR Parameters
        {"PARAM_LHRR_BLAS_L", {1400, {}, false}},
        {"PARAM_LHRR_BLAS_K", {18, {}, false}},
        {"PARAM_LHRR_BLAS_T", {2, {}, false}},

        // CPRR Parameters
        {"PARAM_CPRR_MKL_L", {400, {}, false}},
        {"PARAM_CPRR_MKL_K", {20, {}, false}},
        {"PARAM_CPRR_MKL_T", {2, {}, false}},

        // LHRR Parameters
        {"PARAM_LHRR_MKL_L", {1400, {}, false}},
        {"PARAM_LHRR_MKL_K", {18, {}, false}},
        {"PARAM_LHRR_MKL_T", {2, {}, false}},

        // CPRR Parameters
        {"PARAM_CPRR_L", {400, {}, false}},
        {"PARAM_CPRR_K", {20, {}, false}},
        {"PARAM_CPRR_T", {2, {}, false}},

        // LHRR Parameters
        {"PARAM_LHRR_L", {1400, {}, false}},
        {"PARAM_LHRR_K", {18, {}, false}},
        {"PARAM_LHRR_T", {2, {}, false}},

        // NONE Parameters
        {"PARAM_NONE_L", {400, {}, false}},

        // RDPAC Parameters
        {"PARAM_RDPAC_L", {400, {}, false}},
        {"PARAM_RDPAC_L_MULT", {2.0, {}, false}},
        {"PARAM_RDPAC_P", {0.75, {}, false}},
        {"PARAM_RDPAC_PL", {0.97, {}, false}},
        {"PARAM_RDPAC_K_START", {1, {}, false}},
        {"PARAM_RDPAC_K_END", {15, {}, false}},
        {"PARAM_RDPAC_K_INC", {1, {}, false}},

        // BFSTree Parameters (only the RBO metric is ported; see
        // Methods/hpp/BFSTree.hpp). PARAM_BFSTREE_B is the F5/F7 row batch:
        // measured table 256 for L=1400, 1024 for L=100.
        {"PARAM_BFSTREE_L", {1400, {}, false}},
        {"PARAM_BFSTREE_K", {20, {}, false}},
        {"PARAM_BFSTREE_B", {256, {}, false}},
        {"PARAM_BFSTREE_CORRELATION_METRIC",
         {std::string("RBO"), {std::string("RBO")}, false}},

        // RECKNNGRAPH Parameters
        {"PARAM_RECKNNGRAPH_L", {200, {}, false}},
        {"PARAM_RECKNNGRAPH_K", {15, {}, false}},
        {"PARAM_RECKNNGRAPH_EPSILON", {0.0125, {}, false}},

        // RFE Parameters
        {"PARAM_RFE_K", {20, {}, false}},
        {"PARAM_RFE_T", {2, {}, false}},
        {"PARAM_RFE_L", {1400, {}, false}},
        {"PARAM_RFE_PA", {0.1, {}, false}},
        {"PARAM_RFE_TH_CC", {0.0, {}, false}},
        {"PARAM_RFE_PERFORM_CCS", {false, {}, false}},
        {"PARAM_RFE_RERANK_BY_EMB", {false, {}, false}},
        {"PARAM_RFE_EXPORT_EMBEDDINGS", {false, {}, false}},
        {"PARAM_RFE_EMBEDDINGS_PATH", {"", {}, false}},
        {"PARAM_RFE_CCS_PATH", {"", {}, false}},

        // RKGRAPH Parameters
        {"PARAM_RKGRAPH_K", {20, {}, false}},
        {"PARAM_RKGRAPH_T", {1, {}, false}},
        {"PARAM_RKGRAPH_P", {0.95, {}, false}},
        {"PARAM_RKGRAPH_L", {100, {}, false}},

        // RLRECOM Parameters
        {"PARAM_RLRECOM_K", {8, {}, false}},
        {"PARAM_RLRECOM_L", {400, {}, false}},
        {"PARAM_RLRECOM_LAMBDA", {2.0, {}, false}},
        {"PARAM_RLRECOM_EPSILON", {0.0125, {}, false}},

        // RLSIM Parameters
        {"PARAM_RLSIM_TOPK", {15, {}, false}},
        {"PARAM_RLSIM_CK", {700, {}, false}},
        {"PARAM_RLSIM_T", {3, {}, false}},
        {"PARAM_RLSIM_METRIC", {"INTERSECTION", {"INTERSECTION", "RBO", "KENDALL_TAU", "SPEARMAN", "GOODMAN", "JACCARD", "JACCARD_K", "KENDALL_TAU_W"}, false}}};

    return rules;
  }
};

#endif
