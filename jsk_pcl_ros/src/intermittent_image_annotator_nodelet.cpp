// -*- mode: c++ -*-
/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, JSK Lab
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
 *     disclaimer in the documentation and/o2r other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
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
#define BOOST_PARAMETER_MAX_ARITY 7
#include "jsk_pcl_ros/intermittent_image_annotator.h"
#include <tf_conversions/tf_eigen.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <eigen_conversions/eigen_msg.h>
#include "jsk_pcl_ros/pcl_conversion_util.h"
#include <visualization_msgs/Marker.h>

namespace jsk_pcl_ros
{
  void IntermittentImageAnnotator::onInit()
  {
    DiagnosticNodelet::onInit();
    listener_ = TfListenerSingleton::getInstance();
    last_publish_time_ = ros::Time::now();
    pnh_->param("fixed_frame_id", fixed_frame_id_, std::string("odom"));
    pnh_->param("max_image_buffer", max_image_buffer_, 5);
    pnh_->param("rate", rate_, 1.0);
    pub_pose_ = pnh_->advertise<geometry_msgs::PoseStamped>(
      "output/direction", 1);
    pub_roi_ = pnh_->advertise<jsk_pcl_ros::PosedCameraInfo>(
      "output/roi", 1);
    pub_marker_ = pnh_->advertise<visualization_msgs::Marker>(
      "output/marker", 1);
    // resize ring buffer
    snapshot_buffer_ = boost::circular_buffer<SnapshotInformation::Ptr>(
      max_image_buffer_);
    image_transport::ImageTransport it(*pnh_);
    image_pub_ = it.advertise("output", 1);
    image_sub_ = it.subscribeCamera(
      "input/image", 1,
      &IntermittentImageAnnotator::cameraCallback,
      this);
    rect_sub_ = pnh_->subscribe("output/screenrectangle", 1,
                                &IntermittentImageAnnotator::rectCallback,
                                this);
    shutter_service_ = pnh_->advertiseService(
      "shutter",
      &IntermittentImageAnnotator::shutterCallback, this);
    request_service_ = pnh_->advertiseService(
      "request",
      &IntermittentImageAnnotator::requestCallback, this);
    clear_service_ = pnh_->advertiseService(
      "clear",
      &IntermittentImageAnnotator::clearCallback, this);
  }

  // we donnot use subscribe/unsubscribe on demand
  void IntermittentImageAnnotator::subscribe()
  {
  }

  void IntermittentImageAnnotator::unsubscribe()
  {
  }

  void IntermittentImageAnnotator::rectCallback(
    const geometry_msgs::PolygonStamped::ConstPtr& rect)
  {
    boost::mutex::scoped_lock lock(mutex_);
    int x0 = rect->polygon.points[0].x;
    int x1 = rect->polygon.points[1].x;
    int y0 = rect->polygon.points[0].y;
    int y1 = rect->polygon.points[1].y;
    if (x0 > x1) {
      std::swap(x0, x1);
    }
    if (y0 > y1) {
      std::swap(y0, y1);
    }
    // check x region
    int width = latest_image_msg_->width;
    int x0_index = x0 / width;
    int x1_index = x1 / width;
    if (x0_index != x1_index) {
      NODELET_WARN("malformed rectangle");
      return;
    }
    else {
      int image_index = x0_index;
      NODELET_INFO("image index: %d", image_index);
      SnapshotInformation::Ptr info = snapshot_buffer_[image_index];
      // local point
      int width_offset = width * image_index;
      int x0_wo_offset = x0 - width_offset;
      int x1_wo_offset = x1 - width_offset;
      cv::Point2d mid((x0_wo_offset + x1_wo_offset) / 2.0,
                      (y0 + y1) / 2.0);
      Eigen::Affine3d pose(info->camera_pose_);
      image_geometry::PinholeCameraModel camera_model = info->camera_;
      cv::Point3d mid_3d = camera_model.projectPixelTo3dRay(mid);
      Eigen::Vector3d ray(mid_3d.x, mid_3d.y, mid_3d.z); // ray is camera local
      ray = ray / ray.norm();
      Eigen::Vector3d ray_global = pose.rotation() * ray;
      NODELET_INFO("ray: [%f, %f, %f]", ray_global[0], ray_global[1], ray_global[2]);
      
      Eigen::Vector3d z = pose.rotation() * Eigen::Vector3d::UnitZ();
      NODELET_INFO("z: [%f, %f, %f]", z[0], z[1], z[2]);
      Eigen::Vector3d original_pos = pose.translation();
      Eigen::Quaterniond q;
      q.setFromTwoVectors(z, ray_global);
      NODELET_INFO("q: [%f, %f, %f, %f]", q.x(), q.y(), q.z(), q.w());
      Eigen::Affine3d output_pose = pose.rotate(q);
      output_pose.translation() = original_pos;
      geometry_msgs::PoseStamped ros_pose;
      tf::poseEigenToMsg(output_pose, ros_pose.pose);
      ros_pose.header.stamp = latest_image_msg_->header.stamp;
      ros_pose.header.frame_id = fixed_frame_id_;
      pub_pose_.publish(ros_pose);

      // publish ROI
      jsk_pcl_ros::PosedCameraInfo camera_info;
      camera_info.header.stamp = latest_image_msg_->header.stamp;
      camera_info.header.frame_id = fixed_frame_id_;
      camera_info.camera_info
        = sensor_msgs::CameraInfo(info->camera_.cameraInfo());
      camera_info.camera_info.roi.x_offset = x0_wo_offset;
      camera_info.camera_info.roi.y_offset = y0;
      camera_info.camera_info.roi.width = x1_wo_offset - x0_wo_offset;
      camera_info.camera_info.roi.height = y1 - y0;
      tf::poseEigenToMsg(info->camera_pose_, camera_info.offset);
      pub_roi_.publish(camera_info);
      
      // marker
      // 2d points
      cv::Point2d A(x0_wo_offset, y0);
      cv::Point2d B(x0_wo_offset, y1);
      cv::Point2d C(x1_wo_offset, y1);
      cv::Point2d D(x1_wo_offset, y0);
      // 3d local points
      cv::Point3d A_3d = info->camera_.projectPixelTo3dRay(A) * 3;
      cv::Point3d B_3d = info->camera_.projectPixelTo3dRay(B) * 3;
      cv::Point3d C_3d = info->camera_.projectPixelTo3dRay(C) * 3;
      cv::Point3d D_3d = info->camera_.projectPixelTo3dRay(D) * 3;
      cv::Point3d O_3d;
      // convert to ros point
      geometry_msgs::Point A_ros, B_ros, C_ros, D_ros, O_ros;
      pointFromXYZToXYZ<cv::Point3d, geometry_msgs::Point>(A_3d, A_ros);
      pointFromXYZToXYZ<cv::Point3d, geometry_msgs::Point>(B_3d, B_ros);
      pointFromXYZToXYZ<cv::Point3d, geometry_msgs::Point>(C_3d, C_ros);
      pointFromXYZToXYZ<cv::Point3d, geometry_msgs::Point>(D_3d, D_ros);
      pointFromXYZToXYZ<cv::Point3d, geometry_msgs::Point>(O_3d, O_ros);
      // build edges
      visualization_msgs::Marker marker;
      marker.header.stamp = latest_image_msg_->header.stamp;
      marker.header.frame_id = fixed_frame_id_;
      tf::poseEigenToMsg(info->camera_pose_, marker.pose);
      marker.type = visualization_msgs::Marker::LINE_LIST;
      marker.points.push_back(O_ros); marker.points.push_back(A_ros);
      marker.points.push_back(O_ros); marker.points.push_back(B_ros);
      marker.points.push_back(O_ros); marker.points.push_back(C_ros);
      marker.points.push_back(O_ros); marker.points.push_back(D_ros);
      marker.points.push_back(A_ros); marker.points.push_back(B_ros);
      marker.points.push_back(B_ros); marker.points.push_back(C_ros);
      marker.points.push_back(C_ros); marker.points.push_back(D_ros);
      marker.points.push_back(D_ros); marker.points.push_back(A_ros);
      marker.scale.x = 0.01;
      marker.scale.y = 0.01;
      marker.scale.z = 0.01;
      marker.color.a = 1.0;
      marker.color.r = 1.0;
      pub_marker_.publish(marker);
    }
  }
  
  void IntermittentImageAnnotator::cameraCallback(
      const sensor_msgs::Image::ConstPtr& image_msg,
      const sensor_msgs::CameraInfo::ConstPtr& info_msg)
  {
    boost::mutex::scoped_lock lock(mutex_);
    vital_checker_->poke();
    latest_image_msg_ = image_msg;
    latest_camera_info_msg_ = info_msg;

    if (snapshot_buffer_.size() != 0) {
      ros::Time now = ros::Time::now();
      if ((now - last_publish_time_).toSec() > 1.0 / rate_) {
        cv::Mat concatenated_image;
        std::vector<cv::Mat> images;
        //ROS_INFO("%lu images", snapshot_buffer_.size());
        for (size_t i = 0; i < snapshot_buffer_.size(); i++) {
          images.push_back(snapshot_buffer_[i]->image_);
        }
        cv::hconcat(images, concatenated_image);
        cv_bridge::CvImage concatenate_bridge(latest_camera_info_msg_->header, // ??
                                              sensor_msgs::image_encodings::BGR8,
                                              concatenated_image);
        image_pub_.publish(concatenate_bridge.toImageMsg());
        last_publish_time_ = now;
      }
    }
    
  }
 
 bool IntermittentImageAnnotator::shutterCallback(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    boost::mutex::scoped_lock lock(mutex_);
    if (latest_camera_info_msg_) {
      SnapshotInformation::Ptr
        info (new SnapshotInformation());
      // resolve tf
      try {
        if (listener_->waitForTransform(
              fixed_frame_id_,
              latest_camera_info_msg_->header.frame_id,
              latest_camera_info_msg_->header.stamp,
              ros::Duration(1.0))) {
          tf::StampedTransform transform;
          listener_->lookupTransform(fixed_frame_id_,
                                     latest_camera_info_msg_->header.frame_id,
                                     latest_camera_info_msg_->header.stamp,
                                     transform);
          cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(
            latest_image_msg_,
            sensor_msgs::image_encodings::BGR8);
          Eigen::Affine3d eigen_transform;
          image_geometry::PinholeCameraModel camera;
          camera.fromCameraInfo(latest_camera_info_msg_);
          tf::transformTFToEigen(transform, eigen_transform);
          info->camera_pose_ = eigen_transform;
          info->camera_ = camera;
          info->image_ = cv_ptr->image;
          snapshot_buffer_.push_back(info);
          return true;
        }
        else {
          NODELET_ERROR("failed to resolve tf from %s to %s",
                        fixed_frame_id_.c_str(),
                        latest_camera_info_msg_->header.frame_id.c_str());
          return false;
        }
      }
      catch (tf2::ConnectivityException &e)
      {
        NODELET_ERROR("Transform error: %s", e.what());
        return false;
      }
      catch (tf2::InvalidArgumentException &e)
      {
        NODELET_ERROR("Transform error: %s", e.what());
        return false;
      }
    }
    else {
      NODELET_ERROR("not yet camera message is available");
      return false;
    }
  }

  bool IntermittentImageAnnotator::clearCallback(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    boost::mutex::scoped_lock lock(mutex_);
    snapshot_buffer_.clear();
  }

  bool IntermittentImageAnnotator::requestCallback(
    std_srvs::Empty::Request& req,
    std_srvs::Empty::Response& res)
  {
    // concatenate images
    boost::mutex::scoped_lock lock(mutex_);
    if (snapshot_buffer_.size() == 0) {
      NODELET_ERROR("no image is stored");
      return false;
    }
    else {
      cv::Mat concatenated_image;
      std::vector<cv::Mat> images;
      ROS_INFO("%lu images", snapshot_buffer_.size());
      for (size_t i = 0; i < snapshot_buffer_.size(); i++) {
        images.push_back(snapshot_buffer_[i]->image_);
      }
      cv::hconcat(images, concatenated_image);
      cv_bridge::CvImage concatenate_bridge(latest_camera_info_msg_->header, // ??
                                            sensor_msgs::image_encodings::BGR8,
                                            concatenated_image);
      image_pub_.publish(concatenate_bridge.toImageMsg());
      return true;
    }
  }

  void IntermittentImageAnnotator::updateDiagnostic(
    diagnostic_updater::DiagnosticStatusWrapper &stat)
  {
    if (vital_checker_->isAlive()) {
      stat.summary(diagnostic_msgs::DiagnosticStatus::OK,
                   "IntermittentImageAnnotator running");
    }
    else {
      jsk_topic_tools::addDiagnosticErrorSummary(
        "IntermittentImageAnnotator", vital_checker_, stat);
    }
  }
}

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS (jsk_pcl_ros::IntermittentImageAnnotator, nodelet::Nodelet);
