/// @file
///
/// kuka_control is designed to compute the torque command based on 
/// desired joint position, velocity, and acceleration, and measured joint position and velocity.
/// Currently, the final torque command is composed of inverse dynamics torque command and joint position PD
/// controller command.
/// (TODO: Generalize this ID controller to more general types of feedback controllers)
/// Messages are sent via LCM channels.

#include <lcm/lcm-cpp.hpp>

#include "drake/common/drake_assert.h"
#include "drake/common/drake_path.h"
#include "drake/examples/kuka_iiwa_arm/iiwa_common.h"
#include "drake/multibody/parsers/urdf_parser.h"
#include "drake/multibody/rigid_body_tree.h"

#include "drake/lcmt_robot_controller_reference.hpp"
#include "drake/lcmt_iiwa_status.hpp"
#include "drake/lcmt_iiwa_command.hpp"

#include "drake/util/drakeGeometryUtil.h"

using Eigen::MatrixXd;
using Eigen::VectorXd;
using Eigen::VectorXi;
using drake::Vector1d;
using Eigen::Vector2d;
using Eigen::Vector3d;

namespace drake {
namespace examples {
namespace kuka_iiwa_arm {
namespace {

const char* const kLcmStatusChannel = "IIWA_STATUS";
const char* const kLcmControlRefChannel = "CONTROLLER_REFERENCE";
const char* const kLcmCommandChannel = "IIWA_COMMAND";
const int kNumJoints = 7;

class RobotController {
 public:
  /// tree is aliased
  explicit RobotController(const RigidBodyTree<double>& tree)
      : tree_(tree), controller_trigger_(false) {
    VerifyIiwaTree(tree);
    lcm_.subscribe(kLcmStatusChannel,
                    &RobotController::HandleStatus, this);
    lcm_.subscribe(kLcmControlRefChannel,
                    &RobotController::HandleControl, this);
  }

  void Run() {
    int64_t cur_time_us = -1;

    // Initialize the timestamp to an invalid number so we can detect the first message.
    iiwa_status_.utime = cur_time_us;
    robot_controller_reference_.utime = cur_time_us;

    lcmt_iiwa_command iiwa_command;
    iiwa_command.num_joints = kNumJoints;
    iiwa_command.joint_position.resize(kNumJoints, 0.);
    iiwa_command.num_torques = kNumJoints;
    iiwa_command.joint_torque.resize(kNumJoints, 0.);

    Eigen::VectorXd joint_position_desired(kNumJoints); 
    Eigen::VectorXd joint_velocity_desired(kNumJoints); 
    Eigen::VectorXd joint_accel_desired(kNumJoints); 

    bool half_servo_rate_flag_ = true; // make the iiwa command get published every two servo loops

    while (true) {
      // Call lcm handle until at least one message is processed
      while (0 == lcm_.handleTimeout(10)) { }
      DRAKE_ASSERT(iiwa_status_.utime != -1);
      cur_time_us = iiwa_status_.utime;

      if (controller_trigger_) {  
        const int kNumDof = 7;
        iiwa_command.utime = iiwa_status_.utime;

        // Kuka-Controller (Currently implement an inverse dynamics controller)
        // Set desired joint position, velocity and acceleration
        for (int joint = 0; joint < kNumDof; joint++){
          joint_position_desired(joint) = robot_controller_reference_.joint_position_desired[joint];
          joint_velocity_desired(joint) = robot_controller_reference_.joint_velocity_desired[joint];
          joint_accel_desired(joint) = robot_controller_reference_.joint_accel_desired[joint];
        }

        double *qptr = &iiwa_status_.joint_position_measured[0];
        Eigen::Map<Eigen::VectorXd> q(qptr, kNumDof);
        double *qdptr = &iiwa_status_.joint_velocity_estimated[0];
        Eigen::Map<Eigen::VectorXd> qd(qdptr, kNumDof);

        // Computing inverse dynamics torque command
        KinematicsCache<double> cache = tree_.doKinematics(q, qd);
        const RigidBodyTree<double>::BodyToWrenchMap no_external_wrenches;
        Eigen::VectorXd torque_command = tree_.inverseDynamics(cache, no_external_wrenches, joint_accel_desired, false);

        // gravity compensation without gripper (to cancel out the low-level kuka controller)
        Eigen::VectorXd z = Eigen::VectorXd::Zero(kNumDof); 
        Eigen::VectorXd gravity_torque = gravity_comp_no_gripper(cache, z, false, tree_);
        torque_command -= gravity_torque;
        
        // PD position control
        Eigen::VectorXd position_ctrl_torque_command(kNumDof);
        Eigen::VectorXd Kp_pos_ctrl(kNumDof); // 7 joints
        Kp_pos_ctrl << 225, 361, 144, 81, 324, 36, 49;// best gains for December 9th kuka demo
        //Kp_pos_ctrl << 100, 100, 100, 100, 100, 81, 50;// original gains
        Eigen::VectorXd Kd_pos_ctrl(kNumDof); // 7 joints
        Kd_pos_ctrl << 25, 33, 20, 15, 36, 2, 3;// best gains for December 9th kuka demo, tune down damping gains from dummy critically damped gains
        //Kd_pos_ctrl << 19, 19, 19, 19, 19, 18, 14;// original gains
        // (TODOs) Add integral control (anti-windup)
        for (int joint = 0; joint < kNumJoints; joint++) {
          position_ctrl_torque_command(joint) = Kp_pos_ctrl(joint)*(joint_position_desired(joint) - iiwa_status_.joint_position_measured[joint])
                                              + Kd_pos_ctrl(joint)*(joint_velocity_desired(joint) - iiwa_status_.joint_velocity_estimated[joint]);
        }
        //Combination of ID torque control and IK position control
        torque_command += position_ctrl_torque_command;

        // -------->(For Safety) Set up iiwa position command<----------
        for (int joint = 0; joint < kNumJoints; joint++) {
          iiwa_command.joint_position[joint] = joint_position_desired(joint);
        }

        // -------->Set up iiwa torque command<-------------
        for (int joint = 0; joint < kNumJoints; joint++) {
          iiwa_command.joint_torque[joint] = torque_command(joint);
          iiwa_command.joint_torque[joint] = std::max(-150.0, std::min(150.0, iiwa_command.joint_torque[joint]));
        }

        if (half_servo_rate_flag_){
          half_servo_rate_flag_ = false;
          lcm_.publish(kLcmCommandChannel, &iiwa_command);
        }else{
          half_servo_rate_flag_ = true;
        }
      }
    
    }
  }

 private:
  void HandleStatus(const lcm::ReceiveBuffer* rbuf, const std::string& chan,
                    const lcmt_iiwa_status* status) {
    iiwa_status_ = *status;
  }

  void HandleControl(const lcm::ReceiveBuffer* rbuf, const std::string& chan,
                    const lcmt_robot_controller_reference* input) {
    robot_controller_reference_ = *input;
    controller_trigger_ = true;
  }

  Eigen::VectorXd gravity_comp_no_gripper(KinematicsCache<double>& cache, const Eigen::VectorXd& vd,
      bool include_velocity_terms, const RigidBodyTree<double>& tree) const {
    cache.checkCachedKinematicsSettings(include_velocity_terms, include_velocity_terms, "gravity_comp_no_gripper");

    const bool include_acceleration_terms = true;
    int num_joints = 7;
    int kTwistSize = 6;
    unsigned int body_size_no_gripper = tree.FindBodyIndex("iiwa_link_ee") + 1; // the last arm link before gripper links, + 1 is due to additional world frame

    // Compute spatial accelerations and net wrenches that should be exerted to
    // achieve those accelerations.
    Matrix6X<double> body_accelerations(kTwistSize, body_size_no_gripper);
    Matrix6X<double> net_wrenches(kTwistSize, body_size_no_gripper);
    for (size_t i = 0; i < body_size_no_gripper; ++i) {
      const RigidBody<double>& body = *tree.bodies[i];
      if (body.has_parent_body()) {
        const RigidBody<double>& parent_body = *(body.get_parent());
        const auto& cache_element = cache.get_element(i);

        auto body_acceleration = body_accelerations.col(i);

        // Initialize body acceleration to acceleration of parent body.
        auto parent_acceleration =
            body_accelerations.col(parent_body.get_body_index());
        body_acceleration = parent_acceleration;
        // Add component due to joint acceleration.
        if (include_acceleration_terms) {
          const DrakeJoint& joint = body.getJoint();
          auto vd_joint = vd.middleRows(body.get_velocity_start_index(),
                                        joint.get_num_velocities());
          body_acceleration.noalias() +=
              cache_element.motion_subspace_in_world * vd_joint;
        }
        auto net_wrench = net_wrenches.col(i);
        const auto& body_inertia = cache_element.inertia_in_world;
        net_wrench.noalias() = body_inertia * body_acceleration;
      } else {
        drake::TwistVector<double> a_grav;
        a_grav << 0, 0, 0, 0, 0, -9.81;
        body_accelerations.col(i) = -a_grav.cast<double>();
        net_wrenches.col(i).setZero();
      }
    }

    // Do a backwards pass to compute joint wrenches from net wrenches, 
    // and project the joint wrenches onto the joint's motion subspace to find the joint torque.
    auto& joint_wrenches = net_wrenches;
    const auto& joint_wrenches_const = net_wrenches;
    VectorX<double> gravity_torques(num_joints, 1);

    for (ptrdiff_t i = body_size_no_gripper - 1; i >= 0; --i) {
      RigidBody<double>& body = *tree.bodies[i];
      if (body.has_parent_body()) {
        const auto& cache_element = cache.get_element(i);
        const auto& joint = body.getJoint();
        auto joint_wrench = joint_wrenches_const.col(i);

        const auto& motion_subspace = cache_element.motion_subspace_in_world;
        auto joint_torques = gravity_torques.middleRows(body.get_velocity_start_index(), joint.get_num_velocities());
        joint_torques.noalias() = motion_subspace.transpose() * joint_wrench;

        const RigidBody<double>& parent_body = *(body.get_parent());
        auto parent_joint_wrench = joint_wrenches.col(parent_body.get_body_index());
        parent_joint_wrench += joint_wrench;
      }
    }

    return gravity_torques;
  }

  lcm::LCM lcm_;
  const RigidBodyTree<double>& tree_;
  bool controller_trigger_;// control runner wait for the first message from plan runner 
  lcmt_iiwa_status iiwa_status_;
  lcmt_robot_controller_reference robot_controller_reference_;
};

int DoMain(int argc, const char* argv[]) {

  auto tree = std::make_unique<RigidBodyTree<double>>();
  parsers::urdf::AddModelInstanceFromUrdfFileToWorld(
      GetDrakePath() + "/examples/kuka_iiwa_arm/urdf/iiwa14_fixed_gripper.urdf",
      multibody::joints::kFixed, tree.get());

  RobotController runner(*tree);
  runner.Run();
  return 0;
}

}  // namespace
}  // namespace kuka_iiwa_arm
}  // namespace examples
}  // namespace drake

int main(int argc, const char* argv[]) {
  return drake::examples::kuka_iiwa_arm::DoMain(argc, argv);
}