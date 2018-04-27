# augmented_robot_controllers
This repo contains augmented_controllers plugins  that extend the capabilites the controllers that exist in this repo https://github.com/KavrakiLab/robot_controllers

Currently The following augmented controllers have been implemented :
1. **CartesianTwistAvoidController.**

## CartesianTwistAvoidController
It extends the **CartesianTwistController** using moveit to check for collision in realtime.
To use this plugin you have to edit in defaults_controller.yaml the following line :

```
cartesian_twist:
   type: robot_controllers/CartesianTwistController"
```
to 

```
cartesian_twist:
   type: "augmented_robot_controllers/CartesianTwistAvoidController"
```
