#include <ros/ros.h>
#include <rospack/rospack.h>
#include <opencv/highgui.h>
#include <cv_bridge/CvBridge.h>
#include <cv_bridge/cv_bridge.h>
#pragma message "Compiling " __FILE__ "..."
#include <posedetection_msgs/Feature0DDetect.h>
#include <posedetection_msgs/ImageFeature0D.h>
#include <posedetection_msgs/ObjectDetection.h>
#include <posedetection_msgs/Object6DPose.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Quaternion.h>
#include <image_transport/image_transport.h>
#include <image_geometry/pinhole_camera_model.h>
#include <tf/tf.h>
#include <boost/shared_ptr.hpp>
#include <boost/foreach.hpp>
#include <vector>
#include <sstream>
#include <iostream>

#include <posedetectiondb/SetTemplate.h>

bool _first_sample_change;


void features2keypoint (posedetection_msgs::Feature0D features,
			std::vector<cv::KeyPoint>& keypoints,
			cv::Mat& descriptors){
  keypoints.resize(features.scales.size());
  descriptors.create(features.scales.size(),features.descriptor_dim,CV_32FC1);
  std::vector<cv::KeyPoint>::iterator keypoint_it = keypoints.begin();
  for ( int i=0; keypoint_it != keypoints.end(); ++keypoint_it, ++i ) {
    *keypoint_it = cv::KeyPoint(cv::Point2f(features.positions[i*2+0],
					    features.positions[i*2+1]),
				features.descriptor_dim, // size
				features.orientations[i], //angle
				0, // resonse
				features.scales[i] // octave
				);
    for (int j = 0; j < features.descriptor_dim; j++){
      descriptors.at<float>(i,j) =
	features.descriptors[i*features.descriptor_dim+j];
    }
  }
}


class Matching_Template
{
public:
  std::string _matching_frame;
  cv::Mat _template_img;
  std::vector<cv::KeyPoint> _template_keypoints;
  cv::Mat _template_descriptors;
  int _original_width_size;
  int _original_height_size;
  double _template_width;	// width of template [m]
  double _template_height;	// height of template [m]
  tf::Transform _relativepose;
  cv::Mat _affine_matrix;
  std::string _window_name;
  // The maximum allowed reprojection error to treat a point pair as an inlier
  double _reprojection_threshold;
  // threshold on squared ratio of distances between NN and 2nd NN
  double _distanceratio_threshold;
  std::vector<cv::Point2d> _correspondances;
  cv::Mat _previous_stack_img;

  Matching_Template(){
  }
  Matching_Template(cv::Mat img,
		    std::string matching_frame,
		    int original_width_size,
		    int original_height_size,
		    double template_width,
		    double template_height,
		    tf::Transform relativepose,
		    cv::Mat affine_matrix,
		    double reprojection_threshold,
		    double distanceratio_threshold,
		    std::string window_name,
		    bool autosize){

    _template_img = img.clone();
    _matching_frame = matching_frame;
    _original_width_size = original_width_size;
    _original_height_size = original_height_size;
    _template_width = template_width;
    _template_height = template_height;
    _relativepose = relativepose;
    _affine_matrix = affine_matrix;
    _window_name = window_name;
    _reprojection_threshold = reprojection_threshold;
    _distanceratio_threshold = distanceratio_threshold;
    _template_keypoints.clear();
    _correspondances.clear();
    //    cv::namedWindow(_window_name, autosize ? CV_WINDOW_AUTOSIZE : 0);
  }


  virtual ~Matching_Template(){
    std::cerr << "delete " << _window_name << std::endl;
  }


  std::string get_window_name(){
    return _window_name;
  }

  std::vector<cv::Point2d>* correspondances(){
    return &_correspondances;
  }

  bool check_template (){
    if (_template_img.empty() && _template_keypoints.size() == 0 &&
	_template_descriptors.empty()){
      return false;
    }
    else {
      return true;
    }
  }


  int set_template(ros::ServiceClient client){
    posedetection_msgs::Feature0DDetect srv;
    sensor_msgs::CvBridge bridge;

    if ( _template_img.empty()) {
      ROS_ERROR ("template picture is empty.");
      return -1;
    }
    ROS_INFO_STREAM("read template image and call " << client.getService() << " service");
    boost::shared_ptr<IplImage> _template_imgipl =
      boost::shared_ptr<IplImage>(new IplImage (_template_img));
    srv.request.image = *bridge.cvToImgMsg(_template_imgipl.get(), "bgr8");
    if (client.call(srv)) {
      ROS_INFO_STREAM("get features with " << srv.response.features.scales.size() << " descriptoins");
      features2keypoint (srv.response.features, _template_keypoints, _template_descriptors);

      // inverse affine translation
      cv::Mat M_inv;
      cv::Mat dst;
      cv::invert(_affine_matrix, M_inv);
      //      cv::drawKeypoints (tmp, _template_keypoints, dst, cv::Scalar::all(1), cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
      //      cv::drawKeypoints (tmp, _template_keypoints, dst);
      // cv::warpPerspective(_template_img, dst, M_inv,
      // 			  cv::Size(_original_width_size, _original_height_size),
      // 			  CV_INTER_LINEAR, IPL_BORDER_CONSTANT, 0);
      for (int i = 0; i < (int)_template_keypoints.size(); i++){
	cv::Point2f pt;
	cv::Mat pt_mat(cv::Size(1,1), CV_32FC2, &pt);
	cv::Mat pt_src_mat(cv::Size(1,1), CV_32FC2, &_template_keypoints.at(i).pt);
	cv::perspectiveTransform (pt_src_mat, pt_mat, M_inv);
	_template_keypoints.at(i).pt = pt_mat.at<cv::Point2f>(0,0);
      }
      //      cvSetMouseCallback (_window_name.c_str(), &PointPoseExtractor::cvmousecb, this);
      //      _template_img = dst;
      return 0;
    } else {
      ROS_ERROR("Failed to call service Feature0DDetect");
      return 1;
    }
  }


  double log_fac( int n )
  {
    static std::vector<double> slog_table;
    int osize = slog_table.size();
    if(osize <= n){
      slog_table.resize(n+1);
      if(osize == 0){
	slog_table[0] = -1;
	slog_table[1] = log(1.0);
	osize = 2;
      }
      for(int i = osize; i <= n; i++ ){
	slog_table[i] = slog_table[i-1] + log(i);
      }
    }
    return slog_table[n];
  }

  int min_inlier( int n, int m, double p_badsupp, double p_badxform )
  {
    double pi, sum;
    int i, j;
    double lp_badsupp = log( p_badsupp );
    double lp_badsupp1 = log( 1.0 - p_badsupp );
    for( j = m+1; j <= n; j++ ){
      sum = 0;
      for( i = j; i <= n; i++ ){
	pi = (i-m) * lp_badsupp + (n-i+m) * lp_badsupp1 +
	  log_fac( n - m ) - log_fac( i - m ) - log_fac( n - i );
	sum += exp( pi );
      }
      if( sum < p_badxform )
	break;
    }
    return j;
  }


  bool estimate_od (ros::ServiceClient client, cv::Mat src_img,
		    std::vector<cv::KeyPoint> sourceimg_keypoints,
		    image_geometry::PinholeCameraModel pcam,
		    double err_thr, cv::Mat &stack_img,
		    cv::flann::Index* ft, posedetection_msgs::Object6DPose* o6p){

    if ( _template_keypoints.size()== 0 &&
	 _template_descriptors.empty() ){
      set_template(client);
    }
    if ( _template_keypoints.size()== 0 &&
	 _template_descriptors.empty() ) {
      ROS_ERROR ("Template image was not set.");
      return false;
    }

    // stacked image
    cv::Size stack_size  = cv::Size(MAX(src_img.cols,_template_img.cols),
				    src_img.rows+_template_img.rows);
    stack_img = cv::Mat(stack_size,CV_8UC3);
    stack_img = cv::Scalar(0);
    cv::Mat stack_img_tmp(stack_img,cv::Rect(0,0,_template_img.cols,_template_img.rows));
    cv::add(stack_img_tmp, _template_img, stack_img_tmp);
    cv::Mat stack_img_src(stack_img,cv::Rect(0,_template_img.rows,src_img.cols,src_img.rows));
    cv::add(stack_img_src, src_img, stack_img_src);
    _previous_stack_img = stack_img.clone();

    // matching
    cv::Mat m_indices(_template_descriptors.rows, 2, CV_32S);
    cv::Mat m_dists(_template_descriptors.rows, 2, CV_32F);
    ft->knnSearch(_template_descriptors, m_indices, m_dists, 2, cv::flann::SearchParams(-1) );

    // matched points
    std::vector<cv::Point2f> pt1, pt2;
    std::vector<int> queryIdxs,trainIdxs;
    for ( unsigned int j = 0; j < _template_keypoints.size(); j++ ) {
      if ( m_dists.at<float>(j,0) < m_dists.at<float>(j,1) * _distanceratio_threshold) {
	queryIdxs.push_back(j);
	trainIdxs.push_back(m_indices.at<int>(j,0));
      }
    }
    if ( queryIdxs.size() == 0 ) {
      ROS_WARN_STREAM("could not found matched points with distanceratio(" <<_distanceratio_threshold << ")");
    } else {
      cv::KeyPoint::convert(_template_keypoints,pt1,queryIdxs);
      cv::KeyPoint::convert(sourceimg_keypoints,pt2,trainIdxs);
    }

    ROS_INFO ("Found %d total matches among %d template keypoints", (int)pt2.size(), (int)_template_keypoints.size());

    cv::Mat H;
    std::vector<uchar> mask((int)pt2.size());

    if ( pt1.size() > 4 ) {
	  // ToDO for curve face
      H = cv::findHomography(cv::Mat(pt1), cv::Mat(pt2), mask, CV_RANSAC, _reprojection_threshold);
    }

    // draw line
    for (int j = 0; j < (int)pt1.size(); j++){
      cv::Point2f pt;
      cv::Mat pt_mat(cv::Size(1,1), CV_32FC2, &pt);
      cv::Mat pt_src_mat(cv::Size(1,1), CV_32FC2, &pt1.at(j));
      cv::perspectiveTransform (pt_src_mat, pt_mat, _affine_matrix);
      pt1.at(j) = pt_mat.at<cv::Point2f>(0,0);
      if ( mask.at(j)){
	cv::line(stack_img, pt1.at(j), pt2.at(j)+cv::Point2f(0,_template_img.rows),
		 CV_RGB(0,255,0), 1,8,0);
      }
      else {
	cv::line(stack_img, pt1.at(j), pt2.at(j)+cv::Point2f(0,_template_img.rows),
		 CV_RGB(255,0,255), 1,8,0);
      }
    }
    int inlier_sum = 0;
    for (int k=0; k < (int)mask.size(); k++){
      inlier_sum += (int)mask.at(k);
    }

    double text_scale = 1.5;
    {
      int fontFace = 0, thickness, baseLine;
      int x, y;
      cv::Size text_size;
      std::string text;

      text = "inlier: " + boost::lexical_cast<std::string>((int)inlier_sum) + " / " + boost::lexical_cast<std::string>((int)pt2.size());
      text_size = cv::getTextSize(text, fontFace, text_scale, thickness, &baseLine);
      x = stack_img.size().width - text_size.width;
      y = text_size.height + thickness + 10; // 10pt pading
      cv::putText (stack_img, text, cv::Point(x, y),
                   fontFace, text_scale, CV_RGB(0, 255, 0),
                   thickness, 8, false);

      text = "template: " + boost::lexical_cast<std::string>((int)_template_keypoints.size());
      text_size = cv::getTextSize(text, fontFace, text_scale, thickness, &baseLine);
      x = stack_img.size().width - text_size.width;
      y += text_size.height + thickness + 10; // 10pt pading
      cv::putText (stack_img, text, cv::Point(x, y),
                   fontFace, text_scale, CV_RGB(0, 255, 0),
                   thickness, 8, false);
    }

    // draw correspondances
    ROS_INFO("_correspondances.size: %d", (int)_correspondances.size());
    for (int j = 0; j < (int)_correspondances.size(); j++){
      cv::circle(stack_img, cv::Point2f(_correspondances.at(j).x, _correspondances.at(j).y + _template_img.size().height),
		 8, CV_RGB(255,0,0), -1);
    }
    if ((cv::countNonZero( H ) == 0) || (inlier_sum < min_inlier((int)pt2.size(), 4, 0.10, 0.01))){
      if( _window_name != "" )
	cv::imshow(_window_name, stack_img);
      return false;
    }

    std::string _type;
    char chr[20];

    // number of match points
    sprintf(chr, "%d", (int)pt2.size());
    _type = _matching_frame + "_" + std::string(chr);

    sprintf(chr, "%d", inlier_sum);
    _type = _type + "_" + std::string(chr);

    cv::Point2f corners2d[4] = {cv::Point2f(0,0),
				cv::Point2f(_original_width_size,0),
				cv::Point2f(_original_width_size,_original_height_size),
				cv::Point2f(0,_original_height_size)};
    cv::Mat corners2d_mat (cv::Size(4, 1), CV_32FC2, corners2d);
    cv::Point3f corners3d[4] = {cv::Point3f(0,0,0),
				cv::Point3f(0,_template_width,0),
				cv::Point3f(_template_height,_template_width,0),
				cv::Point3f(_template_height,0,0)};
    cv::Mat corners3d_mat (cv::Size(4, 1), CV_32FC3, corners3d);

    cv::Mat corners2d_mat_trans;

    cv::perspectiveTransform (corners2d_mat, corners2d_mat_trans, H);

    double fR3[3], fT3[3];
    cv::Mat rvec(3, 1, CV_64FC1, fR3);
    cv::Mat tvec(3, 1, CV_64FC1, fT3);

    cv::solvePnP (corners3d_mat, corners2d_mat_trans, pcam.intrinsicMatrix(),
		  pcam.distortionCoeffs(),
		  rvec, tvec);

    tf::Transform checktf, resulttf;
    checktf.setOrigin( tf::Vector3(fT3[0], fT3[1], fT3[2] ) );
    double rx = fR3[0], ry = fR3[1], rz = fR3[2];

    tf::Quaternion quat;
    double angle = cv::norm(rvec);

    quat.setRotation(tf::Vector3(rx/angle, ry/angle, rz/angle), angle);
    checktf.setRotation( quat );

    resulttf = checktf * _relativepose;

    ROS_INFO( "tx: (%0.2lf,%0.2lf,%0.2lf) rx: (%0.2lf,%0.2lf,%0.2lf)",
	      resulttf.getOrigin().getX(),
	      resulttf.getOrigin().getY(),
	      resulttf.getOrigin().getZ(),
    	      resulttf.getRotation().getAxis().x() * resulttf.getRotation().getAngle(),
    	      resulttf.getRotation().getAxis().y() * resulttf.getRotation().getAngle(),
    	      resulttf.getRotation().getAxis().z() * resulttf.getRotation().getAngle());

    o6p->pose.position.x = resulttf.getOrigin().getX();
    o6p->pose.position.y = resulttf.getOrigin().getY();
    o6p->pose.position.z = resulttf.getOrigin().getZ();
    o6p->pose.orientation.w = resulttf.getRotation().w();
    o6p->pose.orientation.x = resulttf.getRotation().x();
    o6p->pose.orientation.y = resulttf.getRotation().y();
    o6p->pose.orientation.z = resulttf.getRotation().z();
    o6p->type = _matching_frame; // _type

    // draw 3d cube model
    std::vector<cv::Point2f> projected_top;
    {
      double cfR3[3], cfT3[3];
      cv::Mat crvec(3, 1, CV_64FC1, cfR3);
      cv::Mat ctvec(3, 1, CV_64FC1, cfT3);
      cfT3[0] = checktf.getOrigin().getX();
      cfT3[1] = checktf.getOrigin().getY();
      cfT3[2] = checktf.getOrigin().getZ();
      cfR3[0] = checktf.getRotation().getAxis().x() * checktf.getRotation().getAngle();
      cfR3[1] = checktf.getRotation().getAxis().y() * checktf.getRotation().getAngle();
      cfR3[2] = checktf.getRotation().getAxis().z() * checktf.getRotation().getAngle();

      cv::Point3f coords[8] = {cv::Point3f(0,0,0),
			       cv::Point3f(0, _template_width, 0),
			       cv::Point3f(_template_height, _template_width,0),
			       cv::Point3f(_template_height, 0, 0),
			       cv::Point3f(0, 0, -0.03),
			       cv::Point3f(0, _template_width, -0.03),
			       cv::Point3f(_template_height, _template_width, -0.03),
			       cv::Point3f(_template_height, 0, -0.03)};

      cv::Mat coords_mat (cv::Size(8, 1), CV_32FC3, coords);
      cv::projectPoints(coords_mat, crvec, ctvec, pcam.intrinsicMatrix(),
			pcam.distortionCoeffs(),
			projected_top);
      for (int j = 0; j < 4; j++){
	projected_top.push_back(projected_top.at(j));
      }
    }

    double err_sum = 0;
    bool err_success = true;
    for (int j = 0; j < 4; j++){
      double err = sqrt(pow((corners2d_mat_trans.at<cv::Point2f>(0,j).x - projected_top.at(j).x), 2) +
		      pow((corners2d_mat_trans.at<cv::Point2f>(0,j).y - projected_top.at(j).y), 2));
      err_sum += err;
    }
    if (err_sum > err_thr){
      err_success = false;
    }

    // draw lines araound object
    for (int j = 0; j < corners2d_mat_trans.cols; j++){
      cv::Point2f p1(corners2d_mat_trans.at<cv::Point2f>(0,j).x,
		     corners2d_mat_trans.at<cv::Point2f>(0,j).y+_template_img.rows);
      cv::Point2f p2(corners2d_mat_trans.at<cv::Point2f>(0,(j+1)%corners2d_mat_trans.cols).x,
		     corners2d_mat_trans.at<cv::Point2f>(0,(j+1)%corners2d_mat_trans.cols).y+_template_img.rows);
      cv::line (stack_img, p1, p2, CV_RGB(255, 0, 0),
		(err_success?4:1), // width
		CV_AA, 0);
    }

    // draw 3d cube model
    if(projected_top.size() == 8) { // verify, the size is 8
      int cnt = 8;
      std::vector<cv::Point2f> ps(cnt);
      for(int i=0; i<cnt; i++)
	ps[i] = cv::Point2f(projected_top[i].x,
			    projected_top[i].y+_template_img.rows);

      int draw_width = ( err_success ? 3 : 1);
      for(int i=0; i<4; i++) {
	cv::line (stack_img, ps[i], ps[(i+1)%4], CV_RGB(0, 0, 255), draw_width, CV_AA, 0);
	cv::line (stack_img, ps[i+4], ps[(i+1)%4+4], CV_RGB(0, 0, 255), draw_width, CV_AA, 0);
	cv::line (stack_img, ps[i], ps[i+4], CV_RGB(0, 0, 255), draw_width, CV_AA, 0);
      }
    }

    // draw coords
    if ( err_success )
    {
      double cfR3[3], cfT3[3];
      cv::Mat crvec(3, 1, CV_64FC1, cfR3);
      cv::Mat ctvec(3, 1, CV_64FC1, cfT3);
      cfT3[0] = resulttf.getOrigin().getX();
      cfT3[1] = resulttf.getOrigin().getY();
      cfT3[2] = resulttf.getOrigin().getZ();
      cfR3[0] = resulttf.getRotation().getAxis().x() * resulttf.getRotation().getAngle();
      cfR3[1] = resulttf.getRotation().getAxis().y() * resulttf.getRotation().getAngle();
      cfR3[2] = resulttf.getRotation().getAxis().z() * resulttf.getRotation().getAngle();

      cv::Point3f coords[4] = {cv::Point3f(0,0,0),
			       cv::Point3f(0.05,0,0),
			       cv::Point3f(0,0.05,0),
			       cv::Point3f(0,0,0.05)};
      cv::Mat coords_mat (cv::Size(4, 1), CV_32FC3, coords);
      std::vector<cv::Point2f> coords_img_points;
      cv::projectPoints(coords_mat, crvec, ctvec, pcam.intrinsicMatrix(),
			pcam.distortionCoeffs(),
			coords_img_points);
      std::vector<cv::Point2f> ps(4);
      for(int i=0; i<4; i++)
	ps[i] = cv::Point2f(coords_img_points[i].x,
			    coords_img_points[i].y+_template_img.rows);
      cv::line (stack_img, ps[0], ps[1], CV_RGB(255, 0, 0), 3, CV_AA, 0);
      cv::line (stack_img, ps[0], ps[2], CV_RGB(0, 255, 0), 3, CV_AA, 0);
      cv::line (stack_img, ps[0], ps[3], CV_RGB(0, 0, 255), 3, CV_AA, 0);
    }

    // write text on image
    {
      std::string text;
      int x, y;
      text = "error: " + boost::lexical_cast<std::string>(err_sum);
      x = stack_img.size().width - 16*17*text_scale; // 16pt * 17
      y = _template_img.size().height - (16 + 2)*text_scale*6;
      cv::putText (stack_img, text, cv::Point(x, y),
		   0, text_scale, CV_RGB(0, 255, 0),
		   2, 8, false);

    }
    // for debug window
    if( _window_name != "" )
      cv::imshow(_window_name, stack_img);

    return err_success;
  }
};  // the end of difinition of class Matching_Template


class PointPoseExtractor
{
  ros::NodeHandle _n;
  image_transport::ImageTransport it;
  ros::Subscriber _sub;
  ros::ServiceServer _server;
  ros::ServiceClient _client;
  ros::Publisher _pub, _pub_agg;
  image_transport::Publisher _debug_pub;
  double _reprojection_threshold;
  double _distanceratio_threshold;
  double _th_step;
  double _phi_step;
  bool _autosize;
  double _err_thr;
  cv::Mat _cam_mat;
  cv::Mat _cam_dist;
  static std::vector<Matching_Template *> _templates;
  bool pnod;
  bool _initialized;
  bool _viewer;

public:
  PointPoseExtractor() : it(ros::NodeHandle("~")) {
    // _sub = _n.subscribe("ImageFeature0D", 1,
    //                     &PointPoseExtractor::imagefeature_cb, this);
    _client = _n.serviceClient<posedetection_msgs::Feature0DDetect>("Feature0DDetect");
    _pub = _n.advertise<posedetection_msgs::ObjectDetection>("ObjectDetection", 10);
    _pub_agg = _n.advertise<posedetection_msgs::ObjectDetection>("ObjectDetection_agg", 10);
    _debug_pub = it.advertise("debug_image", 1);
    _server = _n.advertiseService("SetTemplate", &PointPoseExtractor::settemplate_cb, this);
    _initialized = false;
  }

  virtual ~PointPoseExtractor(){
    _sub.shutdown();
    _client.shutdown();
    _pub.shutdown();
    _pub_agg.shutdown();
  }

  static void make_template_from_mousecb(Matching_Template *mt){
    cv::Mat H;
    cv::Mat tmp_template;
    std::vector<cv::Point2f>pt1, pt2;
    double width, height;
    std::cout << "input template's [width]" << std::endl;
    std::cin >> width;
    std::cout << "input template's [height]" << std::endl;
    std::cin >> height;

    for (int i = 0; i < 4; i++){
      pt1.push_back(cv::Point2d((int)mt->_correspondances.at(i).x,
				(int)mt->_correspondances.at(i).y + mt->_template_img.size().height));
    }

    cv::Rect rect = cv::boundingRect(cv::Mat(pt1));
    //    H = cv::findHomography(cv::Mat(pt1), cv::Mat(pt2));

    cv::getRectSubPix(mt->_previous_stack_img, rect.size(),
                      cv::Point2f((rect.tl().x + rect.br().x)/2.0,(rect.tl().y + rect.br().y)/2.0),
                      tmp_template);

    for (int i = 0; i < (int)pt1.size(); i++){
      pt2.push_back(cv::Point2d((int)pt1.at(i).x - rect.x,
				(int)pt1.at(i).y - rect.y - mt->_template_img.size().height));
    }
    // cv::Mat mask_img = cv::Mat::zeros(tmp_template.size(), CV_8UC3);
    // cv::fillConvexPoly(mask_img, pt2.begin(), (int)pt2.size(), CV_RGB(255, 255, 255));

    // cv::namedWindow("hoge", 1);
    // cv::imshow("hoge", mask_img);

    cv::Mat M = (cv::Mat_<double>(3,3) << 1,0,0, 0,1,0, 0,0,1);
    std::string window_name = "sample" + boost::lexical_cast<std::string>((int)_templates.size());

    Matching_Template* tmplt = 
      new Matching_Template (tmp_template, "sample",
			     tmp_template.size().width, tmp_template.size().height,
			     width, height,
			     tf::Transform(tf::Quaternion(0, 0, 0, 1), tf::Vector3(0, 0, 0)),
			     M,
			     mt->_reprojection_threshold,
			     mt->_distanceratio_threshold,
			     _first_sample_change ? window_name : mt->_window_name,
			     cv::getWindowProperty(mt->_window_name, CV_WND_PROP_AUTOSIZE));

    mt->_correspondances.clear();
    _templates.push_back(tmplt);
    cv::namedWindow(_first_sample_change ? window_name : mt->_window_name,
		    cv::getWindowProperty(mt->_window_name, CV_WND_PROP_AUTOSIZE));
    cvSetMouseCallback (_first_sample_change ? window_name.c_str() : mt->_window_name.c_str(),
			&cvmousecb, static_cast<void *>(_templates.back()));
    _first_sample_change = true;
  }

  static void cvmousecb (int event, int x, int y, int flags, void* param){
    Matching_Template *mt = (Matching_Template*)param;
    // std::cerr << "mousecb_ -> " << mt << std::endl;
    switch (event){
    case CV_EVENT_LBUTTONUP: {
      cv::Point2d pt(x,y - (int)mt->_template_img.size().height);
      ROS_INFO("add correspondence (%d, %d)", (int)pt.x, (int)pt.y);
      mt->_correspondances.push_back(pt);
      if ((int)mt->_correspondances.size() >= 4){
	make_template_from_mousecb(mt);
	mt->_correspondances.clear();
	ROS_INFO("reset");
      }
      break;
    }
    case CV_EVENT_RBUTTONUP: {
      mt->_correspondances.clear();
      ROS_INFO("reset");
      break;
    }
    }
  }


  void initialize () {
    std::string matching_frame;
    std::string _pose_str;
    double template_width;
    double template_height;
    std::string template_filename;
    std::string window_name;
    ros::NodeHandle local_nh("~");

    local_nh.param("child_frame_id", matching_frame, std::string("matching"));
    local_nh.param("object_width",  template_width,  0.06);
    local_nh.param("object_height", template_height, 0.0739);
    local_nh.param("relative_pose", _pose_str, std::string("0 0 0 0 0 0 1"));
    std::string default_template_file_name;
    rospack::ROSPack rp;
    try {
      rospack::Package *p = rp.get_pkg("opencv2");
      if (p!=NULL) default_template_file_name = p->path + std::string("/opencv/share/opencv/doc/opencv-logo2.png");
    } catch (std::runtime_error &e) {
    }
    local_nh.param("template_filename", template_filename, default_template_file_name);
    local_nh.param("reprojection_threshold", _reprojection_threshold, 3.0);
    local_nh.param("distanceratio_threshold", _distanceratio_threshold, 0.49);
    local_nh.param("error_threshold", _err_thr, 50.0);
    local_nh.param("theta_step", _th_step, 5.0);
    local_nh.param("phi_step", _phi_step, 5.0);
    local_nh.param("viewer_window", _viewer, true);
    local_nh.param("window_name", window_name, std::string("sample1"));
    local_nh.param("autosize", _autosize, false);
    local_nh.param("publish_null_object_detection", pnod, false);

    _first_sample_change = false;

    // make one template
    cv::Mat template_img;
    template_img = cv::imread (template_filename, 1);
    if ( template_img.empty()) {
      ROS_ERROR ("template picture <%s> cannot read. template picture is not found or uses unsuported format.", template_filename.c_str());
    }

    // relative pose
    std::vector<double> rv(7);
    std::istringstream iss(_pose_str);
    for(int i=0; i<7; i++)
      iss >> rv[i];
    tf::Transform transform(tf::Quaternion(rv[3],rv[4],rv[5],rv[6]),
    			    tf::Vector3(rv[0], rv[1], rv[2]));

    // add the image to template list 
    add_new_template(template_img, window_name, transform,
		     template_width, template_height, _th_step, _phi_step);

  } // initialize


  cv::Mat make_homography(cv::Mat src, cv::Mat rvec, cv::Mat tvec,
			  double template_width, double template_height, cv::Size &size){

    cv::Point3f coner[4] = {cv::Point3f(-(template_width/2.0), -(template_height/2.0),0),
			    cv::Point3f(template_width/2.0,-(template_height/2.0),0),
			    cv::Point3f(template_width/2.0,template_height/2.0,0),
			    cv::Point3f(-(template_width/2.0),template_height/2.0,0)};
    cv::Mat coner_mat (cv::Size(4, 1), CV_32FC3, coner);
    std::vector<cv::Point2f> coner_img_points;

    cv::projectPoints(coner_mat, rvec, tvec, _cam_mat,
		      _cam_dist,
		      coner_img_points);

    float x_min = 10000, x_max = 0;
    float y_min = 10000, y_max = 0;
    for (int i = 0; i < (int)coner_img_points.size(); i++){
      x_min = std::min(x_min, coner_img_points.at(i).x);
      x_max = std::max(x_max, coner_img_points.at(i).x);
      y_min = std::min(y_min, coner_img_points.at(i).y);
      y_max = std::max(y_max, coner_img_points.at(i).y);
    }

    std::vector<cv::Point2f> coner_img_points_trans;
    for (int i = 0; i < (int)coner_img_points.size(); i++){
      cv::Point2f pt_tmp(coner_img_points.at(i).x - x_min,
			 coner_img_points.at(i).y - y_min);
      coner_img_points_trans.push_back(pt_tmp);
    }

    cv::Point2f template_points[4] = {cv::Point2f(0,0),
				      cv::Point2f(src.size().width,0),
				      cv::Point2f(src.size().width,src.size().height),
				      cv::Point2f(0,src.size().height)};
    cv::Mat template_points_mat (cv::Size(4, 1), CV_32FC2, template_points);

    size = cv::Size(x_max - x_min, y_max - y_min);
    return cv::findHomography(template_points_mat, cv::Mat(coner_img_points_trans), 0, 3);
  }


  int make_warped_images (cv::Mat src, std::vector<cv::Mat> &imgs,
			  std::vector<cv:: Mat> &Mvec,
			  double template_width, double template_height,
			  double th_step, double phi_step){

    std::vector<cv::Size> sizevec;

    for (int i = (int)((-3.14/4.0)/th_step); i*th_step < 3.14/4.0; i++){
      for (int j = (int)((-3.14/4.0)/phi_step); j*phi_step < 3.14/4.0; j++){
	double fR3[3], fT3[3];
	cv::Mat rvec(3, 1, CV_64FC1, fR3);
	cv::Mat tvec(3, 1, CV_64FC1, fT3);

	tf::Quaternion quat(0, th_step*i, phi_step*j);
	fR3[0] = quat.getAxis().x() * quat.getAngle();
	fR3[1] = quat.getAxis().y() * quat.getAngle();
	fR3[2] = quat.getAxis().z() * quat.getAngle();
	fT3[0] = 0;
	fT3[1] = 0;
	fT3[2] = 0.5;

	cv::Mat M;
	cv::Size size;
	M = make_homography(src, rvec, tvec, template_width, template_height, size);
	Mvec.push_back(M);
	sizevec.push_back(size);
      }
    }

    for (int i = 0; i < (int)Mvec.size(); i++){
      cv::Mat dst;
      cv::warpPerspective(src, dst, Mvec.at(i), sizevec.at(i),
			  CV_INTER_LINEAR, IPL_BORDER_CONSTANT, 0);
      imgs.push_back(dst);
    }
    return 0;
  }

  bool add_new_template(cv::Mat img, std::string typestr, tf::Transform relative_pose,
			double template_width, double template_height,
			double theta_step=5.0, double phi_step=5.0)
  {
    std::vector<cv::Mat> imgs;
    std::vector<cv::Mat> Mvec;
    make_warped_images(img, imgs, Mvec,
		       template_width, template_height, theta_step, phi_step);

    for (int i = 0; i < (int)imgs.size(); i++){
      std::string type = typestr;
      if(imgs.size() > 1) {
	char chr[20];
	sprintf(chr, "%d", i);
	type += "_" + std::string(chr);
      }

      Matching_Template * tmplt = 
	new Matching_Template(imgs.at(i), type,
			      img.size().width, img.size().height,
			      template_width, template_height,
			      relative_pose, Mvec.at(i),
			      _reprojection_threshold,
			      _distanceratio_threshold,
			      (_viewer ? type : ""), _autosize);
      _templates.push_back(tmplt);
      if( _viewer )
	cv::namedWindow(type, _autosize ? CV_WINDOW_AUTOSIZE : 0);
      cvSetMouseCallback (type.c_str(), &cvmousecb, static_cast<void *>(_templates.back()));
    }
    return true;
  }

  bool settemplate_cb (posedetectiondb::SetTemplate::Request &req,
		       posedetectiondb::SetTemplate::Response &res){
      IplImage* iplimg;
      cv::Mat img;
      sensor_msgs::CvBridge bridge;

      bridge.fromImage (req.image, "bgr8");
      iplimg = bridge.toIpl();
      img = cv::Mat(iplimg);

      tf::Transform transform(tf::Quaternion(req.relativepose.orientation.x,
					     req.relativepose.orientation.y,
					     req.relativepose.orientation.z,
					     req.relativepose.orientation.w),
			      tf::Vector3(req.relativepose.position.x,
					  req.relativepose.position.y,
					  req.relativepose.position.z));

      // add the image to template list 
      add_new_template(img, req.type, transform,
		       req.dimx, req.dimy, 1.0, 1.0);
      return true;
  }


  void imagefeature_cb (const posedetection_msgs::ImageFeature0DConstPtr& msg){
    std::vector<cv::KeyPoint> sourceimg_keypoints;
    cv::Mat sourceimg_descriptors;
    image_geometry::PinholeCameraModel pcam;
    sensor_msgs::CvBridge bridge;
    std::vector<posedetection_msgs::Object6DPose> vo6p;
    posedetection_msgs::ObjectDetection od;

    bridge.fromImage (msg->image, "bgr8");
    IplImage* src_imgipl;
    src_imgipl = bridge.toIpl();
    cv::Mat src_img(src_imgipl);

    // from ros messages to keypoint and pin hole camera model
    features2keypoint (msg->features, sourceimg_keypoints, sourceimg_descriptors);
    pcam.fromCameraInfo(msg->info);

    _cam_mat = pcam.intrinsicMatrix().clone();
    _cam_dist = pcam.distortionCoeffs().clone();

    if ( !_initialized ) {
      // make template images from camera info
      initialize();
      std::cerr << "initialize templates done" << std::endl;
      _initialized = true;
    }

    if ( sourceimg_keypoints.size () < 2 ) {
      ROS_INFO ("The number of keypoints in source image is less than 2");
    }
    else {
      // make KDTree
      cv::flann::Index *ft = new cv::flann::Index(sourceimg_descriptors, cv::flann::KDTreeIndexParams(1));

      // matching and detect object
      ROS_INFO("_templates size: %d", (int)_templates.size());
      for (int i = 0; i < (int)_templates.size(); i++){
	posedetection_msgs::Object6DPose o6p;

	cv::Mat debug_img;
	if (_templates.at(i)->estimate_od(_client, src_img, sourceimg_keypoints, pcam, _err_thr, debug_img, ft, &o6p))
	  vo6p.push_back(o6p);

	// for debug Image topic
	cv_bridge::CvImage out_msg;
	out_msg.header   = msg->image.header;
	out_msg.encoding = "bgr8";
	out_msg.image    = debug_img;
	_debug_pub.publish(out_msg.toImageMsg());
      }
      delete ft;
      if (((int)vo6p.size() != 0) || pnod) {
	od.header.stamp = msg->image.header.stamp;
	od.header.frame_id = msg->image.header.frame_id;
	od.objects = vo6p;
	_pub.publish(od);
	_pub_agg.publish(od);
      }
    }
    // BOOST_FOREACH(Matching_Template* mt, _templates) {
    //   std::cerr << "templates_ -> " << mt << std::endl;
    // }
    cv::waitKey( 10 );
  }

  /* if there are subscribers of the output topic -> do work
	 else if -> unregister all topics this node subscribing
   */
  void check_subscribers()
  {
	if(_pub.getNumSubscribers() == 0) {
	  if(_sub)
		_sub.shutdown();
	} else {
	  if(!_sub)
		_sub = _n.subscribe("ImageFeature0D", 1,
				    &PointPoseExtractor::imagefeature_cb, this);
	}
  }

};  // the end of definition of class PointPoseExtractor


std::vector<Matching_Template *> PointPoseExtractor::_templates;


int main (int argc, char **argv){
  ros::init (argc, argv, "PointPoseExtractor");

  PointPoseExtractor matcher;

  ros::Rate r(10); // 10 hz
  while(ros::ok()) {
	ros::spinOnce();
	matcher.check_subscribers();
	r.sleep();
  }

  return 0;
}
