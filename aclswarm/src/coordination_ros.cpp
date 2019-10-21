/**
 * @file coordination_ros.cpp
 * @brief ROS wrapper for coordination components of aclswarm
 * @author Parker Lusk <parkerclusk@gmail.com>
 * @date 15 Oct 2019
 */

#include "aclswarm/coordination_ros.h"
#include "aclswarm/utils.h"

namespace acl {
namespace aclswarm {

CoordinationROS::CoordinationROS(const ros::NodeHandle nh,
                                  const ros::NodeHandle nhp)
: nh_(nh), nhp_(nhp)
{
  if (!utils::loadVehicleInfo(vehname_, vehid_, vehs_)) {
    ros::shutdown();
    return;
  }

  //
  // Load parameters
  //

  nhp_.param<double>("assignment_dt", assignment_dt_, 0.5);
  nhp_.param<double>("control_dt", control_dt_, 0.05);

  //
  // Timers for assignment and distributed control tasks
  //

  ros::NodeHandle nhQ(nh_);
  nhQ.setCallbackQueue(&task_queue_);

  tim_assignment_ = nhQ.createTimer(ros::Duration(assignment_dt_),
                                            &CoordinationROS::assignCb, this);
  tim_control_ = nhQ.createTimer(ros::Duration(control_dt_),
                                            &CoordinationROS::controlCb, this);

  // Prevent timer tasks from blocking each other
  constexpr int NUM_TASKS = 2; // there are only two timers
  spinner_ = std::unique_ptr<ros::AsyncSpinner>(
                            new ros::AsyncSpinner(NUM_TASKS, &task_queue_));

  //
  // Instantiate module objects for tasks
  //

  controller_.reset(new DistCntrl());
  assignment_.reset(new Assignment());

  //
  // ROS pub/sub communication
  //

  sub_formation_ = nh_.subscribe("formation", 1,
                                    &CoordinationROS::formationCb, this);
  sub_tracker_ = nh_.subscribe("vehicle_estimates", 1,
                                    &CoordinationROS::vehicleTrackerCb, this);

  pub_distcmd_ = nh_.advertise<geometry_msgs::Vector3Stamped>("distcmd", 1);

}

// ----------------------------------------------------------------------------

void CoordinationROS::spin()
{
  // We do not expect formations to be sent that frequently, so slow check.
  ros::Rate r(5);
  while (ros::ok()) {

    if (formation_received_) {
      // stop tasks
      spinner_->stop();

      // operator sent a new formation (pts and adj mat). we should:
      // 1. Reset CBAA / stop assignment module (thread?)
      // 2. Zero commands / stop control module (thread?)
      // 3. Load new formation into memory (from msg)
      // 4. Solve for gains (or load from msg)
      // 5. Start assignment module (thread?)
      // 6. Start control module (thread?)

      // allow downstream tasks to continue
      spinner_->start();
      formation_received_ = false;
    }

    r.sleep();
  }
}

// ----------------------------------------------------------------------------
// Private Methods
// ----------------------------------------------------------------------------

void CoordinationROS::formationCb(const aclswarm_msgs::FormationConstPtr& msg)
{
  formation_received_ = true;
}

// ----------------------------------------------------------------------------

void CoordinationROS::vehicleTrackerCb(
                  const aclswarm_msgs::VehicleEstimatesConstPtr& msg)
{

}

// ----------------------------------------------------------------------------

void CoordinationROS::assignCb(const ros::TimerEvent& event)
{
  
}

// ----------------------------------------------------------------------------

void CoordinationROS::controlCb(const ros::TimerEvent& event)
{
  controller_->compute();

  geometry_msgs::Vector3Stamped msg;
  msg.header.stamp = ros::Time::now();
  pub_distcmd_.publish(msg);
}

} // ms aclswarm
} // ns acl
