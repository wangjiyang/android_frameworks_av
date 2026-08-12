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

#ifndef ANDROID_COMPANION_VIRTUALCAMERA_UTIL_H
#define ANDROID_COMPANION_VIRTUALCAMERA_UTIL_H

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "aidl/android/companion/virtualcamera/Format.h"
#include "aidl/android/hardware/camera/common/Status.h"
#include "aidl/android/hardware/camera/device/StreamBuffer.h"
#include "android/binder_auto_utils.h"
#include "android/hardware_buffer.h"
#include "system/graphics.h"
#include "ui/Fence.h"

namespace android {
namespace companion {
namespace virtualcamera {

constexpr int kHardwareBufferUsage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                                     AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
constexpr int kHardwareBufferFormat = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420;

// RAII utility class to safely lock AHardwareBuffer and obtain android_ycbcr
// structure describing YUV plane layout.
//
// Access to the buffer is locked immediatelly afer construction.
class YCbCrLockGuard {
 public:
  YCbCrLockGuard(std::shared_ptr<AHardwareBuffer> hwBuffer, uint32_t usageFlags);
  YCbCrLockGuard(YCbCrLockGuard&& other) = default;
  ~YCbCrLockGuard();

  // Returns OK if the buffer is successfully locked.
  status_t getStatus() const;

  // Dereferencing instance of this guard returns android_ycbcr structure
  // describing the layout.
  // Caller needs to check whether the buffer was successfully locked
  // before dereferencing.
  const android_ycbcr& operator*() const;

  // Disable copy.
  YCbCrLockGuard(const YCbCrLockGuard&) = delete;
  YCbCrLockGuard& operator=(const YCbCrLockGuard&) = delete;

 private:
  std::shared_ptr<AHardwareBuffer> mHwBuffer;
  android_ycbcr mYCbCr = {};
  status_t mLockStatus = DEAD_OBJECT;
};

// RAII utility class to safely lock AHardwareBuffer and obtain
// AHardwareBuffer_Planes (Suitable for interacting with RGBA / BLOB buffers.
//
// Access to the buffer is locked immediatelly afer construction.
class PlanesLockGuard {
 public:
  PlanesLockGuard(std::shared_ptr<AHardwareBuffer> hwBuffer,
                  uint64_t usageFlags, sp<Fence> fence = nullptr);
  PlanesLockGuard(PlanesLockGuard&& other) = default;
  ~PlanesLockGuard();

  // Returns OK if the buffer is successfully locked.
  status_t getStatus() const;

  // Dereferencing instance of this guard returns AHardwareBuffer_Planes
  // structure describing the layout.
  //
  // Caller needs to check whether the buffer was successfully locked
  // before dereferencing.
  const AHardwareBuffer_Planes& operator*() const;

  // Disable copy.
  PlanesLockGuard(const PlanesLockGuard&) = delete;
  PlanesLockGuard& operator=(const YCbCrLockGuard&) = delete;

 private:
  std::shared_ptr<AHardwareBuffer> mHwBuffer;
  AHardwareBuffer_Planes mPlanes;
  status_t mLockStatus = DEAD_OBJECT;
};

// Converts camera AIDL status to ndk::ScopedAStatus
inline ndk::ScopedAStatus cameraStatus(
    const ::aidl::android::hardware::camera::common::Status status) {
  return ndk::ScopedAStatus::fromServiceSpecificError(
      static_cast<int32_t>(status));
}

// Import Fence from AIDL NativeHandle.
//
// If the handle can't be used to construct Fence (is empty or doesn't contain
// only single fd) this function will return Fence instance in invalid state.
sp<Fence> importFence(
    const ::aidl::android::hardware::common::NativeHandle& handle);

// Returns true if specified pixel format is supported for virtual camera input.
bool isPixelFormatSupportedForInput(
    ::aidl::android::companion::virtualcamera::Format format);

// Returns true if specified format is supported for virtual camera input.
bool isFormatSupportedForInput(
    int width, int height,
    ::aidl::android::companion::virtualcamera::Format format, int maxFps);

// Representation of resolution / size.
struct Resolution {
  Resolution() = default;
  Resolution(const int w, const int h) : width(w), height(h) {
  }

  // Order by increasing pixel count, and by width for same pixel count.
  bool operator<(const Resolution& other) const {
    const int pixCount = width * height;
    const int otherPixCount = other.width * other.height;
    return pixCount == otherPixCount ? width < other.width
                                     : pixCount < otherPixCount;
  }

  bool operator<=(const Resolution& other) const {
    return *this == other || *this < other;
  }

  bool operator==(const Resolution& other) const {
    return width == other.width && height == other.height;
  }

  int width = 0;
  int height = 0;
};

struct FpsRange {
  int32_t minFps;
  int32_t maxFps;

  bool operator<(const FpsRange& other) const {
    return maxFps == other.maxFps ? minFps < other.minFps
                                  : maxFps < other.maxFps;
  }
};

struct GpsCoordinates {
  // Represented by a double[] in metadata with index 0 for
  // latitude and index 1 for longitude, 2 for altitude.
  double_t latitude;
  double_t longitude;
  double_t altitude;
  std::optional<int64_t> timestamp;
  std::string provider;
};

inline bool isApproximatellySameAspectRatio(const Resolution r1,
                                            const Resolution r2) {
  static constexpr float kAspectRatioEpsilon = 0.05;
  float aspectRatio1 =
      static_cast<float>(r1.width) / static_cast<float>(r1.height);
  float aspectRatio2 =
      static_cast<float>(r2.width) / static_cast<float>(r2.height);

  return std::abs(aspectRatio1 - aspectRatio2) < kAspectRatioEpsilon;
}

// Returns a texture-coordinate transform matrix (column-major, as consumed by
// EglTextureProgram::draw) that center-crops an input texture of resolution
// `input` so that it can be rendered into an output of resolution `output`
// *without distortion*.
//
// ★ 为什么需要这个(2026-08-12):
//   渲染路径是"把整张输入纹理拉伸铺满 viewport"(见 EglProgram.h:61 的注释
//   "Shader stretches the texture over the viewport"),所以一旦输入和输出
//   比例不同,画面就会被拉变形。这正是 HAL 原本无条件要求"会话内所有流同比例"
//   的根本原因 —— 那条限制是渲染能力的产物,不是协议的要求。
//   有了中心裁剪,不同比例就能共存:16:9 的输入给 4:3 的流时裁掉左右两边,
//   反之裁掉上下。
//
// 语义上这也是对的:真实相机的传感器是固定比例的,App 要 4:3 时相机 HAL
// 同样是从传感器上裁一块出来(ANDROID_SCALER_CROP_REGION),不会去拉伸。
//
// The returned matrix must be *pre-multiplied* onto the SurfaceTexture's own
// transform matrix (which handles buffer orientation / y-flip), never used on
// its own — see composeTransforms below.
std::array<float, 16> createCenterCropTransform(Resolution input,
                                                Resolution output);

// Returns the matrix product `outer * inner` for two column-major 4x4
// matrices.
//
// 用法:M = composeTransforms(crop, surfaceTransform),即裁剪在**外层**。
// 着色器算的是 `texCoord = M * vec4(s,t,0,1)`。
//
// ⚠️⚠️ 已知局限:**含 90°/270° 旋转时裁剪会作用在错误的轴上**。
//   createCenterCropTransform 只拿得到**未旋转的 buffer 尺寸**
//   (mInputSurfaceSize),它无从得知 SurfaceTexture 的 transform 会不会把
//   宽高轴对调。所以 transform 一旦含 ROT_90/ROT_270,"收窄宽度"这个意图
//   经旋转后会落到输出的**高度**方向上 —— 裁错边 + 残留变形。
//
//   ★ 这**不是**换个复合顺序能修好的,是**缺信息**:两种顺序都错,
//     只是错的方向不同。真要支持旋转,得先从 transform 里解出旋转角、
//     把 input 的宽高对调后再算裁剪。
//
//   为什么现在不修:当前链路的生产者(MediaCodec → Surface)**不调**
//   `setBuffersTransform`,transform 是单位阵,不触发这条路径。
//   贸然加"解旋转"的代码反而是没有判据的复杂度。
//   ⇒ 留作已知边界,并由 tests/UtilTest.cc 里
//     `cropIsComputedInUnrotatedBufferSpace` 把"单位阵前提"钉死:
//     将来谁引入带旋转的生产者,会先撞到那条用例而不是对着画面猜。
std::array<float, 16> composeTransforms(const std::array<float, 16>& outer,
                                        const std::array<float, 16>& inner);

std::ostream& operator<<(std::ostream& os, const Resolution& resolution);

}  // namespace virtualcamera
}  // namespace companion
}  // namespace android

#endif  // ANDROID_COMPANION_VIRTUALCAMERA_UTIL_H
