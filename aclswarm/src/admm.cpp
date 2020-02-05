/**
 * @file admm.h
 * @brief Wrapper for ADMM-based gain design generated by MATLAB
 * @author Parker Lusk <parkerclusk@gmail.com>
 * @date 29 Jan 2020
 */

#include "aclswarm/admm.h"

namespace acl {
namespace aclswarm {

ADMM::ADMM(const size_t n)
{
  ADMMGainDesign3D_initialize();

  // allocate memory for a 2D array of dynamic size. Used to store ADMM result.
  emxInit_real_T(&Aopt_, 2);
}

// ----------------------------------------------------------------------------

ADMM::~ADMM()
{
  // clean up our mess
  emxFree_real_T(&Aopt_);
  ADMMGainDesign3D_terminate();
}

// ----------------------------------------------------------------------------

GainMat ADMM::calculateFormationGains(const PtsMat& p, const AdjMat& adjmat)
{
  // copy the incoming memory so we can use it. Not super efficient.
  Eigen::Matrix<double, 3, Eigen::Dynamic> pp = p.transpose();
  Eigen::MatrixXd a = adjmat.cast<double>();

  // Map the memory into the MATLAB emx structure for use
  emxArray_real_T * Qs = emxCreateWrapper_real_T(pp.data(), pp.rows(), pp.cols());
  emxArray_real_T * adj = emxCreateWrapper_real_T(a.data(), a.rows(), a.cols());

  ADMMGainDesign3D(Qs, adj, Aopt_);

  emxFree_real_T(&adj);
  emxFree_real_T(&Qs);

  Eigen::Map<GainMat> A(Aopt_->data, Aopt_->size[0], Aopt_->size[1]);

  // kill values that are smaller in magnitude than eps
  return (1e-10 < A.array().abs()).select(A, 0.0);
}

// ----------------------------------------------------------------------------
// Private Methods
// ----------------------------------------------------------------------------

void ADMM::emxPrint(emxArray_real_T * emx)
{
  std::cout << emx->size[0] << "x" << emx->size[1] << std::endl;

  for (size_t i=0; i<emx->size[0]; ++i) {
   for (size_t j=0; j<emx->size[1]; ++j) {
     std::cout << std::setprecision(4) << std::setfill(' ') << std::setw(10)
       << emx->data[i + emx->size[0] * j] << " ";
   }
   std::cout << std::endl;
  }
}

} // ns aclswarm
} // ns acl