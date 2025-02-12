// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.

// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/cameras/Camera_Common.hpp"
#include "openMVG/cameras/Cameras_Common_command_line_helper.hpp"
#include "openMVG/sfm/pipelines/sequential/sequential_SfM.hpp"
#include "openMVG/sfm/pipelines/sfm_features_provider.hpp"
#include "openMVG/sfm/pipelines/sfm_matches_provider.hpp"
#include "openMVG/sfm/sfm_data.hpp"
#include "openMVG/sfm/sfm_data_io.hpp"
#include "openMVG/sfm/sfm_report.hpp"
#include "openMVG/sfm/sfm_view.hpp"
#include "openMVG/system/timer.hpp"
#include "openMVG/types.hpp"

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

using namespace openMVG;
using namespace openMVG::cameras;
using namespace openMVG::sfm;

inline bool view_less(const std::pair<std::string, IndexT> &p1, const std::pair<std::string, IndexT> &p2) {
  return (p1.first.compare(p2.first) < 0);
}

std::map<IndexT, IndexT> ComputeOrder(std::set<IndexT> &v) {
  std::vector<IndexT> w;
  std::copy(v.begin(), v.end(), std::back_inserter(w));
  std::sort(w.begin(), w.end());

  std::map<IndexT, IndexT> res;
  for (IndexT idx=0; idx<w.size(); ++idx) {
    res[w[idx]] = idx;
  }

  return res;
}

void SortAndClean(SfM_Data &sfm_data_) {
  std::vector<std::pair<std::string, IndexT>> valid_view_imgs;
  std::set<IndexT> valid_intrinsic_ids;
  std::map<IndexT, IndexT> intrinsic_map;
  std::map<IndexT, IndexT> view_map;

  for (auto it = sfm_data_.GetViews().begin(); it != sfm_data_.GetViews().end(); ++it)
  {
    const View * view = it->second.get();
    if (!sfm_data_.IsPoseAndIntrinsicDefined(view))
    {
      continue;
    }

    valid_view_imgs.push_back(std::make_pair(view->s_Img_path, view->id_view));
    valid_intrinsic_ids.insert(view->id_intrinsic);
  }


  intrinsic_map = ComputeOrder(valid_intrinsic_ids);
  std::sort(valid_view_imgs.begin(), valid_view_imgs.end(), view_less);
  SfM_Data sfm_data;
  sfm_data.s_root_path = sfm_data_.s_root_path;

  for (IndexT idx=0; idx<valid_view_imgs.size(); idx++) {
    auto view = sfm_data_.views[valid_view_imgs[idx].second];
    sfm_data.poses[idx] = sfm_data_.poses[view->id_pose];
    view->id_view = idx;
    view->id_pose = idx;
    view->id_intrinsic = intrinsic_map[view->id_intrinsic];
    sfm_data.views[idx] = view;
    view_map[valid_view_imgs[idx].second] = idx;
  }

  for (auto& intrinsic : intrinsic_map) {
    sfm_data.intrinsics[intrinsic.second] = sfm_data_.intrinsics[intrinsic.first];
  }

  for (auto &landmark : sfm_data_.structure) {
    Observations obs_new;
    for (auto &observation : landmark.second.obs) {
      std::pair<IndexT, Observation> ob_new(view_map[observation.first], observation.second);
      obs_new.insert(ob_new);
    }
    sfm_data.structure[landmark.first].obs = std::move(obs_new);
    sfm_data.structure[landmark.first].X = std::move(landmark.second.X);
  }

  std::swap(sfm_data_, sfm_data);
}

/// From 2 given image file-names, find the two corresponding index in the View list
bool computeIndexFromImageNames(
  const SfM_Data & sfm_data,
  const std::pair<std::string,std::string>& initialPairName,
  Pair& initialPairIndex)
{
  if (initialPairName.first == initialPairName.second)
  {
    std::cerr << "\nInvalid image names. You cannot use the same image to initialize a pair." << std::endl;
    return false;
  }

  initialPairIndex = {UndefinedIndexT, UndefinedIndexT};

  /// List views filenames and find the one that correspond to the user ones:
  for (Views::const_iterator it = sfm_data.GetViews().begin();
    it != sfm_data.GetViews().end(); ++it)
  {
    const View * v = it->second.get();
    const std::string filename = stlplus::filename_part(v->s_Img_path);
    if (filename == initialPairName.first)
    {
      initialPairIndex.first = v->id_view;
    }
    else{
      if (filename == initialPairName.second)
      {
        initialPairIndex.second = v->id_view;
      }
    }
  }
  return (initialPairIndex.first != UndefinedIndexT &&
      initialPairIndex.second != UndefinedIndexT);
}


int main(int argc, char **argv)
{
  using namespace std;
  std::cout << "Sequential/Incremental reconstruction" << std::endl
            << " Perform incremental SfM (Initial Pair Essential + Resection)." << std::endl
            << std::endl;

  CmdLine cmd;

  std::string sSfM_Data_Filename;
  std::string sMatchesDir, sMatchFilename;
  std::string sOutDir = "";
  std::pair<std::string,std::string> initialPairString("","");
  std::string sIntrinsic_refinement_options = "ADJUST_ALL";
  int i_User_camera_model = PINHOLE_CAMERA_RADIAL3;
  uint32_t u_acransac_times = 4096;
  bool b_omit_angle_error = false;
  bool b_use_motion_priors = false;
  bool b_use_pba = false;

  cmd.add( make_option('i', sSfM_Data_Filename, "input_file") );
  cmd.add( make_option('m', sMatchesDir, "matchdir") );
  cmd.add( make_option('M', sMatchFilename, "match_file") );
  cmd.add( make_option('o', sOutDir, "outdir") );
  cmd.add( make_option('a', initialPairString.first, "initialPairA") );
  cmd.add( make_option('b', initialPairString.second, "initialPairB") );
  cmd.add( make_option('c', i_User_camera_model, "camera_model") );
  cmd.add( make_option('f', sIntrinsic_refinement_options, "refineIntrinsics") );
  cmd.add( make_option('A', u_acransac_times, "ACRANSAC_times"));
  cmd.add( make_switch('e', "omit_angle_error"));
  cmd.add( make_switch('p', "pba_option"));
  cmd.add( make_switch('P', "prior_usage") );

  try {
    if (argc == 1) throw std::string("Invalid parameter.");
    cmd.process(argc, argv);
  } catch (const std::string& s) {
    std::cerr << "Usage: " << argv[0] << '\n'
    << "[-i|--input_file] path to a SfM_Data scene\n"
    << "[-m|--matchdir] path to the matches that corresponds to the provided SfM_Data scene\n"
    << "[-o|--outdir] path where the output data will be stored\n"
    << "\n[Optional]\n"
    << "[-a|--initialPairA] filename of the first image (without path)\n"
    << "[-b|--initialPairB] filename of the second image (without path)\n"
    << "[-c|--camera_model] Camera model type for view with unknown intrinsic:\n"
      << "\t 1: Pinhole \n"
      << "\t 2: Pinhole radial 1\n"
      << "\t 3: Pinhole radial 3 (default)\n"
      << "\t 4: Pinhole radial 3 + tangential 2\n"
      << "\t 5: Pinhole fisheye\n"
      << "\t 6: Pinhole radial 1 pba\n"
    << "[-f|--refineIntrinsics] Intrinsic parameters refinement option\n"
      << "\t ADJUST_ALL -> refine all existing parameters (default) \n"
      << "\t NONE -> intrinsic parameters are held as constant\n"
      << "\t ADJUST_FOCAL_LENGTH -> refine only the focal length\n"
      << "\t ADJUST_PRINCIPAL_POINT -> refine only the principal point position\n"
      << "\t ADJUST_DISTORTION -> refine only the distortion coefficient(s) (if any)\n"
      << "\t -> NOTE: options can be combined thanks to '|'\n"
      << "\t ADJUST_FOCAL_LENGTH|ADJUST_PRINCIPAL_POINT\n"
      <<      "\t\t-> refine the focal length & the principal point position\n"
      << "\t ADJUST_FOCAL_LENGTH|ADJUST_DISTORTION\n"
      <<      "\t\t-> refine the focal length & the distortion coefficient(s) (if any)\n"
      << "\t ADJUST_PRINCIPAL_POINT|ADJUST_DISTORTION\n"
      <<      "\t\t-> refine the principal point position & the distortion coefficient(s) (if any)\n"
    << "[-P|--prior_usage] Enable usage of motion priors (i.e GPS positions) (default: false)\n"
    << "[-p|--pba_option] Enable usage of pba(default: false)\n"   
    << "[-M|--match_file] path to the match file to use.\n"
    << "[-e|--omit_angle_error] Enable usage of angle error(default false)\n"
    << "[-A|--ACRANSAC_times] Adjust AC-RANSAC times(default 4096)\n"
    << std::endl;

    std::cerr << s << std::endl;
    return EXIT_FAILURE;
  }
  
  b_use_pba = cmd.used('p');
  
  if ( !isValid(openMVG::cameras::EINTRINSIC(i_User_camera_model)) )  {
    std::cerr << "\n Invalid camera type" << std::endl;
    return EXIT_FAILURE;
  }

  const cameras::Intrinsic_Parameter_Type intrinsic_refinement_options =
    cameras::StringTo_Intrinsic_Parameter_Type(sIntrinsic_refinement_options);
  if (intrinsic_refinement_options == static_cast<cameras::Intrinsic_Parameter_Type>(0) )
  {
    std::cerr << "Invalid input for Bundle Adjusment Intrinsic parameter refinement option" << std::endl;
    return EXIT_FAILURE;
  }

  // Load input SfM_Data scene
  SfM_Data sfm_data;
  if (!Load(sfm_data, sSfM_Data_Filename, ESfM_Data(VIEWS|INTRINSICS))) {
    std::cerr << std::endl
      << "The input SfM_Data file \""<< sSfM_Data_Filename << "\" cannot be read." << std::endl;
    return EXIT_FAILURE;
  }

  if (b_use_pba && ((intrinsic_refinement_options & cameras::Intrinsic_Parameter_Type::ADJUST_PRINCIPAL_POINT)
                   != static_cast<cameras::Intrinsic_Parameter_Type>(0))){
    std::cout<< "Warning: PBA can not adjust principle point!" <<std::endl;
  }

  // Init the regions_type from the image describer file (used for image regions extraction)
  using namespace openMVG::features;
  const std::string sImage_describer = stlplus::create_filespec(sMatchesDir, "image_describer", "json");
  std::unique_ptr<Regions> regions_type = Init_region_type_from_file(sImage_describer);
  if (!regions_type)
  {
    std::cerr << "Invalid: "
      << sImage_describer << " regions type file." << std::endl;
    return EXIT_FAILURE;
  }

  // Features reading
  std::shared_ptr<Features_Provider> feats_provider = std::make_shared<Features_Provider>();
  if (!feats_provider->load(sfm_data, sMatchesDir, regions_type)) {
    std::cerr << std::endl
      << "Invalid features." << std::endl;
    return EXIT_FAILURE;
  }
  // Matches reading
  std::shared_ptr<Matches_Provider> matches_provider = std::make_shared<Matches_Provider>();
  if // Try to read the provided match filename or the default one (matches.f.txt/bin)
  (
    !(matches_provider->load(sfm_data, sMatchFilename) ||
      matches_provider->load(sfm_data, stlplus::create_filespec(sMatchesDir, "matches.f.txt")) ||
      matches_provider->load(sfm_data, stlplus::create_filespec(sMatchesDir, "matches.f.bin")))
  )
  {
    std::cerr << std::endl
      << "Invalid matches file." << std::endl;
    return EXIT_FAILURE;
  }

  if (sOutDir.empty())  {
    std::cerr << "\nIt is an invalid output directory" << std::endl;
    return EXIT_FAILURE;
  }

  if (!stlplus::folder_exists(sOutDir))
  {
    if (!stlplus::folder_create(sOutDir))
    {
      std::cerr << "\nCannot create the output directory" << std::endl;
    }
  }

  //---------------------------------------
  // Sequential reconstruction process
  //---------------------------------------

  openMVG::system::Timer timer;
  SequentialSfMReconstructionEngine sfmEngine(
    sfm_data,
    sOutDir,
    stlplus::create_filespec(sOutDir, "Reconstruction_Report.html"));

  // Configure the features_provider & the matches_provider
  sfmEngine.SetPba(b_use_pba);
  sfmEngine.SetFeaturesProvider(feats_provider.get());
  sfmEngine.SetMatchesProvider(matches_provider.get());

  // Configure reconstruction parameters
  sfmEngine.Set_Intrinsics_Refinement_Type(intrinsic_refinement_options);
  sfmEngine.SetUnknownCameraType(EINTRINSIC(i_User_camera_model));
  b_use_motion_priors = cmd.used('P');
  sfmEngine.Set_Use_Motion_Prior(b_use_motion_priors);
  b_omit_angle_error = cmd.used('e');
  sfmEngine.SetAngleError(b_omit_angle_error);
  sfmEngine.SetACRANSAC_times(u_acransac_times);

  // Handle Initial pair parameter
  if (!initialPairString.first.empty() && !initialPairString.second.empty())
  {
    Pair initialPairIndex;
    if (!computeIndexFromImageNames(sfm_data, initialPairString, initialPairIndex))
    {
        std::cerr << "Could not find the initial pairs <" << initialPairString.first
          <<  ", " << initialPairString.second << ">!\n";
      return EXIT_FAILURE;
    }
    sfmEngine.setInitialPair(initialPairIndex);
  }

  if (sfmEngine.Process())
  {
    SortAndClean(sfmEngine.Get_SfM_Data());
    std::cout << std::endl << " Total Ac-Sfm took (s): " << timer.elapsed() << std::endl;

    std::cout << "...Generating SfM_Report.html" << std::endl;
    Generate_SfM_Report(sfmEngine.Get_SfM_Data(),
      stlplus::create_filespec(sOutDir, "SfMReconstruction_Report.html"));

    //-- Export to disk computed scene (data & visualizable results)
    std::cout << "...Export SfM_Data to disk." << std::endl;
   /* Save(sfmEngine.Get_SfM_Data(),
      stlplus::create_filespec(sOutDir, "sfm_data", ".bin"),
      ESfM_Data(ALL));
   */	
    Save(sfmEngine.Get_SfM_Data(),
      stlplus::create_filespec(sOutDir, "cloud_and_poses", ".ply"),
      ESfM_Data(ALL));

    Save(sfmEngine.Get_SfM_Data(),
      stlplus::create_filespec(sOutDir, "sfm_data", ".json"),
      ESfM_Data(VIEWS | EXTRINSICS | INTRINSICS));

    Save(sfmEngine.Get_SfM_Data(),
      stlplus::create_filespec(sOutDir, "sfm_data_all", ".json"),
      ESfM_Data(ALL));

    return EXIT_SUCCESS;
  }
  return EXIT_FAILURE;
}
