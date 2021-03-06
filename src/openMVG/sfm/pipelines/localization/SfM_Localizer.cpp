
// Copyright (c) 2015 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/sfm/pipelines/localization/SfM_Localizer.hpp"
#include "openMVG/sfm/sfm_data_BA_ceres.hpp"

#include "openMVG/multiview/solver_resection_kernel.hpp"
#include "openMVG/multiview/solver_resection_p3p.hpp"
#include "openMVG/robust_estimation/robust_estimator_ACRansac.hpp"
#include "openMVG/robust_estimation/robust_estimator_ACRansacKernelAdaptator.hpp"

namespace openMVG {
namespace sfm {

struct ResectionSquaredResidualError {
  // Compute the residual of the projection distance(pt2D, Project(P,pt3D))
  // Return the squared error
  static double Error(const Mat34 & P, const Vec2 & pt2D, const Vec3 & pt3D){
    const Vec2 x = Project(P, pt3D);
    return (x - pt2D).squaredNorm();
  }
};

bool SfM_Localizer::Localize
(
  const Pair & image_size,
  const cameras::IntrinsicBase * optional_intrinsics,
  Image_Localizer_Match_Data & resection_data,
  geometry::Pose3 & pose
)
{
  // --
  // Compute the camera pose (resectioning)
  // --
  Mat34 P;
  resection_data.vec_inliers.clear();

  // Setup the admissible upper bound residual error
  const double dPrecision =
    resection_data.error_max == std::numeric_limits<double>::infinity() ?
    std::numeric_limits<double>::infinity() :
    Square(resection_data.error_max);

  size_t MINIMUM_SAMPLES = 0;
  const cameras::Pinhole_Intrinsic * pinhole_cam = dynamic_cast<const cameras::Pinhole_Intrinsic *>(optional_intrinsics);
  if (pinhole_cam == nullptr)
  {
    //--
    // Classic resection (try to compute the entire P matrix)
    typedef openMVG::resection::kernel::SixPointResectionSolver SolverType;
    MINIMUM_SAMPLES = SolverType::MINIMUM_SAMPLES;

    typedef openMVG::robust::ACKernelAdaptorResection<
      SolverType, ResectionSquaredResidualError, openMVG::robust::UnnormalizerResection, Mat34>
      KernelType;

    KernelType kernel(resection_data.pt2D, image_size.first, image_size.second,
      resection_data.pt3D);
    // Robust estimation of the Projection matrix and its precision
    const std::pair<double,double> ACRansacOut =
      openMVG::robust::ACRANSAC(kernel, resection_data.vec_inliers, resection_data.max_iteration, &P, dPrecision, true);
    // Update the upper bound precision of the model found by AC-RANSAC
    resection_data.error_max = ACRansacOut.first;
  }
  else
  {
    //--
    // Since K calibration matrix is known, compute only [R|t]
    typedef openMVG::euclidean_resection::P3PSolver SolverType;
    MINIMUM_SAMPLES = SolverType::MINIMUM_SAMPLES;

    typedef openMVG::robust::ACKernelAdaptorResection_K<
      SolverType, ResectionSquaredResidualError,
      openMVG::robust::UnnormalizerResection, Mat34>  KernelType;

    // since the intrinsics are known undistort the input 2d points
    //@fixe there is a lot of code redundancy in this solution; find better solution to
    // avoid code duplication when calling ACRansacOut
    if(pinhole_cam->have_disto())
    {
      const std::size_t numPts = resection_data.pt2D.cols();
      Mat pt2Dundistorted = Mat2X(2, numPts);
      for(std::size_t iPoint = 0; iPoint < numPts; ++iPoint)
      {
        pt2Dundistorted.col(iPoint) = pinhole_cam->get_ud_pixel(resection_data.pt2D.col(iPoint));
       }
      KernelType kernel = KernelType(pt2Dundistorted, resection_data.pt3D, pinhole_cam->K());
      // Robust estimation of the Projection matrix and its precision
      const std::pair<double,double> ACRansacOut =
        openMVG::robust::ACRANSAC(kernel, resection_data.vec_inliers, resection_data.max_iteration, &P, dPrecision, true);
      // Update the upper bound precision of the model found by AC-RANSAC
      resection_data.error_max = ACRansacOut.first;
    }
    else
    {
      // otherwise we just pass the input points
      KernelType kernel = KernelType(resection_data.pt2D, resection_data.pt3D, pinhole_cam->K());
      // Robust estimation of the Projection matrix and its precision
      const std::pair<double,double> ACRansacOut =
        openMVG::robust::ACRANSAC(kernel, resection_data.vec_inliers, resection_data.max_iteration, &P, dPrecision, true);
      // Update the upper bound precision of the model found by AC-RANSAC
      resection_data.error_max = ACRansacOut.first;
    }
  }

  // Test if the mode support some points (more than those required for estimation)
  const bool bResection = (resection_data.vec_inliers.size() > MINIMUM_SAMPLES * OPENMVG_MINIMUM_SAMPLES_COEF);
#ifdef WANTS_POPART_COUT
  if (!bResection) 
  {
    std::cout << "bResection is false";
    std::cout << " because resection_data.vec_inliers.size() = " << resection_data.vec_inliers.size();
    std::cout << " and MINIMUM_SAMPLES = " << MINIMUM_SAMPLES << std::endl;
  }
#endif

  if (bResection)
  {
    resection_data.projection_matrix = P;
    Mat3 K, R;
    Vec3 t;
    KRt_From_P(P, &K, &R, &t);
    pose = geometry::Pose3(R, -R.transpose() * t);
  }
#ifdef WANTS_POPART_COUT
  std::cout << "\n"
    << "-------------------------------" << "\n"
    << "-- Robust Resection " << "\n"
    << "-- Resection status: " << bResection << "\n"
    << "-- #Points used for Resection: " << resection_data.pt2D.cols() << "\n"
    << "-- #Points validated by robust Resection: " << resection_data.vec_inliers.size() << "\n"
    << "-- Threshold: " << resection_data.error_max << "\n"
    << "-------------------------------" << std::endl;
#endif
  return bResection;
}

bool SfM_Localizer::RefinePose
(
  cameras::IntrinsicBase * intrinsics,
  geometry::Pose3 & pose,
  const Image_Localizer_Match_Data & matching_data,
  bool b_refine_pose,
  bool b_refine_intrinsic
)
{
  // Setup a tiny SfM scene with the corresponding 2D-3D data
  SfM_Data sfm_data;
  // view
  sfm_data.views.insert( std::make_pair(0, std::make_shared<View>("",0, 0, 0)));
  // pose
  sfm_data.poses[0] = pose;
  // intrinsic (the shared_ptr does not take the ownership, will not release the input pointer)
  std::shared_ptr<cameras::IntrinsicBase> localIntrinsics(intrinsics->clone());
  sfm_data.intrinsics[0] = localIntrinsics;
  // structure data (2D-3D correspondences)
  for (size_t i = 0; i < matching_data.vec_inliers.size(); ++i)
  {
    const size_t idx = matching_data.vec_inliers[i];
    Landmark landmark;
    landmark.X = matching_data.pt3D.col(idx);
    landmark.obs[0] = Observation(matching_data.pt2D.col(idx), UndefinedIndexT);
    sfm_data.structure[i] = std::move(landmark);
  }

  Bundle_Adjustment_Ceres bundle_adjustment_obj;
  BA_Refine refineOptions = BA_REFINE_NONE;
  if(b_refine_pose)
    refineOptions |= BA_REFINE_ROTATION | BA_REFINE_TRANSLATION;
  if(b_refine_intrinsic)
    refineOptions |= BA_REFINE_INTRINSICS;

  const bool b_BA_Status = bundle_adjustment_obj.Adjust(sfm_data, refineOptions);
  if (!b_BA_Status)
    return false;

  pose = sfm_data.poses[0];
  if(b_refine_intrinsic)
    intrinsics->assign(*localIntrinsics);
  return true;
}

} // namespace sfm
} // namespace openMVG
