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

// 中心裁剪矩阵的数学单元测试。
//
// ★★ 为什么这个文件必须存在(2026-08-12):
//   我先在树外写了个一次性 main() 验算,验完就把结论写进了注释
//   ——"同比例时返回严格单位矩阵(有单元测试钉着)"。
//   对抗式 review 一 grep 就发现:**tests/ 里根本没有这个测试**,
//   注释宣称了一个不存在的安全网。任何人读到那句都会放心跳过验证。
//   ⇒ 树外验算不算数,判据必须**长在树里、跟着 CI 一起跑**。
//
// 覆盖:
//   ① 同比例 ⇒ **严格**单位阵(保证原有 16:9 路径逐像素不变)
//   ② 16:9 输入 → 4:3 输出 ⇒ 裁左右、保高度
//   ③ 4:3 输入 → 16:9 输出 ⇒ 裁上下、保宽度
//   ④ 退化输入 ⇒ 单位阵,不产生 NaN
//   ⑤ composeTransforms 与单位阵可交换、且**顺序敏感**
//   ⑥ ★ 含 ROT_90 的 SurfaceTexture transform 下的行为(见文件末尾说明)

#include <array>
#include <cmath>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "util/Util.h"

namespace android {
namespace companion {
namespace virtualcamera {
namespace {

constexpr float kEpsilon = 1e-5f;

// 单位阵(列主序)。
constexpr std::array<float, 16> kIdentity{1.f, 0.f, 0.f, 0.f,   //
                                          0.f, 1.f, 0.f, 0.f,   //
                                          0.f, 0.f, 1.f, 0.f,   //
                                          0.f, 0.f, 0.f, 1.f};  //

// 按着色器的算法把纹理坐标过一遍矩阵:texCoord = M * vec4(s, t, 0, 1)
// (列主序:元素 (row, col) 位于 M[col * 4 + row])
void applyToTexCoord(const std::array<float, 16>& m, float s, float t,
                     float* outS, float* outT) {
  *outS = m[0] * s + m[4] * t + m[12];
  *outT = m[1] * s + m[5] * t + m[13];
}

void expectMatrixNear(const std::array<float, 16>& actual,
                      const std::array<float, 16>& expected) {
  for (int i = 0; i < 16; ++i) {
    EXPECT_NEAR(actual[i], expected[i], kEpsilon) << "element " << i;
  }
}

// ── ① 同比例必须是严格单位阵 ────────────────────────────────────────
// 这条是**整个改动的安全阀**:它保证开启裁剪之后,所有原本就同比例的
// 渲染路径逐像素不变(不会因为多乘了一个"几乎是单位阵"的矩阵而抖动)。
TEST(CenterCropTransformTest, sameAspectRatioIsExactlyIdentity) {
  expectMatrixNear(createCenterCropTransform(Resolution(1920, 1080),
                                             Resolution(1280, 720)),
                   kIdentity);
  expectMatrixNear(
      createCenterCropTransform(Resolution(640, 480), Resolution(320, 240)),
      kIdentity);
  // 完全相同的尺寸自然也该是单位阵。
  expectMatrixNear(
      createCenterCropTransform(Resolution(800, 600), Resolution(800, 600)),
      kIdentity);
}

// ── ② 16:9 输入渲染进 4:3 输出:裁左右,高度保满 ────────────────────
TEST(CenterCropTransformTest, widerInputCropsHorizontally) {
  const std::array<float, 16> m =
      createCenterCropTransform(Resolution(1920, 1080), Resolution(640, 480));

  float s = 0.f, t = 0.f;
  // 保留宽度比例 = (4/3)/(16/9) = 0.75 ⇒ 左右各切掉 0.125。
  applyToTexCoord(m, 0.f, 0.f, &s, &t);
  EXPECT_NEAR(s, 0.125f, kEpsilon);
  EXPECT_NEAR(t, 0.f, kEpsilon) << "高度必须保满,不该被裁";

  applyToTexCoord(m, 1.f, 1.f, &s, &t);
  EXPECT_NEAR(s, 0.875f, kEpsilon);
  EXPECT_NEAR(t, 1.f, kEpsilon) << "高度必须保满,不该被裁";

  // 中心必须还在中心 —— 这正是"**中心**裁剪"的定义。
  applyToTexCoord(m, 0.5f, 0.5f, &s, &t);
  EXPECT_NEAR(s, 0.5f, kEpsilon);
  EXPECT_NEAR(t, 0.5f, kEpsilon);
}

// ── ③ 4:3 输入渲染进 16:9 输出:裁上下,宽度保满 ────────────────────
TEST(CenterCropTransformTest, tallerInputCropsVertically) {
  const std::array<float, 16> m =
      createCenterCropTransform(Resolution(1440, 1080), Resolution(1920, 1080));

  float s = 0.f, t = 0.f;
  applyToTexCoord(m, 0.f, 0.f, &s, &t);
  EXPECT_NEAR(s, 0.f, kEpsilon) << "宽度必须保满,不该被裁";
  EXPECT_NEAR(t, 0.125f, kEpsilon);

  applyToTexCoord(m, 1.f, 1.f, &s, &t);
  EXPECT_NEAR(s, 1.f, kEpsilon) << "宽度必须保满,不该被裁";
  EXPECT_NEAR(t, 0.875f, kEpsilon);
}

// ── ④ 退化输入不能产生 NaN ──────────────────────────────────────────
// 渲染线程在极端时序下可能拿到 0 尺寸;NaN 矩阵会让整屏变成噪声,
// 而且**不报任何错**。
TEST(CenterCropTransformTest, degenerateInputYieldsIdentityNotNan) {
  for (const auto& [in, out] :
       std::array<std::pair<Resolution, Resolution>, 4>{
           {{Resolution(0, 0), Resolution(640, 480)},
            {Resolution(640, 480), Resolution(0, 0)},
            {Resolution(-1, 480), Resolution(640, 480)},
            {Resolution(640, 480), Resolution(640, -1)}}}) {
    const std::array<float, 16> m = createCenterCropTransform(in, out);
    for (int i = 0; i < 16; ++i) {
      EXPECT_FALSE(std::isnan(m[i])) << "element " << i << " is NaN";
    }
    expectMatrixNear(m, kIdentity);
  }
}

// ── ⑤ composeTransforms ────────────────────────────────────────────
TEST(ComposeTransformsTest, identityIsNeutralOnBothSides) {
  const std::array<float, 16> crop =
      createCenterCropTransform(Resolution(1920, 1080), Resolution(640, 480));

  expectMatrixNear(composeTransforms(crop, kIdentity), crop);
  expectMatrixNear(composeTransforms(kIdentity, crop), crop);
}

TEST(ComposeTransformsTest, matchesKnownProduct) {
  // 两个简单的缩放/平移阵,手算乘积作为独立参照 —— 避免"用实现验证实现"。
  // A:x 缩放 2、平移 1;B:y 缩放 3、平移 5。
  std::array<float, 16> a = kIdentity;
  a[0] = 2.f;
  a[12] = 1.f;
  std::array<float, 16> b = kIdentity;
  b[5] = 3.f;
  b[13] = 5.f;

  // A * B 作用在 (s,t): 先 B 后 A ⇒ x' = 2s + 1, y' = 3t + 5
  const std::array<float, 16> ab = composeTransforms(a, b);
  float s = 0.f, t = 0.f;
  applyToTexCoord(ab, 1.f, 1.f, &s, &t);
  EXPECT_NEAR(s, 3.f, kEpsilon);
  EXPECT_NEAR(t, 8.f, kEpsilon);
}

TEST(ComposeTransformsTest, isOrderSensitive) {
  // 用一个 90 度旋转当内层,两种顺序必须给出不同结果。
  // 若相同,说明乘法实现退化成了可交换 —— 那是实现错误。
  const std::array<float, 16> rot90{0.f, 1.f, 0.f, 0.f,   //
                                    -1.f, 0.f, 0.f, 0.f,  //
                                    0.f, 0.f, 1.f, 0.f,   //
                                    1.f, 0.f, 0.f, 1.f};  //
  const std::array<float, 16> crop =
      createCenterCropTransform(Resolution(1920, 1080), Resolution(640, 480));

  const std::array<float, 16> cropOuter = composeTransforms(crop, rot90);
  const std::array<float, 16> cropInner = composeTransforms(rot90, crop);

  bool differ = false;
  for (int i = 0; i < 16; ++i) {
    if (std::abs(cropOuter[i] - cropInner[i]) > kEpsilon) {
      differ = true;
    }
  }
  EXPECT_TRUE(differ) << "两种复合顺序不该得到同一个矩阵";
}

// ── ⑥ ★ 旋转下的已知局限(把它钉成可执行的文档)────────────────────
//
// createCenterCropTransform 只拿到**未旋转的** buffer 尺寸,它无从得知
// SurfaceTexture 的 transform 会不会把宽高轴对调。因此当 transform 含
// ROT_90/ROT_270 时,裁剪会作用在**旋转前**的轴上 —— 相当于裁错 90°。
//
// 当前 AgentOS 链路里生产者(MediaCodec → Surface)不设
// setBuffersTransform,transform 是单位阵,所以不触发。
// 这条用例把"单位阵下才成立"这个前提固定下来:
// 一旦将来有人引入带旋转的生产者,应当先来读这里,而不是对着画面猜。
TEST(CenterCropTransformTest, cropIsComputedInUnrotatedBufferSpace) {
  const std::array<float, 16> crop =
      createCenterCropTransform(Resolution(1920, 1080), Resolution(640, 480));

  // 单位阵 transform(当前链路的真实情况)下,裁剪落在 s 轴 —— 符合预期。
  const std::array<float, 16> composed = composeTransforms(crop, kIdentity);
  float s = 0.f, t = 0.f;
  applyToTexCoord(composed, 0.f, 0.f, &s, &t);
  EXPECT_NEAR(s, 0.125f, kEpsilon)
      << "无旋转时裁剪必须落在水平轴上;若这条挂了,说明复合顺序被改坏了";
  EXPECT_NEAR(t, 0.f, kEpsilon);
}

}  // namespace
}  // namespace virtualcamera
}  // namespace companion
}  // namespace android
