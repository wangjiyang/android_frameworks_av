/*
 * Copyright (C) 2023 The Android Open Source Project
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

// #define LOG_NDEBUG 0
#define LOG_TAG "VirtualCameraSession"
#include "VirtualCameraSession.h"

#include <android_companion_virtualdevice_flags.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CameraMetadata.h"
#include "EGL/egl.h"
#include "VirtualCameraCaptureResultConsumer.h"
#include "VirtualCameraDevice.h"
#include "VirtualCameraRenderThread.h"
#include "VirtualCameraStream.h"
#include "aidl/android/companion/virtualcamera/ICaptureResultConsumer.h"
#include "aidl/android/companion/virtualcamera/SupportedStreamConfiguration.h"
#include "aidl/android/companion/virtualcamera/VirtualCameraMetadata.h"
#include "aidl/android/hardware/camera/common/Status.h"
#include "aidl/android/hardware/camera/device/BufferCache.h"
#include "aidl/android/hardware/camera/device/BufferStatus.h"
#include "aidl/android/hardware/camera/device/CameraMetadata.h"
#include "aidl/android/hardware/camera/device/CaptureRequest.h"
#include "aidl/android/hardware/camera/device/HalStream.h"
#include "aidl/android/hardware/camera/device/NotifyMsg.h"
#include "aidl/android/hardware/camera/device/RequestTemplate.h"
#include "aidl/android/hardware/camera/device/ShutterMsg.h"
#include "aidl/android/hardware/camera/device/Stream.h"
#include "aidl/android/hardware/camera/device/StreamBuffer.h"
#include "aidl/android/hardware/camera/device/StreamConfiguration.h"
#include "aidl/android/hardware/camera/device/StreamRotation.h"
#include "aidl/android/hardware/graphics/common/BufferUsage.h"
#include "aidl/android/hardware/graphics/common/PixelFormat.h"
#include "android/hardware_buffer.h"
#include "android/native_window_aidl.h"
#include "fmq/AidlMessageQueue.h"
#include "system/camera_metadata.h"
#include "ui/GraphicBuffer.h"
#include "util/AidlUtil.h"
#include "util/EglDisplayContext.h"
#include "util/EglFramebuffer.h"
#include "util/EglProgram.h"
#include "util/JpegUtil.h"
#include "util/MetadataUtil.h"
#include "util/Util.h"

namespace android {
namespace companion {
namespace virtualcamera {

using ::aidl::android::companion::virtualcamera::ICaptureResultConsumer;
using ::aidl::android::companion::virtualcamera::IVirtualCameraCallback;
using ::aidl::android::companion::virtualcamera::SupportedStreamConfiguration;
using ::aidl::android::companion::virtualcamera::VirtualCameraMetadata;
using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::device::BufferCache;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::CameraOfflineSessionInfo;
using ::aidl::android::hardware::camera::device::CaptureRequest;
using ::aidl::android::hardware::camera::device::HalStream;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraOfflineSession;
using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::Stream;
using ::aidl::android::hardware::camera::device::StreamBuffer;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using ::android::base::unique_fd;

namespace {

using namespace std::chrono_literals;

namespace flags = ::android::companion::virtualdevice::flags;

// Size of request/result metadata fast message queue.
// Setting to 0 to always disables FMQ.
constexpr size_t kMetadataMsgQueueSize = 0;

// Maximum number of buffers to use per single stream.
constexpr size_t kMaxStreamBuffers = 2;

constexpr int kInvalidStreamId = -1;

camera_metadata_enum_android_control_capture_intent_t requestTemplateToIntent(
    const RequestTemplate type) {
  switch (type) {
    case RequestTemplate::PREVIEW:
      return ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
    case RequestTemplate::STILL_CAPTURE:
      return ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
    case RequestTemplate::VIDEO_RECORD:
      return ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
    case RequestTemplate::VIDEO_SNAPSHOT:
      return ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
    default:
      // Return PREVIEW by default
      return ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
  }
}

int getMaxFps(const std::vector<SupportedStreamConfiguration>& configs) {
  return std::transform_reduce(
      configs.begin(), configs.end(), 0,
      [](const int a, const int b) { return std::max(a, b); },
      [](const SupportedStreamConfiguration& config) { return config.maxFps; });
}

CameraMetadata createDefaultRequestSettings(
    const RequestTemplate type,
    const std::vector<SupportedStreamConfiguration>& inputConfigs) {
  int maxFps = getMaxFps(inputConfigs);
  auto metadata =
      MetadataBuilder()
          .setAberrationCorrectionMode(
              ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF)
          .setControlCaptureIntent(requestTemplateToIntent(type))
          .setControlMode(ANDROID_CONTROL_MODE_AUTO)
          .setControlAeMode(ANDROID_CONTROL_AE_MODE_ON)
          .setControlAeExposureCompensation(0)
          .setControlAeTargetFpsRange(FpsRange{maxFps, maxFps})
          .setControlAeAntibandingMode(ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO)
          .setControlAePrecaptureTrigger(
              ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE)
          .setControlAfTrigger(ANDROID_CONTROL_AF_TRIGGER_IDLE)
          .setControlAfMode(ANDROID_CONTROL_AF_MODE_OFF)
          .setControlAwbMode(ANDROID_CONTROL_AWB_MODE_AUTO)
          .setControlEffectMode(ANDROID_CONTROL_EFFECT_MODE_OFF)
          .setFaceDetectMode(ANDROID_STATISTICS_FACE_DETECT_MODE_OFF)
          .setFlashMode(ANDROID_FLASH_MODE_OFF)
          .setFlashState(ANDROID_FLASH_STATE_UNAVAILABLE)
          .setJpegQuality(VirtualCameraDevice::kDefaultJpegQuality)
          .setJpegThumbnailQuality(VirtualCameraDevice::kDefaultJpegQuality)
          .setJpegThumbnailSize(0, 0)
          .setNoiseReductionMode(ANDROID_NOISE_REDUCTION_MODE_OFF)
          .build();
  if (metadata == nullptr) {
    ALOGE("%s: Failed to construct metadata for default request type %s",
          __func__, toString(type).c_str());
    return CameraMetadata();
  } else {
    ALOGV("%s: Successfully created metadata for request type %s", __func__,
          toString(type).c_str());
  }
  return *metadata;
}

HalStream getHalStream(const Stream& stream) {
  HalStream halStream;
  halStream.id = stream.id;
  halStream.physicalCameraId = stream.physicalCameraId;
  halStream.maxBuffers = kMaxStreamBuffers;

  if (stream.format == PixelFormat::IMPLEMENTATION_DEFINED) {
    // If format is implementation defined we need it to override
    // it with actual format.
    // TODO(b/301023410) Override with the format based on the
    // camera configuration, once we support more formats.
    halStream.overrideFormat = PixelFormat::YCBCR_420_888;
  } else {
    halStream.overrideFormat = stream.format;
  }
  halStream.overrideDataSpace = stream.dataSpace;

  halStream.producerUsage = static_cast<BufferUsage>(
      static_cast<int64_t>(stream.usage) |
      static_cast<int64_t>(BufferUsage::CAMERA_OUTPUT) |
      static_cast<int64_t>(BufferUsage::GPU_RENDER_TARGET) |
      static_cast<int64_t>(BufferUsage::CPU_WRITE_OFTEN));

  halStream.supportOffline = false;
  return halStream;
}

// getHighestResolutionStream / resolutionFromStream 已随
// pickInputConfigurationForStreams 的改写一起删除(2026-08-12):
// 挑输入档不再只看"最大的那条流",而是看所有流在各维度上的最大值。

Resolution resolutionFromInputConfig(
    const SupportedStreamConfiguration& inputConfig) {
  return Resolution(inputConfig.width, inputConfig.height);
}

std::optional<Resolution> resolutionFromSurface(const sp<Surface> surface) {
  Resolution res{0, 0};
  if (surface == nullptr) {
    ALOGE("%s: Cannot get resolution from null surface", __func__);
    return std::nullopt;
  }

  int status = surface->query(NATIVE_WINDOW_WIDTH, &res.width);
  if (status != NO_ERROR) {
    ALOGE("%s: Failed to get width from surface", __func__);
    return std::nullopt;
  }

  status = surface->query(NATIVE_WINDOW_HEIGHT, &res.height);
  if (status != NO_ERROR) {
    ALOGE("%s: Failed to get height from surface", __func__);
    return std::nullopt;
  }
  return res;
}

std::optional<SupportedStreamConfiguration> pickInputConfigurationForStreams(
    const std::vector<Stream>& requestedStreams,
    const std::vector<SupportedStreamConfiguration>& supportedInputConfigs) {
  // ★ 2026-08-12 改动:原来只看**最大那条流**的比例去挑输入档
  //   (getHighestResolutionStream + 同比例过滤)。会话内允许混比例之后,
  //   那样挑会漏掉别的流 —— 比如最大流是 1920x1080(16:9),
  //   挑中 1920x1080 输入档,而同会话里另有一条 1600x1200(4:3) 的流,
  //   高度 1200 > 1080,只能**放大**,画面糊掉且没有任何报错。
  //
  //   现在改成:输入档必须**在两个维度上同时覆盖所有请求流**,
  //   在此前提下选像素数最小的那个(够用就行,不浪费带宽/显存)。
  //
  //   ⚠️ 同样不能用 Resolution::operator< / <=(它比的是像素总数),
  //      必须逐维度比。理由见 VirtualCameraDevice.cc 里同一处注释。

  // 所有流在各维度上的最大值 —— 输入档至少要这么大才能覆盖全部流。
  int requiredWidth = 0;
  int requiredHeight = 0;
  for (const Stream& stream : requestedStreams) {
    requiredWidth = std::max(requiredWidth, stream.width);
    requiredHeight = std::max(requiredHeight, stream.height);
  }

  // ★ 打分:**先看比例接近程度,再看面积**(2026-08-12 review 后改良)。
  //   只按"面积最小"挑会选到裁剪损失更大的那档。例:App 只要一条
  //   320x180(16:9),而我们有 1440x1080(4:3) 和 1920x1080(16:9) 两档 ——
  //   面积最小的是 4:3 那档,可它要裁掉左右 25%,白白丢掉水平信息;
  //   16:9 那档一刀不用裁。所以"最小"不等于"最好",要按畸变损失排。
  //
  //   比例用**最大请求流**的比例当参照(那条流的画质最要紧)。
  int refWidth = 0;
  int refHeight = 0;
  int refPixels = -1;
  for (const Stream& stream : requestedStreams) {
    const int pixels = stream.width * stream.height;
    if (pixels > refPixels) {
      refPixels = pixels;
      refWidth = stream.width;
      refHeight = stream.height;
    }
  }
  const float refAspect =
      (refHeight > 0) ? static_cast<float>(refWidth) / refHeight : 0.f;

  auto aspectPenalty = [refAspect](const SupportedStreamConfiguration& c) {
    if (c.height <= 0 || refAspect <= 0.f) {
      return 0.f;
    }
    return std::abs(static_cast<float>(c.width) / c.height - refAspect);
  };

  std::optional<SupportedStreamConfiguration> bestConfig;
  for (const SupportedStreamConfiguration& inputConfig : supportedInputConfigs) {
    if (inputConfig.width < requiredWidth ||
        inputConfig.height < requiredHeight) {
      // 覆盖不了(至少一个维度不够),会导致放大,跳过。
      continue;
    }

    if (!bestConfig.has_value()) {
      bestConfig = inputConfig;
      continue;
    }

    const float candidatePenalty = aspectPenalty(inputConfig);
    const float bestPenalty = aspectPenalty(*bestConfig);
    // 比例差距在 epsilon 之内视为"一样好",此时再比面积(取小的,省显存/带宽)。
    constexpr float kAspectTieEpsilon = 0.01f;
    if (candidatePenalty < bestPenalty - kAspectTieEpsilon) {
      bestConfig = inputConfig;
    } else if (std::abs(candidatePenalty - bestPenalty) <= kAspectTieEpsilon &&
               (inputConfig.width * inputConfig.height) <
                   (bestConfig->width * bestConfig->height)) {
      bestConfig = inputConfig;
    }
  }

  return bestConfig;
}

RequestSettings createSettingsFromMetadata(const CameraMetadata& metadata) {
  return RequestSettings{
      .jpegQuality = getJpegQuality(metadata).value_or(
          VirtualCameraDevice::kDefaultJpegQuality),
      .jpegOrientation = getJpegOrientation(metadata),
      .thumbnailResolution =
          getJpegThumbnailSize(metadata).value_or(Resolution(0, 0)),
      .thumbnailJpegQuality = getJpegThumbnailQuality(metadata).value_or(
          VirtualCameraDevice::kDefaultJpegQuality),
      .fpsRange = getFpsRange(metadata),
      .captureIntent = getCaptureIntent(metadata).value_or(
          ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW),
      .gpsCoordinates = getGpsCoordinates(metadata),
      .aePrecaptureTrigger = getPrecaptureTrigger(metadata)};
}

};  // namespace

VirtualCameraSession::VirtualCameraSession(
    std::shared_ptr<VirtualCameraDevice> cameraDevice,
    std::shared_ptr<ICameraDeviceCallback> cameraDeviceCallback,
    std::shared_ptr<IVirtualCameraCallback> virtualCameraClientCallback)
    : mCameraDevice(cameraDevice),
      mCameraDeviceCallback(cameraDeviceCallback),
      mVirtualCameraClientCallback(virtualCameraClientCallback),
      mCurrentInputStreamId(kInvalidStreamId) {
  mRequestMetadataQueue = std::make_unique<RequestMetadataQueue>(
      kMetadataMsgQueueSize, false /* non blocking */);
  if (!mRequestMetadataQueue->isValid()) {
    ALOGE("%s: invalid request fmq", __func__);
  }

  mResultMetadataQueue = std::make_shared<ResultMetadataQueue>(
      kMetadataMsgQueueSize, false /* non blocking */);
  if (!mResultMetadataQueue->isValid()) {
    ALOGE("%s: invalid result fmq", __func__);
  }

 std::shared_ptr<VirtualCameraDevice> virtualCamera = mCameraDevice.lock();
 if (flags::virtual_camera_metadata() && virtualCamera != nullptr &&
    virtualCamera->isPerFrameCameraMetadataEnabled()) {
   // create a capture result consumer shared reference and set it in the
   // session context.
   mSessionContext.setCaptureResultConsumer(
       ndk::SharedRefBase::make<VirtualCameraCaptureResultConsumer>());
  }
}

ndk::ScopedAStatus VirtualCameraSession::close() {
  ALOGV("%s", __func__);
  {
    std::lock_guard<std::mutex> lock(mLock);

    if (mRenderThread != nullptr) {
      mRenderThread->flush();
      mRenderThread->stop();
      mRenderThread = nullptr;

      if (mVirtualCameraClientCallback != nullptr) {
        mVirtualCameraClientCallback->onStreamClosed(mCurrentInputStreamId);
      }
      mCurrentInputStreamId = kInvalidStreamId;
    }
  }

  mSessionContext.closeAllStreams();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::configureStreams(
    const StreamConfiguration& in_requestedConfiguration,
    std::vector<HalStream>* _aidl_return) {
  ALOGV("%s: requestedConfiguration: %s", __func__,
        in_requestedConfiguration.toString().c_str());

  if (_aidl_return == nullptr) {
    return cameraStatus(Status::ILLEGAL_ARGUMENT);
  }

  std::shared_ptr<VirtualCameraDevice> virtualCamera = mCameraDevice.lock();
  if (virtualCamera == nullptr) {
    ALOGW("%s: configure called on already unregistered camera", __func__);
    return cameraStatus(Status::CAMERA_DISCONNECTED);
  }

  mSessionContext.removeStreamsNotInStreamConfiguration(
      in_requestedConfiguration);

  auto& streams = in_requestedConfiguration.streams;
  auto& halStreams = *_aidl_return;
  halStreams.clear();
  halStreams.resize(in_requestedConfiguration.streams.size());

  if (!virtualCamera->isStreamCombinationSupported(in_requestedConfiguration)) {
    ALOGE(
        "%s: Requested stream configuration is not supported, closing existing "
        "session",
        __func__);
    close();
    return cameraStatus(Status::ILLEGAL_ARGUMENT);
  }

  sp<Surface> inputSurface = nullptr;
  int inputStreamId = -1;
  std::optional<SupportedStreamConfiguration> inputConfig;
  {
    std::lock_guard<std::mutex> lock(mLock);
    for (int i = 0; i < in_requestedConfiguration.streams.size(); ++i) {
      halStreams[i] = getHalStream(streams[i]);
      if (mSessionContext.initializeStream(streams[i])) {
        ALOGV("Configured new stream: %s", streams[i].toString().c_str());
      }
    }

    inputConfig = pickInputConfigurationForStreams(
        streams, virtualCamera->getInputConfigs());
    if (!inputConfig.has_value()) {
      ALOGE(
          "%s: Failed to pick any input configuration for stream configuration "
          "request: %s",
          __func__, in_requestedConfiguration.toString().c_str());
      return cameraStatus(Status::ILLEGAL_ARGUMENT);
    }

    if (mRenderThread != nullptr) {
      // If there's already a render thread, it means this is not a first
      // configuration call. If the surface has the same resolution and pixel
      // format as the picked config, we don't need to do anything, the current
      // render thread is capable of serving new set of configuration. However
      // if it differs, we need to discard the current surface and
      // reinitialize the render thread.

      std::optional<Resolution> currentInputResolution =
          resolutionFromSurface(mRenderThread->getInputSurface());
      if (currentInputResolution.has_value() &&
          *currentInputResolution == resolutionFromInputConfig(*inputConfig)) {
        ALOGI(
            "%s: Newly configured set of streams matches existing client "
            "surface (%dx%d)",
            __func__, currentInputResolution->width,
            currentInputResolution->height);
        return ndk::ScopedAStatus::ok();
      }

      if (mVirtualCameraClientCallback != nullptr) {
        mVirtualCameraClientCallback->onStreamClosed(mCurrentInputStreamId);
      }

      // ⚠️ 这里**不能**直接解引用 currentInputResolution:走到这一行有两种
      //   可能,其中一种是上面的 `has_value()` 为 false 而短路下来的
      //   ⇒ 空 optional 解引用 = UB(上游既有 bug,2026-08-12 review 抓出)。
      //   混比例之后 render thread 重建更频繁,这条路径也就更常走。
      //
      // ★ 用具名变量而不是在 ALOGV 实参里现拼字符串:那样临时 std::string
      //   会在取完 .c_str() 后**立刻析构**,ALOGV 拿到的是悬垂指针。
      const std::string currentResolutionStr =
          currentInputResolution.has_value()
              ? std::to_string(currentInputResolution->width) + "x" +
                    std::to_string(currentInputResolution->height)
              : std::string("unknown");
      ALOGV(
          "%s: Newly requested output streams are not suitable for "
          "pre-existing surface (%s), creating new surface (%dx%d)",
          __func__, currentResolutionStr.c_str(), inputConfig->width,
          inputConfig->height);

      mRenderThread->flush();
      mRenderThread->stop();
    }

    mRenderThread = std::make_unique<VirtualCameraRenderThread>(
        mSessionContext, resolutionFromInputConfig(*inputConfig),
        virtualCamera->getMaxInputResolution(), mCameraDeviceCallback);
    mRenderThread->start();
    inputSurface = mRenderThread->getInputSurface();
    inputStreamId = mCurrentInputStreamId =
        virtualCamera->allocateInputStreamId();
  }

  // The onConfigureSession is oneway async, just informs the VD owner of
  // the session params
  if (flags::virtual_camera_metadata() &&
      mVirtualCameraClientCallback != nullptr) {
    VirtualCameraMetadata sessionParamsMetadata;
    status_t ret = convertDeviceToVirtualCameraMetadata(
        in_requestedConfiguration.sessionParams, sessionParamsMetadata);
    if (ret != OK) {
      ALOGE("Failed to convert device to virtual session parameters!");
    }

    mVirtualCameraClientCallback->onConfigureSession(sessionParamsMetadata,
                                                     mSessionContext.getCaptureResultConsumer());
  }

  if (mVirtualCameraClientCallback != nullptr && inputSurface != nullptr) {
    // TODO(b/301023410) Pass streamId based on client input stream id once
    // support for multiple input streams is implemented. For now we always
    // create single texture.
    mVirtualCameraClientCallback->onStreamConfigured(
        inputStreamId, aidl::android::view::Surface(inputSurface.get()),
        inputConfig->width, inputConfig->height, inputConfig->pixelFormat);
  }

  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::constructDefaultRequestSettings(
    RequestTemplate in_type, CameraMetadata* _aidl_return) {
  ALOGV("%s: type %d", __func__, static_cast<int32_t>(in_type));

  std::shared_ptr<VirtualCameraDevice> camera = mCameraDevice.lock();
  if (camera == nullptr) {
    ALOGW(
        "%s: constructDefaultRequestSettings called on already unregistered "
        "camera",
        __func__);
    return cameraStatus(Status::CAMERA_DISCONNECTED);
  }

  switch (in_type) {
    case RequestTemplate::PREVIEW:
    case RequestTemplate::STILL_CAPTURE:
    case RequestTemplate::VIDEO_RECORD:
    case RequestTemplate::VIDEO_SNAPSHOT: {
      *_aidl_return =
          createDefaultRequestSettings(in_type, camera->getInputConfigs());
      return ndk::ScopedAStatus::ok();
    }
    case RequestTemplate::MANUAL:
    case RequestTemplate::ZERO_SHUTTER_LAG:
      // Don't support VIDEO_SNAPSHOT, MANUAL, ZSL templates
      return ndk::ScopedAStatus::fromServiceSpecificError(
          static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
    default:
      ALOGE("%s: unknown request template type %d", __FUNCTION__,
            static_cast<int>(in_type));
      return ndk::ScopedAStatus::fromServiceSpecificError(
          static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
  }
}

ndk::ScopedAStatus VirtualCameraSession::flush() {
  ALOGV("%s", __func__);
  std::lock_guard<std::mutex> lock(mLock);
  if (mRenderThread != nullptr) {
    mRenderThread->flush();
  }
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::getCaptureRequestMetadataQueue(
    MQDescriptor<int8_t, SynchronizedReadWrite>* _aidl_return) {
  ALOGV("%s", __func__);
  *_aidl_return = mRequestMetadataQueue->dupeDesc();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::getCaptureResultMetadataQueue(
    MQDescriptor<int8_t, SynchronizedReadWrite>* _aidl_return) {
  ALOGV("%s", __func__);
  *_aidl_return = mResultMetadataQueue->dupeDesc();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::isReconfigurationRequired(
    const CameraMetadata& in_oldSessionParams,
    const CameraMetadata& in_newSessionParams, bool* _aidl_return) {
  ALOGV("%s: oldSessionParams: %s newSessionParams: %s", __func__,
        in_newSessionParams.toString().c_str(),
        in_oldSessionParams.toString().c_str());

  if (_aidl_return == nullptr) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
  }

  *_aidl_return = true;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::processCaptureRequest(
    const std::vector<CaptureRequest>& in_requests,
    const std::vector<BufferCache>& in_cachesToRemove, int32_t* _aidl_return) {
  ALOGV("%s: request count: %zu", __func__, in_requests.size());

  if (!in_cachesToRemove.empty()) {
    mSessionContext.removeBufferCaches(in_cachesToRemove);
  }

  for (const auto& captureRequest : in_requests) {
    auto status = processCaptureRequest(captureRequest);
    if (!status.isOk()) {
      return status;
    }
  }
  *_aidl_return = in_requests.size();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::signalStreamFlush(
    const std::vector<int32_t>& in_streamIds, int32_t in_streamConfigCounter) {
  ALOGV("%s", __func__);

  (void)in_streamIds;
  (void)in_streamConfigCounter;
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VirtualCameraSession::switchToOffline(
    const std::vector<int32_t>& in_streamsToKeep,
    CameraOfflineSessionInfo* out_offlineSessionInfo,
    std::shared_ptr<ICameraOfflineSession>* _aidl_return) {
  ALOGV("%s", __func__);

  (void)in_streamsToKeep;
  (void)out_offlineSessionInfo;

  if (_aidl_return == nullptr) {
    return ndk::ScopedAStatus::fromServiceSpecificError(
        static_cast<int32_t>(Status::ILLEGAL_ARGUMENT));
  }

  *_aidl_return = nullptr;
  return cameraStatus(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus VirtualCameraSession::repeatingRequestEnd(
    int32_t in_frameNumber, const std::vector<int32_t>& in_streamIds) {
  ALOGV("%s", __func__);
  (void)in_frameNumber;
  (void)in_streamIds;
  return ndk::ScopedAStatus::ok();
}

std::set<int> VirtualCameraSession::getStreamIds() const {
  return mSessionContext.getStreamIds();
}

ndk::ScopedAStatus VirtualCameraSession::processCaptureRequest(
    const CaptureRequest& request) {
  ALOGV("%s: CaptureRequest { frameNumber:%d }", __func__, request.frameNumber);

  std::shared_ptr<ICameraDeviceCallback> cameraCallback = nullptr;
  RequestSettings requestSettings;
  int currentInputStreamId;
  {
    std::lock_guard<std::mutex> lock(mLock);

    // If metadata is empty, last received metadata applies, if it's non-empty
    // update it.
    if (!request.settings.metadata.empty()) {
      mCurrentRequestMetadata = request.settings;
    }

    // We don't have any metadata for this request - this means we received none
    // in first request, this is an error state.
    if (mCurrentRequestMetadata.metadata.empty()) {
      return cameraStatus(Status::ILLEGAL_ARGUMENT);
    }

    requestSettings = createSettingsFromMetadata(mCurrentRequestMetadata);

    cameraCallback = mCameraDeviceCallback;
    currentInputStreamId = mCurrentInputStreamId;
  }

  if (cameraCallback == nullptr) {
    ALOGE(
        "%s: processCaptureRequest called, but there's no camera callback "
        "configured",
        __func__);
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  if (!mSessionContext.importBuffersFromCaptureRequest(request)) {
    ALOGE("Failed to import buffers from capture request.");
    return cameraStatus(Status::INTERNAL_ERROR);
  }

  std::vector<CaptureRequestBuffer> taskBuffers;
  taskBuffers.reserve(request.outputBuffers.size());
  for (const StreamBuffer& streamBuffer : request.outputBuffers) {
    taskBuffers.emplace_back(streamBuffer.streamId, streamBuffer.bufferId,
                             importFence(streamBuffer.acquireFence));
  }

  {
    std::lock_guard<std::mutex> lock(mLock);
    if (mRenderThread == nullptr) {
      ALOGE(
          "%s: processCaptureRequest (frameNumber %d)called before configure "
          "(render thread not initialized)",
          __func__, request.frameNumber);
      return cameraStatus(Status::INTERNAL_ERROR);
    }
    mRenderThread->enqueueTask(std::make_unique<ProcessCaptureRequestTask>(
        request.frameNumber, taskBuffers, requestSettings));
  }

  if (mVirtualCameraClientCallback != nullptr) {
    std::shared_ptr<VirtualCameraDevice> virtualCamera = mCameraDevice.lock();
    if (virtualCamera == nullptr) {
      ALOGW("%s: process capture request on unregistered camera", __func__);
      return cameraStatus(Status::CAMERA_DISCONNECTED);
    }

    std::optional<VirtualCameraMetadata> captureRequestSettings;
    if (flags::virtual_camera_metadata() &&
        virtualCamera->isPerFrameCameraMetadataEnabled()) {
      VirtualCameraMetadata virtualCameraMetadata;
      // Send the settings of the CaptureRequest as VirtualCameraMetadata
      status_t ret = convertDeviceToVirtualCameraMetadata(
          request.settings, virtualCameraMetadata);
      if (ret != OK) {
        ALOGE("Failed to convert device to virtual capture request settings!");
      }
      captureRequestSettings = virtualCameraMetadata;
    }
    ndk::ScopedAStatus status =
        mVirtualCameraClientCallback->onProcessCaptureRequest(
            currentInputStreamId, request.frameNumber, captureRequestSettings);
    if (!status.isOk()) {
      ALOGE(
          "Failed to invoke onProcessCaptureRequest client callback for frame "
          "%d. Flag virtual_camera_metadata enabled: %s. "
          "PerFrameCameraMetadataEnabled %s.",
          request.frameNumber,
          flags::virtual_camera_metadata() ? "true" : "false",
          virtualCamera->isPerFrameCameraMetadataEnabled() ? "true" : "false");
    }
  }

  return ndk::ScopedAStatus::ok();
}

}  // namespace virtualcamera
}  // namespace companion
}  // namespace android
