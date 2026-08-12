/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// ★ 打开 ALOGV(2026-08-11,排查"相机 App 配不出会话"必需)。
//   上游默认是注释掉的 → NDEBUG 下 ALOGV 被**编译期整个消掉**,
//   连字符串都不在二进制里,`setprop log.tag.VirtualCameraDevice VERBOSE`
//   **也救不回来**(那只是运行时过滤)。实测 dump 二进制确认:
//   "Requested config doesn't match any supported input config" 等三条
//   ALOGV 字符串在 /system/bin/virtual_camera 里根本不存在。
//   而 isStreamCombinationSupported 里**大部分拒绝分支都是 ALOGV** ——
//   于是"配不出会话"在日志里完全是哑的,只能看到 App 3 秒后超时退出。
//   ⚠️ 这行是**排查用**;它只影响日志量,不改变任何逻辑。
#define LOG_NDEBUG 0
#define LOG_TAG "VirtualCameraDevice"
#include "VirtualCameraDevice.h"

#include <android_companion_virtualdevice_flags.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "CameraMetadata.h"
#include "VirtualCameraService.h"
#include "VirtualCameraSession.h"
#include "aidl/android/companion/virtualcamera/SupportedStreamConfiguration.h"
#include "aidl/android/companion/virtualcamera/VirtualCameraConfiguration.h"
#include "aidl/android/hardware/camera/common/Status.h"
#include "aidl/android/hardware/camera/device/CameraMetadata.h"
#include "aidl/android/hardware/camera/device/StreamConfiguration.h"
#include "android/binder_auto_utils.h"
#include "android/binder_status.h"
#include "system/camera_metadata.h"
#include "util/AidlUtil.h"
#include "util/MetadataUtil.h"
#include "util/Util.h"

namespace android {
namespace companion {
namespace virtualcamera {

using ::aidl::android::companion::virtualcamera::Format;
using ::aidl::android::companion::virtualcamera::IVirtualCameraCallback;
using ::aidl::android::companion::virtualcamera::LensFacing;
using ::aidl::android::companion::virtualcamera::SensorOrientation;
using ::aidl::android::companion::virtualcamera::SupportedStreamConfiguration;
using ::aidl::android::companion::virtualcamera::VirtualCameraConfiguration;
using ::aidl::android::companion::virtualcamera::VirtualCameraMetadata;
using ::aidl::android::hardware::camera::common::CameraResourceCost;
using ::aidl::android::hardware::camera::common::Status;
using AidlCameraMetadata =
    ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraDeviceSession;
using ::aidl::android::hardware::camera::device::ICameraInjectionSession;
using ::aidl::android::hardware::camera::device::Stream;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::camera::device::StreamRotation;
using ::aidl::android::hardware::camera::device::StreamType;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using HelperCameraMetadata =
    ::android::hardware::camera::common::helper::CameraMetadata;

namespace {

using namespace std::chrono_literals;

namespace flags = ::android::companion::virtualdevice::flags;

// Prefix of camera name - "device@1.1/virtual/{camera_id}"
const char* kDevicePathPrefix = "device@1.1/virtual/";

constexpr int32_t kMaxJpegSize = 13 * 1024 * 1024 /* 13MiB */;

constexpr std::chrono::nanoseconds kMaxFrameDuration =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        1e9ns / VirtualCameraDevice::kMinFps);

constexpr uint8_t kPipelineMaxDepth = 2;

constexpr int k30Fps = 30;

constexpr MetadataBuilder::ControlRegion kDefaultEmptyControlRegion{};

const std::array<Resolution, 5> kStandardJpegThumbnailSizes{
    Resolution(176, 144), Resolution(240, 144), Resolution(256, 144),
    Resolution(240, 160), Resolution(240, 180)};

const std::array<PixelFormat, 3> kOutputFormats{
    PixelFormat::IMPLEMENTATION_DEFINED, PixelFormat::YCBCR_420_888,
    PixelFormat::BLOB};

// The resolutions below will used to extend the set of supported output formats.
// Any resolution that fits within some supported input resolution in *both*
// dimensions is added to the set of supported output resolutions; a differing
// aspect ratio is handled by center-cropping at render time.
const std::array<Resolution, 10> kOutputResolutions{
    Resolution(320, 240),   Resolution(640, 360),  Resolution(640, 480),
    Resolution(720, 480),   Resolution(720, 576),  Resolution(800, 600),
    Resolution(1024, 576),  Resolution(1280, 720), Resolution(1280, 960),
    Resolution(1280, 1080),
};

std::vector<Resolution> getSupportedJpegThumbnailSizes(
    const std::vector<SupportedStreamConfiguration>& configs) {
  // ★ 2026-08-12:同样从"同比例"放宽到"逐维度装得下"。
  //   缩略图路径本来就走 renderIntoEglFramebuffer(带 viewport),
  //   现在那条路径会中心裁剪,所以非同比例的缩略图尺寸也能正确产出
  //   (顺带解决了这里原本的 TODO b/324383963 —— 不再需要 letterbox)。
  auto isSupportedByAnyInputConfig =
      [&configs](const Resolution thumbnailResolution) {
        return std::any_of(
            configs.begin(), configs.end(),
            [thumbnailResolution](const SupportedStreamConfiguration& config) {
              return thumbnailResolution.width <= config.width &&
                     thumbnailResolution.height <= config.height;
            });
      };

  std::vector<Resolution> supportedThumbnailSizes({Resolution(0, 0)});
  std::copy_if(kStandardJpegThumbnailSizes.begin(),
               kStandardJpegThumbnailSizes.end(),
               std::back_insert_iterator(supportedThumbnailSizes),
               isSupportedByAnyInputConfig);
  return supportedThumbnailSizes;
}

bool isSupportedOutputFormat(const PixelFormat pixelFormat) {
  return std::find(kOutputFormats.begin(), kOutputFormats.end(), pixelFormat) !=
         kOutputFormats.end();
}

std::vector<FpsRange> fpsRangesForInputConfig(
    const std::vector<SupportedStreamConfiguration>& configs) {
  std::set<FpsRange> availableRanges;

  for (const SupportedStreamConfiguration& config : configs) {
    availableRanges.insert(
        {.minFps = VirtualCameraDevice::kMinFps, .maxFps = config.maxFps});
    availableRanges.insert({.minFps = config.maxFps, .maxFps = config.maxFps});
  }

  if (std::any_of(configs.begin(), configs.end(),
                  [](const SupportedStreamConfiguration& config) {
                    return config.maxFps >= k30Fps;
                  })) {
    // Extend the set of available ranges with (minFps <= 15, 30) & (30, 30) as
    // required by CDD.
    availableRanges.insert(
        {.minFps = VirtualCameraDevice::kMinFps, .maxFps = k30Fps});
    availableRanges.insert({.minFps = k30Fps, .maxFps = k30Fps});
  }

  return std::vector<FpsRange>(availableRanges.begin(), availableRanges.end());
}

std::optional<Resolution> getMaxResolution(
    const std::vector<SupportedStreamConfiguration>& configs) {
  if (configs.empty()) {
    ALOGE(
        "%s: empty vector of supported configurations, cannot find largest "
        "resolution.",
        __func__);
    return std::nullopt;
  }

  // ★★ 这里原来是上游 AOSP 的一个 bug(2026-08-12 发现并修):
  //      return a.width * b.height < a.width * b.height;
  //    左右两边**完全一样**(都是 a.width * b.height)⇒ 恒为 false
  //    ⇒ max_element 永远返回第一个元素,不是最大的那个。
  //    (正确写法应是 a.width * a.height < b.width * b.height。)
  //
  //    以前所有输入档都同比例、且通常只有一档,所以"取第一个"碰巧没出事。
  //    现在我们要同时声明 16:9 和 4:3 两档,它就会真的报错值。
  //
  // ★★ 为什么**不**返回"逐维度包围盒"(我第一版就是那么写的,被 review 否掉):
  //    这个值会被当成虚拟相机的传感器尺寸
  //    (SENSOR_INFO_ACTIVE_ARRAY_SIZE / getMaxInputResolution),
  //    而 VirtualCameraCaptureResult.cc 里的 crop region 是**硬编码**成
  //    (0, 0, sensorW, sensorH) 的 —— 每帧都声称"我用满了整个 active array"。
  //
  //    包围盒可能是一个**根本不存在的分辨率**:
  //      16:9=1920x1080 与 4:3=1600x1200 ⇒ 包围盒 1920x1200,
  //      而任何一路输入 Surface 都不是这个尺寸 ⇒ 那些像素**不存在**。
  //    App 拿 crop region 去反算人脸框 / 点击对焦坐标就会系统性偏移,
  //    而且**画面本身看着是正常的**,极难定位。
  //
  //    ⇒ 取"面积最大的那一档**真实存在的**输入档",保证 active array
  //      永远对应一个真的能采到的尺寸,App 侧的换算至少自洽。
  auto itMax = std::max_element(configs.begin(), configs.end(),
                                [](const SupportedStreamConfiguration& a,
                                   const SupportedStreamConfiguration& b) {
                                  return a.width * a.height < b.width * b.height;
                                });

  return Resolution(itMax->width, itMax->height);
}

// Returns a map of unique resolution to maximum maxFps for all streams with
// that resolution.
std::map<Resolution, int> getResolutionToMaxFpsMap(
    const std::vector<SupportedStreamConfiguration>& configs) {
  std::map<Resolution, int> resolutionToMaxFpsMap;

  for (const SupportedStreamConfiguration& config : configs) {
    Resolution resolution(config.width, config.height);
    if (resolutionToMaxFpsMap.find(resolution) == resolutionToMaxFpsMap.end()) {
      resolutionToMaxFpsMap[resolution] = config.maxFps;
    } else {
      int currentMaxFps = resolutionToMaxFpsMap[resolution];
      resolutionToMaxFpsMap[resolution] = std::max(currentMaxFps, config.maxFps);
    }
  }

  std::map<Resolution, int> additionalResolutionToMaxFpsMap;
  // Add additional resolutions we can support by downscaling input streams with
  // same aspect ratio.
  for (const Resolution& outputResolution : kOutputResolutions) {
    for (const auto& [resolution, maxFps] : resolutionToMaxFpsMap) {
      if (resolutionToMaxFpsMap.find(outputResolution) !=
          resolutionToMaxFpsMap.end()) {
        // Resolution is already in the map, skip it.
        continue;
      }

      // ★ 2026-08-12:原来要求"同比例"才把这个输出档广播出去。
      //   这一处是**决定性的** —— 前面两道检查放开了,但如果这里不放开,
      //   相机 App 在能力表里根本看不到 4:3 的尺寸,也就永远不会去请求它,
      //   整个改动等于没做(而且看上去"没报错",极具迷惑性)。
      //
      //   现在渲染器能中心裁剪,只要输入档在**两个维度上都够大**,
      //   就能裁+缩出这个输出档 ⇒ 按逐维度覆盖来判定。
      //
      //   ⚠️ 仍然不能用 operator<(它比像素总数),必须逐维度比:
      //      1280x1080 的像素数比 1920x1080 少,但宽度要求一样、
      //      高度更高,从 1920x1080 裁不出来。
      if (outputResolution.width <= resolution.width &&
          outputResolution.height <= resolution.height &&
          !(outputResolution == resolution)) {
        // Lower-or-equal resolution in both dimensions: reachable by
        // center-cropping and/or downscaling the input.
        //
        // ★ 不能在这里 break(2026-08-12 review 抓出):
        //   break 会让结果取"**第一个**能覆盖的输入档"的 maxFps,
        //   而 resolutionToMaxFpsMap 是按像素数排序的 std::map ⇒
        //   拿到的是像素数最小那档的 fps。若两档 fps 不同
        //   (如 4:3 档 30fps、16:9 档 60fps),广播出去的 fps 会**偏低**,
        //   App 就看不到本来支持的高帧率。改成扫完所有档取 max。
        auto it = additionalResolutionToMaxFpsMap.find(outputResolution);
        if (it == additionalResolutionToMaxFpsMap.end()) {
          ALOGD(
              "Extending set of output resolutions with %dx%d (fits within "
              "supported input %dx%d; center-crop handles aspect difference).",
              outputResolution.width, outputResolution.height, resolution.width,
              resolution.height);
          additionalResolutionToMaxFpsMap[outputResolution] = maxFps;
        } else {
          it->second = std::max(it->second, maxFps);
        }
      }
    }
  }

  // Add all resolution we can achieve by downscaling to the map.
  resolutionToMaxFpsMap.insert(additionalResolutionToMaxFpsMap.begin(),
                               additionalResolutionToMaxFpsMap.end());

  return resolutionToMaxFpsMap;
}

// Populates the maxResolution and the outputConfigurations
// from the list of supported stream configs
status_t convertSupportedStreams(
    const std::vector<SupportedStreamConfiguration>& supportedInputConfig,
    Resolution& maxResolution,
    std::vector<MetadataBuilder::StreamConfiguration>& outputConfigurations) {
  std::optional<Resolution> resolution = getMaxResolution(supportedInputConfig);
  if (!resolution.has_value()) {
    return BAD_VALUE;
  }
  maxResolution.width = resolution.value().width;
  maxResolution.height = resolution.value().height;

  // Standard resolutions we can rescale/crop the streams to are added below
  // (see kOutputResolutions) — no longer restricted to matching aspect ratios
  // now that the render path center-crops.

  std::map<Resolution, int> resolutionToMaxFpsMap =
      getResolutionToMaxFpsMap(supportedInputConfig);

  // Add configurations for all unique input resolutions and output formats.
  for (const PixelFormat format : kOutputFormats) {
    std::transform(
        resolutionToMaxFpsMap.begin(), resolutionToMaxFpsMap.end(),
        std::back_inserter(outputConfigurations), [format](const auto& entry) {
          Resolution resolution = entry.first;
          int maxFps = entry.second;
          return MetadataBuilder::StreamConfiguration{
              .width = resolution.width,
              .height = resolution.height,
              .format = static_cast<int32_t>(format),
              .minFrameDuration = std::chrono::nanoseconds(1s) / maxFps,
              .minStallDuration = 0s};
        });
  }

  return OK;
}

std::unique_ptr<AidlCameraMetadata> createDefaultCameraCharacteristics(
    const std::vector<SupportedStreamConfiguration>& supportedInputConfig,
    const SensorOrientation sensorOrientation, const LensFacing lensFacing,
    const int32_t deviceId) {
  MetadataBuilder builder;
  builder
      .setSupportedHardwareLevel(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL)
      .setDeviceId(deviceId)
      .setFlashAvailable(false)
      .setLensFacing(
          static_cast<camera_metadata_enum_android_lens_facing>(lensFacing))
      .setAvailableFocalLengths({VirtualCameraDevice::kFocalLength})
      .setSensorOrientation(static_cast<int32_t>(sensorOrientation))
      .setSensorReadoutTimestamp(ANDROID_SENSOR_READOUT_TIMESTAMP_NOT_SUPPORTED)
      .setSensorTimestampSource(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN)
      .setSensorPhysicalSize(36.0, 24.0)
      .setAvailableAberrationCorrectionModes(
          {ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF})
      .setAvailableNoiseReductionModes({ANDROID_NOISE_REDUCTION_MODE_OFF})
      .setAvailableFaceDetectModes({ANDROID_STATISTICS_FACE_DETECT_MODE_OFF})
      .setAvailableStreamUseCases(
          {ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT,
           ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW,
           ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_STILL_CAPTURE,
           ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_RECORD,
           ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW_VIDEO_STILL,
           ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL})
      .setAvailableTestPatternModes({ANDROID_SENSOR_TEST_PATTERN_MODE_OFF})
      .setAvailableMaxDigitalZoom(1.0)
      .setControlAvailableModes({ANDROID_CONTROL_MODE_AUTO})
      .setControlAfAvailableModes({ANDROID_CONTROL_AF_MODE_OFF})
      .setControlAvailableSceneModes({ANDROID_CONTROL_SCENE_MODE_DISABLED})
      .setControlAvailableEffects({ANDROID_CONTROL_EFFECT_MODE_OFF})
      .setControlAvailableVideoStabilizationModes(
          {ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF})
      .setControlAeAvailableModes({ANDROID_CONTROL_AE_MODE_ON})
      .setControlAeAvailableAntibandingModes(
          {ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO})
      .setControlAeAvailableFpsRanges(
          fpsRangesForInputConfig(supportedInputConfig))
      .setControlMaxRegions(0, 0, 0)
      .setControlAfRegions({kDefaultEmptyControlRegion})
      .setControlAeRegions({kDefaultEmptyControlRegion})
      .setControlAwbRegions({kDefaultEmptyControlRegion})
      .setControlAeCompensationRange(0, 0)
      .setControlAeCompensationStep(camera_metadata_rational_t{0, 1})
      .setControlAwbLockAvailable(false)
      .setControlAeLockAvailable(false)
      .setControlAvailableAwbModes({ANDROID_CONTROL_AWB_MODE_AUTO})
      .setControlZoomRatioRange(/*min=*/1.0, /*max=*/1.0)
      .setCroppingType(ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY)
      .setJpegAvailableThumbnailSizes(
          getSupportedJpegThumbnailSizes(supportedInputConfig))
      .setMaxJpegSize(kMaxJpegSize)
      .setMaxFaceCount(0)
      .setMaxFrameDuration(kMaxFrameDuration)
      .setMaxNumberOutputStreams(
          VirtualCameraDevice::kMaxNumberOfRawStreams,
          VirtualCameraDevice::kMaxNumberOfProcessedStreams,
          VirtualCameraDevice::kMaxNumberOfStallStreams)
      .setRequestPartialResultCount(1)
      .setPipelineMaxDepth(kPipelineMaxDepth)
      .setSyncMaxLatency(ANDROID_SYNC_MAX_LATENCY_UNKNOWN)
      .setAvailableRequestKeys({ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
                                ANDROID_CONTROL_CAPTURE_INTENT,
                                ANDROID_CONTROL_AE_MODE,
                                ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
                                ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
                                ANDROID_CONTROL_AE_ANTIBANDING_MODE,
                                ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
                                ANDROID_CONTROL_AF_TRIGGER,
                                ANDROID_CONTROL_AF_MODE,
                                ANDROID_CONTROL_AWB_MODE,
                                ANDROID_SCALER_CROP_REGION,
                                ANDROID_CONTROL_EFFECT_MODE,
                                ANDROID_CONTROL_MODE,
                                ANDROID_CONTROL_SCENE_MODE,
                                ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
                                ANDROID_CONTROL_ZOOM_RATIO,
                                ANDROID_FLASH_MODE,
                                ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
                                ANDROID_JPEG_ORIENTATION,
                                ANDROID_JPEG_QUALITY,
                                ANDROID_JPEG_THUMBNAIL_QUALITY,
                                ANDROID_JPEG_THUMBNAIL_SIZE,
                                ANDROID_NOISE_REDUCTION_MODE,
                                ANDROID_STATISTICS_FACE_DETECT_MODE})
      .setAvailableResultKeys({
          ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
          ANDROID_CONTROL_AE_ANTIBANDING_MODE,
          ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
          ANDROID_CONTROL_AE_LOCK,
          ANDROID_CONTROL_AE_MODE,
          ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
          ANDROID_CONTROL_AE_STATE,
          ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
          ANDROID_CONTROL_AF_MODE,
          ANDROID_CONTROL_AF_STATE,
          ANDROID_CONTROL_AF_TRIGGER,
          ANDROID_CONTROL_AWB_LOCK,
          ANDROID_CONTROL_AWB_MODE,
          ANDROID_CONTROL_AWB_STATE,
          ANDROID_CONTROL_CAPTURE_INTENT,
          ANDROID_CONTROL_EFFECT_MODE,
          ANDROID_CONTROL_MODE,
          ANDROID_CONTROL_SCENE_MODE,
          ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
          ANDROID_STATISTICS_FACE_DETECT_MODE,
          ANDROID_FLASH_MODE,
          ANDROID_FLASH_STATE,
          ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
          ANDROID_JPEG_QUALITY,
          ANDROID_JPEG_THUMBNAIL_QUALITY,
          ANDROID_LENS_FOCAL_LENGTH,
          ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
          ANDROID_NOISE_REDUCTION_MODE,
          ANDROID_REQUEST_PIPELINE_DEPTH,
          ANDROID_SENSOR_TIMESTAMP,
          ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE,
          ANDROID_STATISTICS_LENS_SHADING_MAP_MODE,
          ANDROID_STATISTICS_SCENE_FLICKER,
      })
      .setAvailableCapabilities(
          {ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE});

  std::vector<MetadataBuilder::StreamConfiguration> outputConfigurations;
  Resolution maxResolution;
  if (convertSupportedStreams(supportedInputConfig, maxResolution,
                              outputConfigurations) != OK) {
    ALOGE(
        "Can not get max resolution from the input stream configs, output "
        "streams not configured!");
    return nullptr;
  }

  builder.setSensorActiveArraySize(0, 0, maxResolution.width,
                                   maxResolution.height);
  builder.setSensorPixelArraySize(maxResolution.width, maxResolution.height);

  ALOGV("Adding %zu output configurations to default CameraCharacteristics.",
        outputConfigurations.size());
  builder.setAvailableOutputStreamConfigurations(outputConfigurations);

  return builder.setAvailableCharacteristicKeys().build();
}

status_t updateStreamConfigurations(
    HelperCameraMetadata& metadataHelper,
    const std::vector<SupportedStreamConfiguration>& supportedInputConfig) {
  std::vector<MetadataBuilder::StreamConfiguration> outputConfigurations;
  Resolution maxResolution;

  status_t ret = convertSupportedStreams(supportedInputConfig, maxResolution,
                                         outputConfigurations);
  if (ret != OK) {
    ALOGE(
        "Can not get max resolution from the input stream configs, output "
        "streams not configured!");
    return ret;
  }

  auto activeArraySizeVec =
      std::vector<int32_t>({0, 0, maxResolution.width, maxResolution.height});
  ret = metadataHelper.update(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                              activeArraySizeVec.data(),
                              activeArraySizeVec.size());
  if (ret != OK) {
    ALOGE("Can not set SENSOR_INFO_ACTIVE_ARRAY_SIZE!");
    return ret;
  }
  auto pixelArraySizeVec =
      std::vector<int32_t>({maxResolution.width, maxResolution.height});
  ret =
      metadataHelper.update(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
                            pixelArraySizeVec.data(), pixelArraySizeVec.size());
  if (ret != OK) {
    ALOGE("Can not set ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE!");
    return ret;
  }

  ALOGV("Adding %zu output configurations to configured CameraCharacteristics.",
        outputConfigurations.size());
  std::vector<int32_t> metadataStreamConfigs;
  std::vector<int64_t> metadataMinFrameDurations;
  std::vector<int64_t> metadataStallDurations;

  convertStreamConfigurationsToMetadataValues(
      outputConfigurations, metadataStreamConfigs, metadataMinFrameDurations,
      metadataStallDurations);
  ret = metadataHelper.update(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                              metadataStreamConfigs.data(),
                              metadataStreamConfigs.size());
  if (ret != OK) {
    ALOGE("Can not set ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS!");
    return ret;
  }

  ret = metadataHelper.update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                              metadataMinFrameDurations.data(),
                              metadataMinFrameDurations.size());
  if (ret != OK) {
    ALOGE("Can not set ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS!");
    return ret;
  }

  ret = metadataHelper.update(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                              metadataStallDurations.data(),
                              metadataStallDurations.size());
  if (ret != OK) {
    ALOGE("Can not set ANDROID_SCALER_AVAILABLE_STALL_DURATIONS!");
    return ret;
  }

  return ret;
}

std::optional<AidlCameraMetadata> initCameraCharacteristics(
    const std::vector<SupportedStreamConfiguration>& supportedInputConfig,
    const SensorOrientation sensorOrientation, const LensFacing lensFacing,
    const std::optional<VirtualCameraMetadata>& configCameraCharacteristics,
    const int32_t deviceId) {
  if (!std::all_of(supportedInputConfig.begin(), supportedInputConfig.end(),
                   [](const SupportedStreamConfiguration& config) {
                     return isFormatSupportedForInput(
                         config.width, config.height, config.pixelFormat,
                         config.maxFps);
                   })) {
    ALOGE("%s: input configuration contains unsupported format", __func__);
    return std::nullopt;
  }

  std::unique_ptr<AidlCameraMetadata> aidlMetadata;
  // If the camera metadata is set by the VD owner, add the deviceId and pass it as it is
  if (configCameraCharacteristics.has_value()) {
    ALOGD("VirtualCameraDevice - using config CameraCharacteristics");
    AidlCameraMetadata deviceCameraMetadata;
    status_t ret = convertVirtualToDeviceCameraMetadata(
        configCameraCharacteristics.value(), deviceCameraMetadata);
    if (ret != OK) {
      ALOGE("Failed to convert virtual to device camera characteristics!");
      return AidlCameraMetadata();
    }

    HelperCameraMetadata metadataHelper = HelperCameraMetadata(
        clone_camera_metadata(reinterpret_cast<const camera_metadata_t*>(
            deviceCameraMetadata.metadata.data())));

    auto deviceIdVec = std::vector<int32_t>({deviceId});
    ret = metadataHelper.update(ANDROID_INFO_DEVICE_ID, deviceIdVec.data(),
                                deviceIdVec.size());
    if (ret != OK) {
      ALOGE(
          "Failed to update ANDROID_INFO_DEVICE_ID for camera "
          "characteristics!");
    }

    // internal values that can't be configured from external metadata
    auto jpegMaxSizeVec = std::vector<int32_t>({kMaxJpegSize});
    ret = metadataHelper.update(ANDROID_JPEG_MAX_SIZE, jpegMaxSizeVec.data(),
                                jpegMaxSizeVec.size());
    if (ret != OK) {
      ALOGE(
          "Failed to update ANDROID_JPEG_MAX_SIZE for camera characteristics!");
    }

    ret = updateStreamConfigurations(metadataHelper, supportedInputConfig);
    if (ret != OK) {
      ALOGE(
          "Failed to update output stream configurations for camera "
          "characteristics!");
    }

    aidlMetadata = cameraMetadataToHal(metadataHelper);
  } else {
    ALOGD("VirtualCameraDevice - createDefaultCameraCharacteristics");
    aidlMetadata = createDefaultCameraCharacteristics(
        supportedInputConfig, sensorOrientation, lensFacing, deviceId);
  }

  if (aidlMetadata == nullptr) {
    ALOGE("Failed to build metadata!");
    return AidlCameraMetadata();
  }

  return std::move(*aidlMetadata);
}

}  // namespace

VirtualCameraDevice::VirtualCameraDevice(
    const std::string& cameraId,
    const VirtualCameraConfiguration& configuration, int32_t deviceId)
    : mCameraId(cameraId),
      mVirtualCameraClientCallback(configuration.virtualCameraCallback),
      mSupportedInputConfigurations(configuration.supportedStreamConfigs),
      mPerFrameCameraMetadataEnabled(
          flags::virtual_camera_metadata()
              ? configuration.perFrameCameraMetadataEnabled
              : false),
      mConfigCameraCharacteristics(flags::virtual_camera_metadata()
                                       ? configuration.cameraCharacteristics
                                       : std::nullopt) {
  std::optional<AidlCameraMetadata> metadata = initCameraCharacteristics(
      mSupportedInputConfigurations, configuration.sensorOrientation,
      configuration.lensFacing, mConfigCameraCharacteristics, deviceId);

  if (metadata.has_value()) {
    mCameraCharacteristics = *metadata;
  } else {
    ALOGE(
        "%s: Failed to initialize camera characteristic based on provided "
        "configuration.",
        __func__);
  }
}

ndk::ScopedAStatus VirtualCameraDevice::getCameraCharacteristics(
    AidlCameraMetadata* _aidl_return) {
  ALOGV("%s", __func__);
  if (_aidl_return == nullptr) {
    return cameraStatus(Status::ILLEGAL_ARGUMENT);
  }

  *_aidl_return = mCameraCharacteristics;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::getPhysicalCameraCharacteristics(
    const std::string& in_physicalCameraId, AidlCameraMetadata* _aidl_return) {
  ALOGV("%s: physicalCameraId %s", __func__, in_physicalCameraId.c_str());
  (void)_aidl_return;

  // VTS tests expect this call to fail with illegal argument status for
  // all publicly advertised camera ids.
  // Because we don't support physical camera ids, we just always
  // fail with illegal argument (there's no valid argument to provide).
  return cameraStatus(Status::ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus VirtualCameraDevice::getResourceCost(
    CameraResourceCost* _aidl_return) {
  ALOGV("%s", __func__);
  if (_aidl_return == nullptr) {
    return cameraStatus(Status::ILLEGAL_ARGUMENT);
  }
  _aidl_return->resourceCost = 100;  // ¯\_(ツ)_/¯
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraDevice::isStreamCombinationSupported(
    const StreamConfiguration& in_streams, bool* _aidl_return) {
  ALOGV("%s", __func__);

  if (_aidl_return == nullptr) {
    return cameraStatus(Status::ILLEGAL_ARGUMENT);
  }

  *_aidl_return = isStreamCombinationSupported(in_streams);
  return ndk::ScopedAStatus::ok();
};

bool VirtualCameraDevice::isStreamCombinationSupported(
    const StreamConfiguration& streamConfiguration) const {
  if (streamConfiguration.streams.empty()) {
    ALOGE("%s: Querying empty configuration", __func__);
    return false;
  }

  const std::vector<Stream>& streams = streamConfiguration.streams;

  // ★ 2026-08-12:原来这里要求"会话内所有流必须同比例",否则直接拒。
  //   那条限制的真正来源是渲染器 —— 着色器把整张输入纹理拉伸铺满 viewport
  //   (EglProgram.h:61),比例不同就会变形,所以只能一刀切禁掉。
  //
  //   现在渲染路径已经支持**中心裁剪**(见 renderIntoEglFramebuffer 里的
  //   createCenterCropTransform),不同比例的流可以从同一张输入纹理上各自
  //   裁出自己要的那块,不再变形 ⇒ 这条检查可以去掉。
  //
  //   为什么这很重要:相机 App(CameraX)在进 VIDEO 模式 / 用倍率控件时,
  //   会在**同一个会话**里同时要 4:3 的分析小流和 16:9 的录像流。
  //   老逻辑下整个会话被拒 → App 等 3 秒超时 → Activity 自己 finish
  //   (表现为"闪退",但进程不死、无 crash 记录,极难定位)。
  //
  //   剩下的约束交给下面的逐流检查:每条流仍必须能从某个输入档裁出来。

  int numberOfProcessedStreams = 0;
  int numberOfStallStreams = 0;
  for (const Stream& stream : streamConfiguration.streams) {
    ALOGV("%s: Configuration queried: %s", __func__, stream.toString().c_str());

    if (stream.streamType == StreamType::INPUT) {
      ALOGW("%s: Input stream type is not supported", __func__);
      return false;
    }

    if (stream.rotation != StreamRotation::ROTATION_0 ||
        !isSupportedOutputFormat(stream.format)) {
      ALOGV("Unsupported output stream type");
      return false;
    }

    if (stream.format == PixelFormat::BLOB) {
      numberOfStallStreams++;
    } else {
      numberOfProcessedStreams++;
    }

    Resolution requestedResolution(stream.width, stream.height);
    // ★ 与上面同一个改动(2026-08-12):不再要求"流和输入档同比例",
    //   改成"输入档在**两个维度上都够大**,能中心裁剪出这条流"。
    //
    //   ⚠️ 判据必须逐维度比,不能用 Resolution::operator<=。
    //      那个 operator 比的是**像素总数**(Util.h:133),
    //      1920x1080(2.07M) 覆盖不了 1600x1200(1.92M) —— 像素数更少
    //      但高度更高,裁不出来,而 operator<= 会说"可以",
    //      于是配置通过、渲染时上下被拉伸。这种错**画面上很难一眼看出**。
    auto matchesSupportedInputConfig =
        [requestedResolution](const SupportedStreamConfiguration& config) {
          return requestedResolution.width <= config.width &&
                 requestedResolution.height <= config.height;
        };
    if (std::none_of(mSupportedInputConfigurations.begin(),
                     mSupportedInputConfigurations.end(),
                     matchesSupportedInputConfig)) {
      ALOGV("Requested config doesn't match any supported input config");
      return false;
    }
  }

  if (numberOfProcessedStreams > kMaxNumberOfProcessedStreams) {
    ALOGE("%s: %d processed streams exceeds the supported maximum of %d",
          __func__, numberOfProcessedStreams, kMaxNumberOfProcessedStreams);
    return false;
  }

  if (numberOfStallStreams > kMaxNumberOfStallStreams) {
    ALOGE("%s: %d stall streams exceeds the supported maximum of %d", __func__,
          numberOfStallStreams, kMaxNumberOfStallStreams);
    return false;
  }

  return true;
}

ndk::ScopedAStatus VirtualCameraDevice::open(
    const std::shared_ptr<ICameraDeviceCallback>& in_callback,
    std::shared_ptr<ICameraDeviceSession>* _aidl_return) {
  ALOGV("%s", __func__);

  *_aidl_return = ndk::SharedRefBase::make<VirtualCameraSession>(
      sharedFromThis(), in_callback, mVirtualCameraClientCallback);

  if (virtualdevice::flags::virtual_camera_on_open()) {
    if (mVirtualCameraClientCallback != nullptr) {
      mVirtualCameraClientCallback->onOpenCamera();
    }
  }

  return ndk::ScopedAStatus::ok();
};

ndk::ScopedAStatus VirtualCameraDevice::openInjectionSession(
    const std::shared_ptr<ICameraDeviceCallback>& in_callback,
    std::shared_ptr<ICameraInjectionSession>* _aidl_return) {
  ALOGV("%s", __func__);

  (void)in_callback;
  (void)_aidl_return;
  return cameraStatus(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus VirtualCameraDevice::setTorchMode(bool in_on) {
  ALOGV("%s: on = %s", __func__, in_on ? "on" : "off");
  return cameraStatus(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus VirtualCameraDevice::turnOnTorchWithStrengthLevel(
    int32_t in_torchStrength) {
  ALOGV("%s: torchStrength = %d", __func__, in_torchStrength);
  return cameraStatus(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus VirtualCameraDevice::getTorchStrengthLevel(
    int32_t* _aidl_return) {
  (void)_aidl_return;
  return cameraStatus(Status::OPERATION_NOT_SUPPORTED);
}

binder_status_t VirtualCameraDevice::dump(int fd, const char**, uint32_t) {
  ALOGD("Dumping virtual camera %s", mCameraId.c_str());
  const char* indent = "  ";
  const char* doubleIndent = "    ";
  dprintf(fd, "%svirtual_camera %s belongs to virtual device %d\n", indent,
          mCameraId.c_str(),
          getDeviceId(mCameraCharacteristics)
              .value_or(VirtualCameraService::kDefaultDeviceId));
  dprintf(fd, "%sSupportedStreamConfiguration:\n", indent);
  for (auto& config : mSupportedInputConfigurations) {
    dprintf(fd, "%s%s", doubleIndent, config.toString().c_str());
  }
  return STATUS_OK;
}

std::string VirtualCameraDevice::getCameraName() const {
  return std::string(kDevicePathPrefix) + mCameraId;
}

const std::vector<SupportedStreamConfiguration>&
VirtualCameraDevice::getInputConfigs() const {
  return mSupportedInputConfigurations;
}

Resolution VirtualCameraDevice::getMaxInputResolution() const {
  std::optional<Resolution> maxResolution =
      getMaxResolution(mSupportedInputConfigurations);
  if (!maxResolution.has_value()) {
    ALOGE(
        "%s: Cannot determine sensor size for virtual camera - input "
        "configurations empty?",
        __func__);
    return Resolution(0, 0);
  }
  return maxResolution.value();
}

int VirtualCameraDevice::allocateInputStreamId() {
  return mNextInputStreamId++;
}

std::shared_ptr<VirtualCameraDevice> VirtualCameraDevice::sharedFromThis() {
  // SharedRefBase which BnCameraDevice inherits from breaks
  // std::enable_shared_from_this. This is recommended replacement for
  // shared_from_this() per documentation in binder_interface_utils.h.
  return ref<VirtualCameraDevice>();
}

}  // namespace virtualcamera
}  // namespace companion
}  // namespace android
