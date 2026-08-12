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

package android.companion.virtualcamera;

import android.companion.virtualcamera.VirtualCameraConfiguration;

/**
 * AIDL Interface to communicate with the VirtualCamera HAL
 * @hide
 */
interface IVirtualCameraService {

    /**
     * Registers a new camera with the virtual camera hal.
     * @return true if the camera was successfully registered
     */
    boolean registerCamera(in IBinder token, in VirtualCameraConfiguration configuration,
            int deviceId);

    /**
     * Unregisters the camera from the virtual camera hal. After this call the virtual camera won't
     * be visible to the camera framework anymore.
     */
    void unregisterCamera(in IBinder token);

    /**
     * Returns the camera id for a given binder token. Note that this id corresponds to the id of
     * the camera device in the camera framework.
     */
    @utf8InCpp String getCameraId(in IBinder token);

    /**
     * Registers a new camera with an **explicit, caller-chosen camera id**.
     *
     * ★★ 为什么需要它(2026-08-12 ovaltine 远程桌面真机踩到):
     *   默认的 registerCamera() 用一个进程内单调自增计数器分配 id
     *   (VirtualCameraService.cc: sNextIdNumericalPortion,起始 1000),
     *   于是**每次会话拿到的 id 都不一样**:v0_1002/1003 → v0_1004/1005 → …
     *
     *   而相机 App **会把"上次用的是哪台相机"持久化**(CameraX 的
     *   CameraSelector 按 id 精确匹配)。会话结束、虚拟相机注销之后,
     *   App 下次启动仍拿旧 id 去找 → 找不到 → 直接抛异常闪退:
     *     java.lang.IllegalStateException: Unable to find camera with id
     *         v0_1001 from list of available cameras.
     *   同一个陈旧 id 还会让 cameraserver 在 provider 拆除竞态下 SIGSEGV
     *   (findDeviceInfoLocked 读空指针)。
     *
     *   ⇒ 让调用方钉死 id,每次会话都注册成**同一个** id,
     *     App 记住的 id 就永远有效,上面两个问题一起消失。
     *
     * ★ 实现上直接复用已有的 registerCameraNoCheck 路径 —— C++ 侧本来就有
     *   带 cameraId 的重载(给 `cmd` 测试相机用),只是没暴露到 AIDL。
     *
     * ⚠️ 调用方必须保证 id 唯一且不与真实相机冲突(建议用 v<deviceId>_9xxx 段)。
     *
     * @return true if the camera was successfully registered
     */
    boolean registerCameraWithId(in IBinder token,
            in VirtualCameraConfiguration configuration,
            @utf8InCpp String cameraId, int deviceId);
}
