#include <uWS/uWS.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "helpers.h"
#include "spline.h"
#include "json.hpp"

// for convenience
using nlohmann::json;
using std::string;
using std::vector;
using std::cout;
using std::endl;

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  std::ifstream in_map_(map_file_.c_str(), std::ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    std::istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }

  // Lanes are as follows:
  //                            0     1      2
  // |  -10  |  -6  |  -2  |0|  2  |  6  |  10  |
  //
  // We start at lane 1:
  int lane = 1;

  // Initalize ego-vehicles velocity:
  double ego_v = 0.0;

  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,
               &map_waypoints_dx,&map_waypoints_dy, &lane, &ego_v]
              (uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
               uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);

        string event = j[0].get<string>();

        if (event == "telemetry") {
          // j[1] is the data JSON object

          // Main car's localization Data
          double car_x = j[1]["x"];
          double car_y = j[1]["y"];
          double car_s = j[1]["s"];
          double car_d = j[1]["d"];
          double car_yaw = j[1]["yaw"];
          double car_speed = j[1]["speed"];

          // Previous path data given to the Planner
          auto previous_path_x = j[1]["previous_path_x"];
          auto previous_path_y = j[1]["previous_path_y"];

          // Previous path's end s and d values
          double end_path_s = j[1]["end_path_s"];
          double end_path_d = j[1]["end_path_d"];

          // Sensor Fusion Data, a list of all other cars on the same side
          //   of the road.
          auto sensor_fusion = j[1]["sensor_fusion"];

          json msgJson;

          vector<double> next_x_vals;
          vector<double> next_y_vals;

          // Store previous path size:
          int prev_size = previous_path_x.size();

          // Set target reference velocity (max velocity for our car to reach):
          const double REF_VEL = 49.5;  // MPH

          // If previous path exists, continue from there:
          if (prev_size > 0) {
            car_s = end_path_s;
          }

          // Analyse nearby cars (true if near me)
          // {TO THE LEFT, AHEAD, TO THE RIGHT}
          vector<bool> cars_nearby = {false, false, false};

          // Go over the cars detected using our sensor fusion data:
          for (int i = 0; i < sensor_fusion.size(); ++i) {
            int id = sensor_fusion[i][0];
            double x = sensor_fusion[i][1];
            double y = sensor_fusion[i][2];
            double vx = sensor_fusion[i][3];
            double vy = sensor_fusion[i][4];
            double s = sensor_fusion[i][5];
            double d = sensor_fusion[i][6];
            // cout << "car id [" << id << "]: (x,y) = " << "(" << x << "," << y << "), (vx,vy) = " << "(" << vx << "," << vy << "), (s,d) = " << "(" << s << "," << d << ")" << endl;

            // Check car's lane:
            int car_lane = -1;
            if (0 <= d && d < 4) {  // Left lane
              car_lane = 0;
            } else if (4 <= d && d < 8) {  // Center lane
              car_lane = 1;
            } else if (8 <= d && d < 12) {  // Right lane
              car_lane = 2;
            }

            // Calculate car speed and s position:
            double check_speed = sqrt(vx * vx + vy * vy);
            double check_car_s = sensor_fusion[i][5];

            // Assess car's s position based on its speed and the previous trajectory size:
            check_car_s += ((double) prev_size * 0.02 * check_speed);

            // Store the s position difference between the car and our ego car:
            double s_diff = check_car_s - car_s;

            // Store the difference between the lanes:
            int lane_diff = car_lane - lane;

              // If the car is to our left or right
            if (lane_diff == -1 || lane_diff == 1) {
              if (fabs(s_diff) < 30) {  // "Danger" zone (s difference is less than 30m)
                cars_nearby[lane_diff + 1] = true;
              }
            } else if (lane_diff == 0) {  // If the car is in our lane
              if ((check_car_s > car_s) && (s_diff < 30)) {  // Car is ahead & in the 30m range
                cars_nearby[lane_diff + 1] = true;
              }
            }
          }  // END IF sensor_fusion


          // Lane change state machine:
          if (cars_nearby[1]) {  // Car ahead of me
              if (lane > 0 && !cars_nearby[0]) {  // Not in left-most lane & no car on the left
                  lane--;  // Go left
              } else if (lane < 2 && !cars_nearby[2]) {  // Not in right-most lane & no car on the right
                  lane++;  // Go right
              } else {  // Can't lane change & too close
                  ego_v -= 0.224;  // Smooth deceleration
              }
          } else if (ego_v < REF_VEL) {  // Haven't reached target speed yet
              ego_v += 0.448;
          }

          // Create a list of widely spaced (x, y) waypoints, evenly spaced at 30m
          // Later we will interpolate these waypoints with spline
          // and fill it in with more points that control speed
          vector<double> ptsx;
          vector<double> ptsy;

          // Reference x, y, yaw states
          // either we will reference the starting point as where the car is
          // or at the previous paths end point
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          // If previous size is almost empty, use the car as starting reference:
          if (prev_size < 2) {
            // Use 2 points that make the path tangent to the car:
            double prev_car_x = car_x - cos(car_yaw);
            double prev_car_y = car_y - sin(car_yaw);

            ptsx.push_back(prev_car_x);
            ptsx.push_back(car_x);

            ptsy.push_back(prev_car_y);
            ptsy.push_back(car_y);
          }
          else {  // Use the previous path's end point as starting reference
            // Redefine reference state as previous path end point:
            ref_x = previous_path_x[prev_size - 1];
            ref_y = previous_path_y[prev_size - 1];

            double ref_x_prev = previous_path_x[prev_size - 2];
            double ref_y_prev = previous_path_y[prev_size - 2];
            ref_yaw = atan2(ref_y - ref_y_prev, ref_x - ref_x_prev);

            // Use 2 points that make the path tangent to the previous path's end point:
            ptsx.push_back(ref_x_prev);
            ptsx.push_back(ref_x);

            ptsy.push_back(ref_y_prev);
            ptsy.push_back(ref_y);
          }

          // Add evenly 30m spaced points ahead of the starting reference in Frenet space:
          vector<double> next_wp0 = getXY(car_s+30, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp1 = getXY(car_s+60, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);
          vector<double> next_wp2 = getXY(car_s+90, (2+4*lane), map_waypoints_s, map_waypoints_x, map_waypoints_y);

          ptsx.push_back(next_wp0[0]);
          ptsx.push_back(next_wp1[0]);
          ptsx.push_back(next_wp2[0]);

          ptsy.push_back(next_wp0[1]);
          ptsy.push_back(next_wp1[1]);
          ptsy.push_back(next_wp2[1]);

          for (int i = 0; i < ptsx.size(); ++i) {
            // Shift car reference angle to 0 degrees:
            double shift_x = ptsx[i] - ref_x;
            double shift_y = ptsy[i] - ref_y;

            ptsx[i] = (shift_x*cos(0-ref_yaw) - shift_y*sin(0-ref_yaw));
            ptsy[i] = (shift_x*sin(0-ref_yaw) + shift_y*cos(0-ref_yaw));
          }

          // Create a spline:
          tk::spline spl;

          // Set (x, y) points to the spline:
          spl.set_points(ptsx, ptsy);

          // Start with all of the previous path points from last time:
          for (int i = 0; i < prev_size; ++i) {
            next_x_vals.push_back(previous_path_x[i]);
            next_y_vals.push_back(previous_path_y[i]);
          }

          // Calculate how to break up spline points so that we travel at our desired reference velocity:
          double target_x = 30.0;
          double target_y = spl(target_x);
          double target_dist = sqrt((target_x)*(target_x) + (target_y)*(target_y));

          double x_add_on = 0.0;

          // Fill up the rest of our path planner after filling it with previous points, here we will always output 50 points:
          for (int i = 1; i <= 50-prev_size; ++i) {
            double N = (target_dist / (0.02 * ego_v / 2.24));
            double x_pt = x_add_on + (target_x)/N;
            double y_pt = spl(x_pt);

            x_add_on = x_pt;

            double x_ref = x_pt;
            double y_ref = y_pt;

            // Rotate back to normal after rotating it earlier:
            x_pt = (x_ref * cos(ref_yaw) - y_ref * sin(ref_yaw));
            y_pt = (x_ref * sin(ref_yaw) + y_ref * cos(ref_yaw));

            x_pt += ref_x;
            y_pt += ref_y;

            next_x_vals.push_back(x_pt);
            next_y_vals.push_back(y_pt);
          }

          // Actuation:
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
        }  // end "telemetry" if
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }  // end websocket if
  }); // end h.onMessage

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }

  h.run();
}
