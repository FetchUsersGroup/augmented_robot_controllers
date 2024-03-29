/*********************************************************************
 *  Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, Fetch Robotics Inc.
 *  Copyright (c) 2013, Unbounded Robotics Inc.
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Unbounded Robotics nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/*
 * Derived a bit from pr2_controllers/cartesian_pose_controller.cpp
 * Author: Michael Ferguson, Wim Meeussen, Hanjun Song
 */

#include <augmented_robot_controllers/cartesian_twist_avoid.h>
#include <pluginlib/class_list_macros.h>

#include <tf_conversions/tf_kdl.h>
#include <urdf/model.h>
#include <kdl_parser/kdl_parser.hpp>

PLUGINLIB_EXPORT_CLASS(augmented_robot_controllers::CartesianTwistAvoidController,
                       robot_controllers::Controller)

namespace augmented_robot_controllers
{
  CartesianTwistAvoidController::CartesianTwistAvoidController()
    : initialized_(false), is_active_(false)
  {
  }

  int CartesianTwistAvoidController::init(ros::NodeHandle& nh, ControllerManager* manager)
  {
    // We absolutely need access to the controller manager
    if (!manager)
    {
      initialized_ = false;
      return -1;
    }

    Controller::init(nh, manager);
    manager_ = manager;

    // Initialize KDL structures
    std::string tip_link, root_link;
    nh.param<std::string>("root_name", root_link, "torso_lift_link");
    nh.param<std::string>("tip_name", tip_link, "wrist_roll_link");

    // Load URDF
    urdf::Model model;
    if (!model.initParam("robot_description"))
    {
      ROS_ERROR("Failed to parse URDF");
      return -1;
    }
    robot_model_loader::RobotModelLoader robot_model_loader("robot_description");
    robot_model::RobotModelPtr kinematic_model = robot_model_loader.getModel();
    planning_scene_ = new planning_scene::PlanningScene(kinematic_model);
    current_state_ = &planning_scene_->getCurrentStateNonConst();
    current_state_->setToDefaultValues();

    ROS_INFO("INITIALIZED planning scene");

    // Load the tree
    KDL::Tree kdl_tree;
    if (!kdl_parser::treeFromUrdfModel(model, kdl_tree))
    {
      ROS_ERROR("Could not construct tree from URDF");
      return -1;
    }

    // Populate the Chain
    if (!kdl_tree.getChain(root_link, tip_link, kdl_chain_))
    {
      ROS_ERROR("Could not construct chain from URDF");
      return -1;
    }

    solver_.reset(new KDL::ChainIkSolverVel_wdls(kdl_chain_));
    fksolver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));
    unsigned num_joints = kdl_chain_.getNrOfJoints();
    tgt_jnt_pos_.resize(num_joints);
    last_tgt_jnt_pos_.resize(num_joints);
    extrp_jnt_pos_.resize(num_joints);
    tgt_jnt_vel_.resize(num_joints);
    last_tgt_jnt_vel_.resize(num_joints);

    // Init Joint Handles
    joints_.clear();
    for (size_t i = 0; i < kdl_chain_.getNrOfSegments(); ++i)
      if (kdl_chain_.getSegment(i).getJoint().getType() != KDL::Joint::None)
        joints_.push_back(manager_->getJointHandle(kdl_chain_.getSegment(i).getJoint().getName()));

    if (joints_.size() != num_joints)
    {
      ROS_ERROR("Inconsistent joint count %d, %d", num_joints, int(joints_.size()));
      return -1;
    }

    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      last_tgt_jnt_vel_(ii) = 0.0;
    }

    // Subscribe to joint_publisher
    extra_joint_sub_ =
      nh.subscribe("/joint_states", 1, &CartesianTwistAvoidController::updateJoints, this);

    // Subscribe to command
    command_sub_ = nh.subscribe<geometry_msgs::TwistStamped>(
      "command", 1, boost::bind(&CartesianTwistAvoidController::command, this, _1));
    last_command_time_ = ros::Time(0);

    initialized_ = true;
    return 0;
  }

  bool CartesianTwistAvoidController::start()
  {
    if (!initialized_)
    {
      ROS_ERROR_NAMED("CartesianTwistAvoidController", "Unable to start, not initialized.");
      is_active_ = false;
      return false;
    }

    for (unsigned ii = 0; ii < joints_.size(); ++ii)
    {
      last_tgt_jnt_vel_(ii) = joints_[ii]->getVelocity();
      tgt_jnt_pos_(ii) = joints_[ii]->getPosition();
      last_tgt_jnt_pos_(ii) = joints_[ii]->getPosition();
    }
    is_active_ = true;
    return true;
  }

  bool CartesianTwistAvoidController::stop(bool force)
  {
    is_active_ = false;
    return true;
  }

  bool CartesianTwistAvoidController::reset()
  {
    // Simply stop
    return (manager_->requestStop(getName()) == 0);
  }

  void CartesianTwistAvoidController::update(const ros::Time& now, const ros::Duration& dt)
  {
    // Need to initialize KDL structs
    if (!initialized_)
      return;  // Should never really hit this

    KDL::Frame cart_pose;
    // Copy desired twist and update time to local var to reduce lock contention
    KDL::Twist twist;
    ros::Time last_command_time;
    {
      boost::mutex::scoped_lock lock(mutex_);
      // FK is used to transform the twist command seen from end-effector frame to
      // the one seen from body frame.
      if (fksolver_->JntToCart(tgt_jnt_pos_, cart_pose) < 0)
      {
        twist.Zero();
        ROS_ERROR_THROTTLE(1.0, "FKsolver solver failed");
      }
      else
      {
        if (twist_command_frame_ == "end_effector_frame")
        {
          twist = cart_pose.M * twist_command_;
        }
        else
        {
          twist = twist_command_;
        }
      }
      last_command_time = last_command_time_;
    }

    unsigned num_joints = joints_.size();

     if ((now - last_command_time) > ros::Duration(0.5))
    {
      manager_->requestStop(getName());
    }

    // change the twist here
    if (solver_->CartToJnt(tgt_jnt_pos_, twist, tgt_jnt_vel_) < 0)
    {
      for (unsigned ii = 0; ii < num_joints; ++ii)
      {
        tgt_jnt_vel_(ii) = 0.0;
      }
    }

    // Limit joint velocities by scaling all target velocities equally so
    // resulting movement is in same direction
    double max_vel = 0.0;
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      max_vel = std::max(std::abs(tgt_jnt_vel_(ii)), max_vel);
    }

    double joint_velocity_limit = 0.5;
    double scale = 1.0;
    if (max_vel > joint_velocity_limit)
    {
      double scale = joint_velocity_limit / max_vel;
      for (unsigned ii = 0; ii < num_joints; ++ii)
      {
        tgt_jnt_vel_(ii) *= scale;
      }
      ROS_DEBUG_THROTTLE(1.0, "Joint velocity limit reached.");
    }

    // Make sure solver didn't generate any NaNs.
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      if (!std::isfinite(tgt_jnt_vel_(ii)))
      {
        ROS_ERROR_THROTTLE(1.0, "Target joint velocity (%d) is not finite : %f", ii,
                           tgt_jnt_vel_(ii));
        tgt_jnt_vel_(ii) = 1.0;
      }
    }
    // Limit accelerations while trying to keep same resulting direction
    // somewhere between previous and current value
    scale = 1.0;
    double accel_limit = 1.0;
    double vel_delta_limit = accel_limit * dt.toSec();
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      double vel_delta = std::abs(tgt_jnt_vel_(ii) - last_tgt_jnt_vel_(ii));
      if (vel_delta > vel_delta_limit)
      {
        scale = std::min(scale, vel_delta_limit / vel_delta);
      }
    }

    if (scale <= 0.0)
    {
      ROS_ERROR_THROTTLE(1.0, "CartesianTwistAvoidController: acceleration limit "
                              "produces non-positive scale %f",
                         scale);
      scale = 0.0;
    }

    // Linear interpolate betwen previous velocity and new target velocity using
    // scale.
    // scale = 1.0  final velocity = new target velocity
    // scale = 0.0  final velocity = previous velocity
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      tgt_jnt_vel_(ii) = (tgt_jnt_vel_(ii) - last_tgt_jnt_vel_(ii)) * scale + last_tgt_jnt_vel_(ii);
    }

    // Calculate new target position of joint.  Put target position a few
    // timesteps into the future
    double dt_sec = dt.toSec();
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      tgt_jnt_pos_(ii) += tgt_jnt_vel_(ii) * dt_sec;
    }

    // Limit target position of joint
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      if (!joints_[ii]->isContinuous())
      {
        if (tgt_jnt_pos_(ii) > joints_[ii]->getPositionMax())
        {
          tgt_jnt_pos_(ii) = joints_[ii]->getPositionMax();
        }
        else if (tgt_jnt_pos_(ii) < joints_[ii]->getPositionMin())
        {
          tgt_jnt_pos_(ii) = joints_[ii]->getPositionMin();
        }
      }
    }
    // THIS IS THE COLLISION AVOIDANCE PART

    // extrapolating 4+1 timesteps in the future
    for (unsigned ii = 0; ii < num_joints; ++ii)
    {
      extrp_jnt_pos_(ii) = tgt_jnt_pos_(ii) + tgt_jnt_vel_(ii) * dt_sec * 9;
    }

    for (unsigned jj = 0; jj < joints_.size(); jj++)
    {
      // ROS_INFO_STREAM("Joint
      // Name:"<<joints_[jj]->getName()<<"Value"<<joints_[jj]->getPosition());
      current_state_->setJointPositions(joints_[jj]->getName(), &extrp_jnt_pos_(jj));
    }
    // The finger joints must be manually updated

    collision_result.clear();
    // planning_scene->getCurrentState().printStatePositions();

    planning_scene_->checkSelfCollision(collision_request, collision_result);
    // ROS_INFO_STREAM("Num of contact  is "<<collision_result.contact_count << "
    // self collision");

    if (collision_result.collision)
    {
      ROS_WARN_STREAM("COllision is Imminent Stopping the movement");
      for (size_t ii = 0; ii < joints_.size(); ++ii)
      {
        // We will send the last valid know state
        tgt_jnt_pos_(ii) = last_tgt_jnt_pos_(ii);
        tgt_jnt_vel_(ii) = 0.0;
      }
    }
    else
    {
      for (size_t ii = 0; ii < joints_.size(); ++ii)
      {
        // This is the last known valid state
        last_tgt_jnt_pos_(ii) = tgt_jnt_pos_(ii);
      }
    }

    for (size_t ii = 0; ii < joints_.size(); ++ii)
    {
      joints_[ii]->setPosition(tgt_jnt_pos_(ii), tgt_jnt_vel_(ii), 0.0);
      last_tgt_jnt_vel_(ii) = tgt_jnt_vel_(ii);
    }
  }

  void CartesianTwistAvoidController::command(const geometry_msgs::TwistStamped::ConstPtr& goal)
  {
    // Need to initialize KDL structs
    if (!initialized_)
    {
      ROS_ERROR("CartesianTwistAvoidController: Cannot accept goal, controller "
                "is not initialized.");
      return;
    }

    if (goal->header.frame_id.empty())
    {
       manager_->requestStop(getName());
       return;
    }

    KDL::Twist twist;
    twist(0) = goal->twist.linear.x;
    twist(1) = goal->twist.linear.y;
    twist(2) = goal->twist.linear.z;
    twist(3) = goal->twist.angular.x;
    twist(4) = goal->twist.angular.y;
    twist(5) = goal->twist.angular.z;

    for (int ii = 0; ii < 6; ++ii)
    {
      if (!std::isfinite(twist(ii)))
      {
        ROS_ERROR_THROTTLE(1.0, "Twist command value (%d) is not finite : %f", ii, twist(ii));
        twist(ii) = 0.0;
      }
    }

    ros::Time now(ros::Time::now());

    {
      boost::mutex::scoped_lock lock(mutex_);
      twist_command_frame_ = goal->header.frame_id;
      twist_command_ = twist;
      last_command_time_ = now;
    }
    // Try to start up
    if (!is_active_ && manager_->requestStart(getName()) != 0)
    {
      ROS_ERROR("CartesianTwistAvoidController: Cannot start, blocked by another "
                "controller.");
      return;
    }
  }

  std::vector<std::string> CartesianTwistAvoidController::getCommandedNames()
  {
    std::vector<std::string> names;
    if (initialized_)
    {
      for (size_t i = 0; i < kdl_chain_.getNrOfSegments(); ++i)
        if (kdl_chain_.getSegment(i).getJoint().getType() != KDL::Joint::None)
          names.push_back(kdl_chain_.getSegment(i).getJoint().getName());
    }
    return names;
  }

  std::vector<std::string> CartesianTwistAvoidController::getClaimedNames()
  {
    return getCommandedNames();
  }

  void CartesianTwistAvoidController::updateJoints(const sensor_msgs::JointStatePtr& msg)
  {
    // We only want to update the joints that are not commanded by this controller
    std::vector<std::string> joint_names{"l_gripper_finger_joint", "r_gripper_finger_joint",
                                         "head_pan_joint",         "head_tilt_joint",
                                         "torso_lift_joint",       "bellows_joint"};

    for (size_t i = 0; i < msg->name.size(); i++)
    {
      if (std::find(std::begin(joint_names), std::end(joint_names), msg->name[i]) !=
          std::end(joint_names))
      {
        current_state_->setJointPositions(msg->name[i], &msg->position[i]);
      }
    }
  }

}  // namespace robot_controllers
