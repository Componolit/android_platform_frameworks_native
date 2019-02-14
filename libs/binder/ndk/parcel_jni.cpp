/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <android/binder_parcel_jni.h>
#include "parcel_internal.h"

#include "lazy_android_runtime.h"

using android::Parcel;

AParcel* AParcel_fromJavaParcel(JNIEnv* env, jobject parcel) {
    if (env == nullptr || parcel == nullptr) {
        return nullptr;
    }

    LazyAndroidRuntime::load();
    if (LazyAndroidRuntime::parcelForJavaObject == nullptr) {
        return nullptr;
    }

    Parcel* platformParcel = LazyAndroidRuntime::parcelForJavaObject(env, parcel);
    if (platformParcel == nullptr) {
        return nullptr;
    }

    return new AParcel(nullptr /*binder*/, platformParcel, false /*owns*/);
}
