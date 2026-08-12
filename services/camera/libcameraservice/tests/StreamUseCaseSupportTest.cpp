/*
 * Copyright (C) 2026 The Android Open Source Project
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

// isStreamUseCaseSupported 的行为测试。
//
// ★ 背景(2026-08-12,ovaltine SM8475):
//   本机六台相机**全部没有**声明 ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES,
//   而 CameraX 在某些配置下会给流打上 StreamUseCase(value=1/2)。
//   上游逻辑在"没声明列表"时只放行 DEFAULT(0),于是整个会话建不起来:
//       E cameraserver: Camera 1: stream use case 1 not supported
//       → CameraCaptureSession 配置失败 → CameraX 超时 → Activity finish()
//   ⚠️ 进程**不死**,所以 crash buffer / tombstone / exit-info 全查不到,
//      表现为"闪退"却毫无线索 —— 这正是它难定位的原因。
//
// 本文件把新旧两种行为的边界钉死,防止以后被"顺手改回上游"。

#include <camera/CameraMetadata.h>
#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "utils/SessionConfigurationUtils.h"

using namespace android;
using namespace android::camera3::SessionConfigurationUtils;

namespace {

// 构造一个**声明了** use case 列表的 deviceInfo。
CameraMetadata deviceInfoWithUseCases(const std::vector<int64_t>& useCases) {
    CameraMetadata m;
    m.update(ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES, useCases.data(), useCases.size());
    return m;
}

}  // namespace

// ── 设备完全没声明 use case 列表(本机的情况)────────────────────────
//
// ★ 这一组是本次改动的核心:标准 use case 全部放行。
//   理由:use case 本质是**给 HAL 的性能提示**(要不要 ZSL、走哪条 ISP 路径),
//   设备没声明能力表 ⇒ 它不理解这些提示 ⇒ 忽略即可,按普通流处理。
//   硬拒的代价是整个会话建不起来,明显更糟。
TEST(StreamUseCaseSupportTest, noDeclaredUseCasesAcceptsStandardOnes) {
    CameraMetadata empty;  // 不含 AVAILABLE_STREAM_USE_CASES

    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT, empty));
    // ★ 下面这条就是真机上失败的那一条(CameraX 的 PREVIEW 流)
    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW, empty));
    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_STILL_CAPTURE, empty));
    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_RECORD, empty));
    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW_VIDEO_STILL, empty));
    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL, empty));
}

// ★★ 负向:CROPPED_RAW 即使在"没声明"时也必须**拒绝**。
//    它和上面那些不同 —— 它会改变**输出内容**(裁剪过的 RAW),
//    不只是性能提示。静默忽略会让 App 拿到与预期不符的数据,
//    那种错误比"会话建不起来"更隐蔽、更难查。
TEST(StreamUseCaseSupportTest, noDeclaredUseCasesStillRejectsCroppedRaw) {
    CameraMetadata empty;
    EXPECT_FALSE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_CROPPED_RAW, empty));
}

// ── 设备**声明了**列表:必须严格按声明的来(这部分行为**没有**改变)──
//
// ★ 这一组是防止我把放宽做过头的护栏:设备声明了列表说明它真的懂 use case,
//   那"它明确没列出来的"就该拒,不能一起放开。
TEST(StreamUseCaseSupportTest, declaredUseCasesAreEnforcedStrictly) {
    CameraMetadata m = deviceInfoWithUseCases({
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT,
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW});

    EXPECT_TRUE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_PREVIEW, m));
    // 没列出来的必须拒 —— 哪怕它是"标准" use case
    EXPECT_FALSE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VIDEO_CALL, m));
    EXPECT_FALSE(isStreamUseCaseSupported(
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_STILL_CAPTURE, m));
}

// ── vendor 区间无条件放行(上游既有行为,顺带钉住)───────────────────
TEST(StreamUseCaseSupportTest, vendorUseCasesAlwaysAllowed) {
    CameraMetadata empty;
    CameraMetadata declared = deviceInfoWithUseCases(
            {ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT});

    const int64_t vendorUseCase =
            ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_VENDOR_START + 3;
    EXPECT_TRUE(isStreamUseCaseSupported(vendorUseCase, empty));
    EXPECT_TRUE(isStreamUseCaseSupported(vendorUseCase, declared));
}
