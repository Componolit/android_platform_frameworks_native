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

#include <binder/Stability.h>

namespace android {

void Stability::markCompilationUnit(const sp<IBinder>& /*binder*/) {
    // TODO(b/136027762): implement
}

void Stability::markVintf(const sp<IBinder>& /*binder*/) {
    // TODO(b/136027762): implement
}

void Stability::markApex(const sp<IBinder>& /*binder*/) {
    // TODO(b/136027762): implement
}

void Stability::markUniversal(const sp<IBinder>& /*binder*/) {
    // TODO(b/136027762): implement
}

}  // namespace stability
