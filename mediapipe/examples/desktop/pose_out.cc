// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// An example of sending OpenCV webcam frames into a MediaPipe graph.

#include "mediapipe/framework/calculator_framework.h"
#include "mediapipe/framework/formats/image_frame.h"
#include "mediapipe/framework/formats/image_frame_opencv.h"
#include "mediapipe/framework/port/commandlineflags.h"
#include "mediapipe/framework/port/file_helpers.h"
#include "mediapipe/framework/port/opencv_highgui_inc.h"
#include "mediapipe/framework/port/opencv_imgproc_inc.h"
#include "mediapipe/framework/port/opencv_video_inc.h"
#include "mediapipe/framework/port/parse_text_proto.h"
#include "mediapipe/framework/port/status.h"

//Take stream from /mediapipe/graphs/hand_tracking/hand_detection_desktop_live.pbtxt
// RendererSubgraph - LANDMARKS:hand_landmarks
#include "mediapipe/calculators/util/landmarks_to_render_data_calculator.pb.h"
#include "mediapipe/framework/formats/landmark.pb.h"
//...................................................................
// try to add rect code into this 
#include "mediapipe/framework/formats/rect.pb.h"
#include "mediapipe/calculators/util/rect_to_render_data_calculator.pb.h"
//...................................................................


#include <typeinfo>
#include <regex>
#include <vector>
#include "mediapipe/tools/generate_vector.h"

// input and output streams to be used/retrieved by calculators
constexpr char kInputStream[] = "input_video";
constexpr char kOutputStream[] = "output_video";
constexpr char kOutputLandmarksStream[] = "pose_landmarks";
constexpr char kWindowName[] = "MediaPipe";


// get the rect data in to our program..........................................
constexpr char kLandmarksRectStream[] = "roi_from_landmarks";
constexpr char kOutputFaceCountStream[] = "pose_count";
// get the rect data in to our program..........................................


// cli inputs
DEFINE_string(
    calculator_graph_config_file, "",
    "Name of file containing text format CalculatorGraphConfig proto.");
DEFINE_string(input_video_path, "",
              "Full path of video to load. "
              "If not provided, attempt to use a webcam.");
DEFINE_string(output_video_path, "",
              "Full path of where to save result (.mp4 only). "
              "If not provided, show result in a window.");

::mediapipe::Status RunMPPGraph() {
  std::string calculator_graph_config_contents;
  MP_RETURN_IF_ERROR(mediapipe::file::GetContents(
      FLAGS_calculator_graph_config_file, &calculator_graph_config_contents));
  LOG(INFO) << "Get calculator graph config contents: "
            << calculator_graph_config_contents;
  mediapipe::CalculatorGraphConfig config =
      mediapipe::ParseTextProtoOrDie<mediapipe::CalculatorGraphConfig>(
          calculator_graph_config_contents);

  LOG(INFO) << "Initialize the calculator graph.";
  mediapipe::CalculatorGraph graph;
  MP_RETURN_IF_ERROR(graph.Initialize(config));

  LOG(INFO) << "Initialize the camera or load the video.";
  cv::VideoCapture capture;
  const bool load_video = !FLAGS_input_video_path.empty();
  if (load_video) {
    capture.open(FLAGS_input_video_path);
  } else {
    capture.open(0);
  }
  RET_CHECK(capture.isOpened());

  cv::VideoWriter writer;
  const bool save_video = !FLAGS_output_video_path.empty();
  if (save_video) {
    LOG(INFO) << "Prepare video writer.";
    cv::Mat test_frame;
    capture.read(test_frame);                    // Consume first frame.
    capture.set(cv::CAP_PROP_POS_AVI_RATIO, 0);  // Rewind to beginning.
    writer.open(FLAGS_output_video_path,
                mediapipe::fourcc('a', 'v', 'c', '1'),  // .mp4
                capture.get(cv::CAP_PROP_FPS), test_frame.size());
    RET_CHECK(writer.isOpened());
  } else {
    cv::namedWindow(kWindowName, /*flags=WINDOW_AUTOSIZE*/ 1);
  }

  // pollers to retrieve streams from graph
  // output stream (i.e. rendered landmark frame)
  ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller image_poller,
                   graph.AddOutputStreamPoller(kOutputStream));
  // hand landmarks stream
    ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller landmarks_poller,
                   graph.AddOutputStreamPoller(kOutputLandmarksStream));
  //hand landmarks_rect stream.................................
  ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller poller_landmark_rect,
            graph.AddOutputStreamPoller(kLandmarksRectStream));
  ASSIGN_OR_RETURN(mediapipe::OutputStreamPoller rect_count_poller,
            graph.AddOutputStreamPoller(kOutputFaceCountStream));
  
  LOG(INFO) << "Start running the calculator graph.";
  MP_RETURN_IF_ERROR(graph.StartRun({}));

  LOG(INFO) << "Start grabbing and processing frames.";

  bool grab_frames = true;
  while (grab_frames) {
    // Capture opencv camera or video frame.
    cv::Mat camera_frame_raw;
    capture >> camera_frame_raw;
    if (camera_frame_raw.empty()) break;  // End of video.
    cv::Mat camera_frame;
    cv::cvtColor(camera_frame_raw, camera_frame, cv::COLOR_BGR2RGB);
    if (!load_video) {
      cv::flip(camera_frame, camera_frame, /*flipcode=HORIZONTAL*/ 1);
    }

    // Wrap Mat into an ImageFrame.
    auto input_frame = absl::make_unique<mediapipe::ImageFrame>(
        mediapipe::ImageFormat::SRGB, camera_frame.cols, camera_frame.rows,
        mediapipe::ImageFrame::kDefaultAlignmentBoundary);
    cv::Mat input_frame_mat = mediapipe::formats::MatView(input_frame.get());
    camera_frame.copyTo(input_frame_mat);

    // Send image packet into the graph.
    size_t frame_timestamp_us =
        (double)cv::getTickCount() / (double)cv::getTickFrequency() * 1e6;
    MP_RETURN_IF_ERROR(graph.AddPacketToInputStream(
        kInputStream, mediapipe::Adopt(input_frame.release())
                          .At(mediapipe::Timestamp(frame_timestamp_us))));

    // Get the graph result packet, or stop if that fails.
    mediapipe::Packet image_packet;
    if (!image_poller.Next(&image_packet)) break;
    auto& output_frame = image_packet.Get<mediapipe::ImageFrame>();


    mediapipe::Packet rect_count_packet;
    if (!rect_count_poller.Next(&rect_count_packet)) break;
    auto& rect_count = rect_count_packet.Get<int>();
    
    if(rect_count > 0){

      mediapipe::Packet landmark_packet;
      //Polling the poller to get landmark packet
      if (!landmarks_poller.Next(&landmark_packet)) break;
      
      auto& landmarks = landmark_packet.Get<mediapipe::NormalizedLandmarkList>();
      std::cout << "waiting...............\n";
      string str =  landmarks.DebugString();
      std::cout << "...............\n";
      // using regular expression to get the cordinate.
      vector<vector<double>> cordinate_collection;
      cordinate_collection = generate_vector(str, camera_frame.cols, camera_frame.rows);
      for(vector<double> cordinate : cordinate_collection){
          cout << cordinate[0] << " " <<cordinate[1] << endl;
      }      
      // you can check type using this code......
      // std:: cout << typeid(landmarks.DebugString()).name();
      //
      // using this code , you can get the list, the size will be 33.
      // landmarks.landmark_size();


      //hand landmarks_rect  print ....................................
      
      mediapipe::Packet landmark_rect_packet;
      if (!poller_landmark_rect.Next(&landmark_rect_packet)) break;
      auto& output_landmarks_rect = landmark_rect_packet.Get<::mediapipe::NormalizedRect>();
      //printout the hand rectangle
      std::cout << "start to received rct data\n";
      std::cout << output_landmarks_rect.DebugString();
    }

    // Convert back to opencv for display or saving.
    cv::Mat output_frame_mat = mediapipe::formats::MatView(&output_frame);
    cv::cvtColor(output_frame_mat, output_frame_mat, cv::COLOR_RGB2BGR);
    if (save_video) {
      writer.write(output_frame_mat);
    } else {
      cv::imshow(kWindowName, output_frame_mat);
      // Press any key to exit.
      const int pressed_key = cv::waitKey(5);
      if (pressed_key >= 0 && pressed_key != 255) grab_frames = false;
    }
  }

  LOG(INFO) << "Shutting down.";
  if (writer.isOpened()) writer.release();
  MP_RETURN_IF_ERROR(graph.CloseInputStream(kInputStream));
  return graph.WaitUntilDone();
}


int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  ::mediapipe::Status run_status = RunMPPGraph();
  if (!run_status.ok()) {
    std::cout << run_status.message();
    LOG(ERROR) << "Failed to run the graph: " << run_status.message();
    
  } else {
    LOG(INFO) << "Success!";
  }
  return 0;
}