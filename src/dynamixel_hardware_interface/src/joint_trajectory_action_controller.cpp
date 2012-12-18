/*
  Copyright (c) 2011, Antons Rebguns <email>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
  * Neither the name of the <organization> nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY Antons Rebguns <email> ''AS IS'' AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL Antons Rebguns <email> BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <sstream>
#include <string>
#include <vector>
#include <XmlRpcValue.h>

#include <dynamixel_hardware_interface/dynamixel_const.h>
#include <dynamixel_hardware_interface/dynamixel_io.h>

#include <dynamixel_hardware_interface/single_joint_controller.h>
#include <dynamixel_hardware_interface/multi_joint_controller.h>
#include <dynamixel_hardware_interface/joint_trajectory_action_controller.h>
#include <dynamixel_hardware_interface/JointState.h>

#include <dynamixel_hardware_interface/SetComplianceMargin.h>
#include <dynamixel_hardware_interface/SetComplianceSlope.h>

#include <ros/ros.h>
#include <pluginlib/class_list_macros.h>
#include <trajectory_msgs/JointTrajectory.h>
#include <control_msgs/FollowJointTrajectoryAction.h>

PLUGINLIB_DECLARE_CLASS(dynamixel_hardware_interface,
                        JointTrajectoryActionController,
                        controller::JointTrajectoryActionController,
                        controller::MultiJointController)

namespace controller
{

JointTrajectoryActionController::JointTrajectoryActionController()
{
  terminate_ = false;
}

JointTrajectoryActionController::~JointTrajectoryActionController()
{
}

bool JointTrajectoryActionController::initialize(std::string name, std::vector<SingleJointController*> deps)
{
  // Load the multi joint controller that this class inherits from. This loads the list of joint_names_
  if (!MultiJointController::initialize(name, deps))
  {
    return false;
  }

  update_rate_ = 1000;
  state_update_rate_ = 50;

  std::string prefix = "joint_trajectory_action_node/constraints/";

  c_nh_.param<double>(prefix + "goal_time", goal_time_constraint_, 0.0);
  c_nh_.param<double>(prefix + "stopped_velocity_tolerance", stopped_velocity_tolerance_, 0.01);
  c_nh_.param<double>("joint_trajectory_action_node/min_velocity", min_velocity_, 0.1);

  goal_constraints_.resize(num_joints_);
  trajectory_constraints_.resize(num_joints_);

  for (size_t i = 0; i < num_joints_; ++i)
  {
    c_nh_.param<double>(prefix + joint_names_[i] + "/goal", goal_constraints_[i], -1.0);
    c_nh_.param<double>(prefix + joint_names_[i] + "/trajectory", trajectory_constraints_[i], -1.0);
  }

  msg_.joint_names = joint_names_;
  msg_.desired.positions.resize(num_joints_);
  msg_.desired.velocities.resize(num_joints_);
  msg_.desired.accelerations.resize(num_joints_);
  msg_.actual.positions.resize(num_joints_);
  msg_.actual.velocities.resize(num_joints_);
  msg_.actual.accelerations.resize(num_joints_);
  msg_.error.positions.resize(num_joints_);
  msg_.error.velocities.resize(num_joints_);
  msg_.error.accelerations.resize(num_joints_);

  return true;
}

void JointTrajectoryActionController::start()
{
  // Subscribe to the set compliance slope and margin
  /*
    while (ros::ok() &&
    !ros::service::waitForService(controller_manager_name_ + "/set_compliance_slope", ros::Duration(5.0))  &&
    ++attempts < max_attempts)
    {
    ROS_INFO_STREAM("Waiting for service " << controller_manager_name_ + "/set_compliance_slope" << " to come up");
    }
    for( std::vector<std::string>::const_iterator joint_it = joint_names_.begin();
    joint_it < joint_names_.end(); ++joint_it )
    {
    set_compliance_slope_ = c_nh_.serviceClient<dynamixel_hardware_interface::SetComplianceSlope>
    (*joint_it + "/start_controller", true);
    set_compliance_margin_ = c_nh_.serviceClient<dynamixel_hardware_interface::SetComplianceSlope>
    (name_ + "/start_controller", true);
    }
  */



  command_sub_ = c_nh_.subscribe("command", 50, &JointTrajectoryActionController::processCommand, this);
  state_pub_ = c_nh_.advertise<control_msgs::FollowJointTrajectoryFeedback>("state", 50);

  action_server_.reset(new FJTAS(c_nh_, "follow_joint_trajectory",
                                 boost::bind(&JointTrajectoryActionController::processFollowTrajectory, this, _1),
                                 false));
  action_server_->start();
  feedback_thread_ = new boost::thread(boost::bind(&JointTrajectoryActionController::updateState, this));
}

void JointTrajectoryActionController::stop()
{
  {
    boost::mutex::scoped_lock terminate_lock(terminate_mutex_);
    terminate_ = true;
  }

  feedback_thread_->join();
  delete feedback_thread_;

  command_sub_.shutdown();
  state_pub_.shutdown();
  action_server_->shutdown();
}

void JointTrajectoryActionController::processCommand(const trajectory_msgs::JointTrajectoryConstPtr& msg)
{
  if (action_server_->isActive())
  {
    action_server_->setPreempted();
  }

  while (action_server_->isActive())
  {
    ros::Duration(0.01).sleep();
  }

  processTrajectory(*msg, false);
}

// This is what MoveIt is sending out
void JointTrajectoryActionController::processFollowTrajectory(const control_msgs::FollowJointTrajectoryGoalConstPtr& goal)
{
  processTrajectory(goal->trajectory, true);
}

void JointTrajectoryActionController::updateState()
{
  ros::Rate rate(state_update_rate_);

  while (nh_.ok())
  {
    {
      boost::mutex::scoped_lock terminate_lock(terminate_mutex_);
      if (terminate_) { break; }
    }

    msg_.header.stamp = ros::Time::now();

    for (size_t j = 0; j < joint_names_.size(); ++j)
    {
      const dynamixel_hardware_interface::JointState* state = joint_states_[joint_names_[j]];
      msg_.desired.positions[j] = state->target_position;
      msg_.desired.velocities[j] = std::abs(state->target_velocity);
      msg_.actual.positions[j] = state->position;
      msg_.actual.velocities[j] = std::abs(state->velocity);
      msg_.error.positions[j] = msg_.actual.positions[j] - msg_.desired.positions[j];
      msg_.error.velocities[j] = msg_.actual.velocities[j] - msg_.desired.velocities[j];
    }

    state_pub_.publish(msg_);

    rate.sleep();
  }
}

void JointTrajectoryActionController::processTrajectory(const trajectory_msgs::JointTrajectory& traj, bool is_action)
{
  control_msgs::FollowJointTrajectoryResult traj_result;
  std::string error_msg;

  int num_points = traj.points.size();

  ROS_DEBUG("Received trajectory with %d points", num_points);

  // Maps from an index in joints_ to an index in the msg
  std::vector<int> lookup(num_joints_, -1);

  // Check that all the joints in the trajectory exist in this multiDOF controller
  for (size_t j = 0; j < num_joints_; ++j)
  {
    for (size_t k = 0; k < traj.joint_names.size(); ++k)
    {
      if (traj.joint_names[k] == joint_names_[j])
      {
        lookup[j] = k;
        break;
      }
    }

    if (lookup[j] == -1)
    {
      traj_result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_JOINTS;
      error_msg = "Unable to locate joint " + joint_names_[j] + " in the commanded trajectory";
      ROS_ERROR("%s", error_msg.c_str());
      if (is_action) { action_server_->setAborted(traj_result, error_msg.c_str()); }
      return;
    }
  }

  // Lower the compliance margin and slope now then raise them back up after trajectory is completed
  // TODO: move this to configuration file
  int traj_compliance_margin = 1;
  int traj_compliance_slope = 90;
  MultiJointController::setAllComplianceMarginSlope( traj_compliance_margin, traj_compliance_slope );

  
  // find out the duration of each segment in the trajectory
  std::vector<double> durations;
  durations.resize(num_points);

  durations[0] = traj.points[0].time_from_start.toSec();
  double trajectory_duration = durations[0];

  for (int i = 1; i < num_points; ++i)
  {
    durations[i] = (traj.points[i].time_from_start - traj.points[i-1].time_from_start).toSec();
    trajectory_duration += durations[i];
    ROS_DEBUG("tpi: %f, tpi-1: %f", traj.points[i].time_from_start.toSec(), traj.points[i-1].time_from_start.toSec());
    ROS_DEBUG("i: %d, duration: %f, total: %f", i, durations[i], trajectory_duration);
  }

  if (traj.points[0].positions.empty())
  {
    traj_result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
    error_msg = "First point of trajectory has no positions";
    ROS_ERROR("%s", error_msg.c_str());
    if (is_action)
    {
      action_server_->setAborted(traj_result, error_msg);
    }
    return;
  }

  std::vector<Segment> trajectory;
  ros::Time time = ros::Time::now() + ros::Duration(0.01);

  for (int i = 0; i < num_points; ++i)
  {
    const trajectory_msgs::JointTrajectoryPoint point = traj.points[i];
    Segment seg;

    if (traj.header.stamp == ros::Time(0.0))
    {
      seg.start_time = (time + point.time_from_start).toSec() - durations[i];
    }
    else
    {
      seg.start_time = (traj.header.stamp + point.time_from_start).toSec() - durations[i];
    }

    seg.duration = durations[i];

    // Checks that the incoming segment has the right number of elements.
    if (!point.velocities.empty() && point.velocities.size() != num_joints_)
    {
      traj_result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
      error_msg = "Command point " + boost::lexical_cast<std::string>(i) + " has " + boost::lexical_cast<std::string>(point.velocities.size()) + " elements for the velocities, expecting " + boost::lexical_cast<std::string>(num_joints_);
      ROS_ERROR("%s", error_msg.c_str());
      if (is_action)
      {
        action_server_->setAborted(traj_result, error_msg);
      }
      return;
    }

    if (!point.positions.empty() && point.positions.size() != num_joints_)
    {
      traj_result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
      error_msg = "Command point " + boost::lexical_cast<std::string>(i) + " has " + boost::lexical_cast<std::string>(point.positions.size()) + " elements for the positions, expecting " + boost::lexical_cast<std::string>(num_joints_);
      ROS_ERROR("%s", error_msg.c_str());
      if (is_action)
      {
        action_server_->setAborted(traj_result, error_msg);
      }
      return;
    }

    seg.velocities.resize(num_joints_);
    seg.positions.resize(num_joints_);

    for (size_t j = 0; j < num_joints_; ++j)
    {
      seg.velocities[j] = point.velocities[lookup[j]];
      seg.positions[j] = point.positions[lookup[j]];
    }

    trajectory.push_back(seg);
  }

  ROS_INFO("Trajectory start requested at %.3lf, waiting...", traj.header.stamp.toSec());
  ros::Time::sleepUntil(traj.header.stamp);

  ros::Time end_time = traj.header.stamp + ros::Duration(trajectory_duration);
  std::vector<ros::Time> seg_end_times(num_points, ros::Time(0.0));

  for (int i = 0; i < num_points; ++i)
  {
    seg_end_times[i] = ros::Time(trajectory[i].start_time + durations[i]);
  }

  ROS_INFO("Trajectory start time is %.3lf, end time is %.3lf, total duration is %.3lf", time.toSec(), end_time.toSec(), trajectory_duration);

  trajectory_ = trajectory;
  ros::Time traj_start_time = ros::Time::now();
  ros::Rate rate(update_rate_);

  //------------------------------------------------------------------------------------------------
  // The main loop - sends motor commands once per for loop
  for (int traj_seg = 0; traj_seg < num_points; ++traj_seg)
  {
    ROS_DEBUG("Processing segment %d -------------------------------------------------", traj_seg);

    // first point in trajectories calculated by OMPL is current position with duration of 0 seconds, skip it
    if (durations[traj_seg] == 0.0)
    {
      ROS_DEBUG("Skipping segment %d because duration is 0", traj_seg);
      continue;
    }

    // List of every port, and that port's corresponding commands for every motor
    std::map<std::string, std::vector<std::vector<int> > > multi_port_commands;

    // -----------------------------------------------------------------------------------------
    // Combine all the commands for every motor of every joint that is on the same port into one
    // "multi_port_commands"

    // Loop through every port in this multi joint controller
    for ( std::map<std::string, std::vector<std::string> >::const_iterator port_it =
            port_to_joints_.begin(); port_it != port_to_joints_.end(); ++port_it)
    {
      ROS_DEBUG_STREAM("Processing Port " << port_it->first );

      // List of all commands for a particular port
      std::vector<std::vector<int> > port_motor_commands;

      // Loop through every joint on the port
      for ( std::vector<std::string>::const_iterator joint_it = port_it->second.begin();
            joint_it != port_it->second.end(); ++joint_it)
      {
        // Cache joint data
        int joint_idx = joint_to_idx_[*joint_it];

        // Get start position of this joint
        double start_position;
        if (traj_seg != 0)
        {
          start_position = trajectory[traj_seg-1].positions[joint_idx];
        }
        else
        {
          start_position = joint_states_[*joint_it]->position;
        }

        // Calculate desired values
        double desired_position = trajectory[traj_seg].positions[joint_idx];
        double desired_velocity = std::max<double>(min_velocity_,
                                                   std::abs(desired_position - start_position) /
                                                   durations[traj_seg]);

        ROS_DEBUG("\tstart_position: %f, duration: %f", start_position, durations[traj_seg]);
        ROS_DEBUG("\tport: %s, joint: %s, dpos: %f, dvel: %f", port_it->first.c_str(),
                  joint_it->c_str(), desired_position, desired_velocity);

        // Check that desired_veclocity is not too high, e.g. the position difference not too large
        if( desired_velocity > joint_to_controller_[*joint_it]->getMaxVelocity() )
        {
          traj_result.error_code = control_msgs::FollowJointTrajectoryResult::PATH_TOLERANCE_VIOLATED;
          error_msg = "Invalid joint trajectory: max velocity exceeded for joint " + *joint_it +
            " with a velocity of " + boost::lexical_cast<std::string>(desired_velocity) +
            " when the max velocity is set to " +
            boost::lexical_cast<std::string>(joint_to_controller_[*joint_it]->getMaxVelocity()) +
            ". On trajectory step " + boost::lexical_cast<std::string>(traj_seg);
          ROS_ERROR("%s", error_msg.c_str());
          if (is_action)
          {
            action_server_->setAborted(traj_result, error_msg);
          }
          return;
        }

        // Generate raw motor commands
        std::vector<std::vector<int> > joint_motor_commands =
          joint_to_controller_[*joint_it]->getRawMotorCommands(desired_position, desired_velocity);

        // Copy raw motor commands to port vector
        for (size_t i = 0; i < joint_motor_commands.size(); ++i)
        {
          port_motor_commands.push_back(joint_motor_commands[i]);
        }

        // Copy port vector to multi port vector
        multi_port_commands[port_it->first] = port_motor_commands;
      }
    }

    // Loop through every port and send it their raw commands
    for ( std::map<std::string, std::vector<std::vector<int> > >::const_iterator
            multi_port_commands_it = multi_port_commands.begin();
          multi_port_commands_it != multi_port_commands.end(); ++multi_port_commands_it)
    {
      port_to_io_[multi_port_commands_it->first]->setMultiPositionVelocity(multi_port_commands_it->second);
    }

    // Now wait for the next segment to be ready to go
    while (time < seg_end_times[traj_seg])
    {
      // check if new trajectory was received, if so abort old one by setting the desired state to current state
      if (is_action && action_server_->isPreemptRequested())
      {
        traj_result.error_code = control_msgs::FollowJointTrajectoryResult::SUCCESSFUL;
        error_msg = "New trajectory received. Aborting old trajectory.";

        std::map<std::string, std::vector<std::vector<int> > > multi_port_commands;

        std::map<std::string, std::vector<std::string> >::const_iterator port_it;
        std::vector<std::string>::const_iterator joint_it;

        for (port_it = port_to_joints_.begin(); port_it != port_to_joints_.end(); ++port_it)
        {
          std::vector<std::vector<int> > port_motor_commands;

          for (joint_it = port_it->second.begin(); joint_it != port_it->second.end(); ++joint_it)
          {
            std::string joint = *joint_it;

            double desired_position = joint_states_[joint]->position;
            double desired_velocity = joint_states_[joint]->velocity;

            std::vector<std::vector<int> > joint_motor_commands = joint_to_controller_[joint]->getRawMotorCommands(desired_position, desired_velocity);
            for (size_t i = 0; i < joint_motor_commands.size(); ++i)
            {
              port_motor_commands.push_back(joint_motor_commands[i]);
            }

            multi_port_commands[port_it->first] = port_motor_commands;
          }
        }

        std::map<std::string, std::vector<std::vector<int> > >::const_iterator multi_port_commands_it;
        for (multi_port_commands_it = multi_port_commands.begin(); multi_port_commands_it != multi_port_commands.end(); ++multi_port_commands_it)
        {
          port_to_io_[multi_port_commands_it->first]->setMultiPositionVelocity(multi_port_commands_it->second);
        }

        action_server_->setPreempted(traj_result, error_msg);
        ROS_WARN("%s", error_msg.c_str());
        return;
      }

      rate.sleep();
      time = ros::Time::now();
    }

    // Verifies trajectory constraints
    for (size_t j = 0; j < joint_names_.size(); ++j)
    {
      if (trajectory_constraints_[j] > 0.0 && msg_.error.positions[j] > trajectory_constraints_[j])
      {
        traj_result.error_code = control_msgs::FollowJointTrajectoryResult::PATH_TOLERANCE_VIOLATED;
        error_msg = "Unsatisfied position constraint for " + joint_names_[j] +
          " trajectory point " + boost::lexical_cast<std::string>(traj_seg) +
          ", " + boost::lexical_cast<std::string>(msg_.error.positions[j]) +
          " is larger than " + boost::lexical_cast<std::string>(trajectory_constraints_[j]);
        ROS_ERROR("%s", error_msg.c_str());
        if (is_action) { action_server_->setAborted(traj_result, error_msg); }
        return;
      }
    }
  } // end of the main loop

  // Raise the compliance margin and slope
  // TODO: move this to configuration file
  traj_compliance_margin = 1;
  traj_compliance_slope = 30;
  setAllComplianceMarginSlope( traj_compliance_margin, traj_compliance_slope );

  // let motors roll for specified amount of time
  ros::Duration(goal_time_constraint_).sleep();

  // Check if all motors are within their goal constraints
  for (size_t i = 0; i < num_joints_; ++i)
  {
    if (goal_constraints_[i] > 0 && std::abs(msg_.error.positions[i]) > goal_constraints_[i])
    {
      traj_result.error_code = control_msgs::FollowJointTrajectoryResult::GOAL_TOLERANCE_VIOLATED;
      error_msg = "Aborting because " + joint_names_[i] +
        " joint wound up outside the goal constraints. The position error " +
        boost::lexical_cast<std::string>(msg_.error.positions[i]) +
        " is larger than the goal constraints " + boost::lexical_cast<std::string>(goal_constraints_[i]);
      ROS_ERROR("%s", error_msg.c_str());
      if (is_action)
      {
        action_server_->setAborted(traj_result, error_msg);
      }
      return;
    }
  }

  traj_result.error_code = control_msgs::FollowJointTrajectoryResult::SUCCESSFUL;
  error_msg = "Trajectory execution successfully completed";
  ROS_INFO("%s", error_msg.c_str());
  action_server_->setSucceeded(traj_result, error_msg);
}

}