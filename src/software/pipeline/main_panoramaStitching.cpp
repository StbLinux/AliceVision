// This file is part of the AliceVision project.
// Copyright (c) 2017 AliceVision contributors.
// Copyright (c) 2012 openMVG contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <aliceVision/sfmData/SfMData.hpp>
#include <aliceVision/sfmDataIO/sfmDataIO.hpp>
#include <aliceVision/sfm/pipeline/regionsIO.hpp>
#include <aliceVision/feature/imageDescriberCommon.hpp>
#include <aliceVision/sfm/pipeline/panorama/ReconstructionEngine_panorama.hpp>
#include <aliceVision/sfm/utils/alignment.hpp>
#include <aliceVision/system/Timer.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/system/cmdline.hpp>
#include <aliceVision/image/all.hpp>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>

#include <cstdlib>

// These constants define the current software version.
// They must be updated when the command line is changed.
#define ALICEVISION_SOFTWARE_VERSION_MAJOR 1
#define ALICEVISION_SOFTWARE_VERSION_MINOR 0

using namespace aliceVision;

namespace po = boost::program_options;
namespace fs = boost::filesystem;

float sigmoid(float x, float sigwidth, float sigMid)
{
  return 1.0f / (1.0f + expf(10.0f * ((x - sigMid) / sigwidth)));
}

/**
 * @brief Function to map equirectangular coordinates onto a world unit vector according a spherical projection
 */
namespace SphericalMapping
{
  Vec3 get3DPoint(const Vec2& pos2d, int width, int height)
  {
    const double x = pos2d(0) - width;
    const double y = height/2.0 - pos2d(1);

    const double longitude = M_PI * 2.0 * x / width;  // between -PI and PI
    const double latitude = M_PI * y / height;        // between -PI/2 and PI/2

    const double Px = cos(latitude) * cos(longitude);
    const double Py = cos(latitude) * sin(longitude);
    const double Pz = sin(latitude);

    return Vec3(Px, Py, Pz);
  }
}

inline std::istream& operator>>(std::istream& in, std::pair<int, int>& out)
{
    in >> out.first;
    in >> out.second;
    return in;
}

int main(int argc, char **argv)
{
  // command-line parameters

  std::string verboseLevel = system::EVerboseLevel_enumToString(system::Logger::getDefaultVerboseLevel());
  std::string sfmDataFilename;
  std::string outputPanorama;

  float scaleFactor = 0.2f;
  std::pair<int, int> panoramaSize = {0, 0};
  bool fisheyeMasking = false;
  float fisheyeMaskingMargin = 0.05f;
  float transitionSize = 10.0f;

  po::options_description allParams(
    "Perform panorama stiching of cameras around a nodal point for 360° panorama creation.\n"
    "AliceVision PanoramaStitching");

  po::options_description requiredParams("Required parameters");
  requiredParams.add_options()
    ("input,i", po::value<std::string>(&sfmDataFilename)->required(),
      "SfMData file.")
    ("output,o", po::value<std::string>(&outputPanorama)->required(),
      "Path of the output folder.");

  po::options_description optionalParams("Optional parameters");
  optionalParams.add_options()
    ("scaleFactor", po::value<float>(&scaleFactor)->default_value(scaleFactor),
      "Scale factor to resize the output resolution (e.g. 0.5 for downscaling to half resolution).")
    ("fisheyeMasking", po::value<bool>(&fisheyeMasking)->default_value(fisheyeMasking),
      "For fisheye images, skip the invalid pixels on the borders.")
    ("fisheyeMaskingMargin", po::value<float>(&fisheyeMaskingMargin)->default_value(fisheyeMaskingMargin),
      "Margin for fisheye images (in percentage of the image).")
    ("transitionSize", po::value<float>(&transitionSize)->default_value(transitionSize),
      "Size of the transition between images (in pixels).")
    //("panoramaSize", po::value<std::pair<int, int>>(&panoramaSize)->default_value(panoramaSize),
    //  "Image size of the output panorama image file.")
    ;

  po::options_description logParams("Log parameters");
  logParams.add_options()
    ("verboseLevel,v", po::value<std::string>(&verboseLevel)->default_value(verboseLevel),
      "verbosity level (fatal, error, warning, info, debug, trace).");

  allParams.add(requiredParams).add(optionalParams).add(logParams);

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, allParams), vm);

    if(vm.count("help") || (argc == 1))
    {
      ALICEVISION_COUT(allParams);
      return EXIT_SUCCESS;
    }
    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    ALICEVISION_CERR("ERROR: " << e.what());
    ALICEVISION_COUT("Usage:\n\n" << allParams);
    return EXIT_FAILURE;
  }

  ALICEVISION_COUT("Program called with the following parameters:");
  ALICEVISION_COUT(vm);

  // set verbose level
  system::Logger::get()->setLogLevel(verboseLevel);

  // load input SfMData scene
  sfmData::SfMData sfmData;
  if(!sfmDataIO::Load(sfmData, sfmDataFilename, sfmDataIO::ESfMData(sfmDataIO::VIEWS|sfmDataIO::INTRINSICS|sfmDataIO::EXTRINSICS)))
  {
    ALICEVISION_LOG_ERROR("The input SfMData file '" << sfmDataFilename << "' cannot be read.");
    return EXIT_FAILURE;
  }

  auto validViews = sfmData.getValidViews();
  int nbCameras = sfmData.getValidViews().size();

  ALICEVISION_LOG_INFO(nbCameras << " loaded from " << sfmDataFilename);
  if(nbCameras == 0)
  {
    ALICEVISION_LOG_ERROR("Failed to get valid cameras from input images.");
    return -1;
  }

  if(panoramaSize.first == 0 || panoramaSize.second == 0)
  {
    sfmData::EEXIFOrientation orientation = sfmData.getView(*validViews.begin()).getMetadataOrientation();
    if(panoramaSize.first != 0 || panoramaSize.second != 0)
    {
      int s = std::max(panoramaSize.first, panoramaSize.second);
      panoramaSize.first = panoramaSize.second = s;
    }
    else
    {
      ALICEVISION_LOG_INFO("Automatic panorama size choice.");
      // TODO: better heuristic to decide the output resolution
      panoramaSize.first = 0;
      for(auto& viewIt: sfmData.getViews())
      {
        IndexT viewId = viewIt.first;
        const sfmData::View& view = *viewIt.second.get();
        if(!sfmData.isPoseAndIntrinsicDefined(&view))
          continue;

        if(orientation == sfmData::EEXIFOrientation::RIGHT ||
           orientation == sfmData::EEXIFOrientation::LEFT ||
           orientation == sfmData::EEXIFOrientation::RIGHT_REVERSED ||
           orientation == sfmData::EEXIFOrientation::LEFT_REVERSED)
        {
            panoramaSize.first += view.getHeight();
            panoramaSize.second = std::max(panoramaSize.second, int(view.getWidth()));
        }
        else
        {
            panoramaSize.first += view.getWidth();
            panoramaSize.second = std::max(panoramaSize.second, int(view.getHeight()));
        }
        ALICEVISION_LOG_INFO("Update output panorama size: " << panoramaSize.first << ", " << panoramaSize.second);
      }
    }
  }

  panoramaSize.first *= scaleFactor;
  panoramaSize.second *= scaleFactor;

  ALICEVISION_LOG_INFO("Output panorama size: " << panoramaSize.first << ", " << panoramaSize.second);

  // Create panorama buffer
  image::Image<image::RGBAfColor> imageOut(panoramaSize.first, panoramaSize.second, true, image::RGBAfColor(0.0f, 0.0f, 0.0f, 0.0f));

  for(auto& viewIt: sfmData.getViews())
  {
    IndexT viewId = viewIt.first;
    const sfmData::View& view = *viewIt.second.get();
    if(!sfmData.isPoseAndIntrinsicDefined(&view))
      continue;

    const sfmData::CameraPose camPose = sfmData.getPose(view);
    const camera::IntrinsicBase& intrinsic = *sfmData.getIntrinsicPtr(view.getIntrinsicId());

    std::string imagePath = view.getImagePath();

    // Image RGB(A)f
    image::Image<image::RGBfColor> imageIn;

    ALICEVISION_LOG_INFO("Reading " << imagePath);
    image::readImage(imagePath, imageIn, image::EImageColorSpace::LINEAR);


    const float maxRadius = std::min(imageIn.Width(), imageIn.Height()) * 0.5f * (1.0 - fisheyeMaskingMargin);
    const float blurWidth = maxRadius * transitionSize;
    const float blurMid = maxRadius - transitionSize/2.f;
    const Vec2i center(imageIn.Width()/2.f, imageIn.Height()/2.f);

    const image::Sampler2d<image::SamplerLinear> sampler;

    for(int y = 0; y < imageOut.Height(); ++y)
    {
      for(int x = 0; x < imageOut.Width(); ++x)
      {
        // equirectangular to unit vector
        // Vec3 ray = SphericalMapping::get3DPoint(Vec2(y,x), imageOut.Height(), imageOut.Width());
        Vec3 ray = SphericalMapping::get3DPoint(Vec2(x,y), imageOut.Width(), imageOut.Height());

        if(camPose.getTransform().depth(ray) < 0)
        {
          // point is not in front of the camera
          continue;
        }

        // unit vector to camera
        const Vec2 pix_disto = intrinsic.project(camPose.getTransform(), ray, true);

        if( pix_disto(0) < 0 || pix_disto(0) >= imageIn.Width() ||
            pix_disto(1) < 0 || pix_disto(1) >= imageIn.Height())
        {
          // the pixel is out of the image
          continue;
        }

        float contribution = 1.0f;
        if(fisheyeMasking)
        {
          const float dist = std::sqrt((pix_disto(0)-center(0)) * (pix_disto(0)-center(0)) + (pix_disto(1)-center(1)) * (pix_disto(1)-center(1)));
          if(dist > maxRadius)
          {
              // contribution = 0.0f;
              continue;
          }
          else
          {
              contribution = sigmoid(dist, transitionSize, blurMid);
          }
        }

        const image::RGBfColor pixel = sampler(imageIn, pix_disto(1), pix_disto(0));
        if(contribution > 0.0f)
        {
          imageOut(y, x).r() += pixel.r() * contribution;
          imageOut(y, x).g() += pixel.g() * contribution;
          imageOut(y, x).b() += pixel.b() * contribution;
          imageOut(y, x).a() += contribution;
        }
      }
    }
  }

  for(int y = 0; y < imageOut.Height(); ++y)
  {
    for(int x = 0; x < imageOut.Width(); ++x)
    {
      image::RGBAfColor& pixel = imageOut(y, x);
      if(pixel.a() > 0.0001f)
      {
        pixel.r() /= pixel.a();
        pixel.g() /= pixel.a();
        pixel.b() /= pixel.a();
        // pixel.a() = 1.0f; // TMP: keep the alpha with the contribution for debugging
      }
    }
  }

  image::writeImage(outputPanorama, imageOut, image::EImageColorSpace::AUTO);

  return EXIT_SUCCESS;
}