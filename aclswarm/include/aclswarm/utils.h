/**
 * @file utils.h
 * @brief Utilities for swarm
 * @author Parker Lusk <parkerclusk@gmail.com>
 * @date 15 Oct 2019
 */

#pragma once

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include <ros/ros.h>

#include <std_msgs/UInt8MultiArray.h>
#include <std_msgs/Float32MultiArray.h>

namespace acl {
namespace aclswarm {

using GainMat = Eigen::MatrixXd;
using AdjMat = Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic>;
using AssignmentMap = std::vector<uint8_t>;

namespace utils {

/**
 * @brief      Loads information about vehicles from rosparam server.
 *
 * @param      name      The name of this vehicle
 * @param      id        The id of this vehicle (idx in vehicles list)
 * @param      vehicles  A list of all vehicles in swarm
 *
 * @return     True if successful.
 */
static bool loadVehicleInfo(std::string& name, uint8_t& vehid,
                            std::vector<std::string>& vehicles)
{
  //
  // Resolve this vehicle's index using this node's ROS ns as the name
  //

  // get the name of this vehicle from the namespace and make uppercase.
  std::string ns = ros::this_node::getNamespace();
  if (ns == "/") {
    ROS_ERROR("Namespace cannot be empty");
    return false;
  }
  name = ns.substr(ns.find_first_not_of('/'));

  // get list of vehicle names, ordered by vehicle index.
  ros::param::get("/vehs", vehicles);
  const auto& it = std::find(vehicles.begin(), vehicles.end(), name);
  if (it == vehicles.end()) {
    ROS_ERROR_STREAM("Could not find myself ('" << name << "') in vehlist");
    return false;
  }

  // deduce vehicle id
  vehid = std::distance(vehicles.begin(), it);

  ROS_WARN_STREAM("Loading '" << name << "' as index " << vehid);

  return true;
}

// ----------------------------------------------------------------------------

/**
 * @brief      Converts adjmat msg to Eigen
 *
 * @param[in]  msg   The adjmat multiarray
 *
 * @return     The eigen matrix
 */
static AdjMat decodeAdjMat(const std_msgs::UInt8MultiArray& msg)
{
  const int rows = msg.layout.dim[0].size;
  const int cols = msg.layout.dim[1].size;

  AdjMat A = AdjMat::Zero(rows, cols);

  for (size_t i=0; i<rows; ++i) {
    for (size_t j=0; j<cols; ++j) {
      A(i,j) = msg.data[msg.layout.data_offset + msg.layout.dim[1].stride * i + j];
    }
  }

  return A;
}

// ----------------------------------------------------------------------------

/**
 * @brief      Converts gain msg to Eigen
 *
 * @param[in]  msg   The gains multiarray
 *
 * @return     The eigen matrix
 */
static GainMat decodeGainMat(const std_msgs::Float32MultiArray& msg)
{
  const int rows = msg.layout.dim[0].size;
  const int cols = msg.layout.dim[1].size;

  GainMat A = GainMat::Zero(rows, cols);

  for (size_t i=0; i<rows; ++i) {
    for (size_t j=0; j<cols; ++j) {
      A(i,j) = msg.data[msg.layout.data_offset + msg.layout.dim[1].stride * i + j];
    }
  }

  return A;
}

// ----------------------------------------------------------------------------

/**
 * @brief      Given an arbitrary numeric vector, return the indices
 *             that would sort the original vector---the sort indices.
 *             In other words, the sort indices describe where each
 *             element of sort(v) originally was in v.
 *
 *                        (idx  0   1  2  3)
 *             v            = [36, 12, 3, 9]
 *             sort(v)      = [3, 9, 12, 36]
 *             ---
 *             sort indices = [2, 3, 1, 0]
 *
 * @param[in]  v
 *
 * @return     The sort indices that would sort the original vector
 */
template<typename T>
static std::vector<T> sortIndices(const std::vector<T>& v)
{
  // initialize original index locations
  std::vector<T> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::sort(idx.begin(), idx.end(),
       [&v](T i1, T i2) {return v[i1] < v[i2];});

  return idx;
}

// ----------------------------------------------------------------------------

/**
 * @brief      Invert the given assignment mapping
 *
 * @param[in]  a  The current assignment
 *
 * @return     The inverse assignment
 */
static AssignmentMap invertAssignment(const AssignmentMap& a)
{
  // The sort indices of the assignment describe the inverse assignment.
  return sortIndices(a);
}

// ----------------------------------------------------------------------------

/**
 * @brief      Clamp function to saturate between a low and high value
 *
 * @param[in]  val      The value to clamp
 * @param[in]  lower    The lower bound
 * @param[in]  upper    The upper bound
 * @param      clamped  Wether or not the value was clamped
 *
 * @tparam     T        The template type
 *
 * @return     The clamped value
 */
template<typename T>
static T clamp(const T& val, const T& lower, const T& upper, bool& clamped) {
  if (val < lower) {
    clamped = true;
    return lower;
  }

  if (val > upper) {
    clamped = true;
    return upper;
  }

  clamped = false;
  return val;
}

// ----------------------------------------------------------------------------

/**
 * @brief      Specialization of clamp without clamp indicator
 */
template<typename T>
static T clamp(const T& val, const T& lower, const T& upper) {
  bool clamped = false;
  return clamp(val, lower, upper, clamped);
}

} // ns utils
} // ns aclswarm
} // ns acl
