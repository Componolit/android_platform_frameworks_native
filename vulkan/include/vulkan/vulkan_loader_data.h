/*
 * Copyright 2015 The Android Open Source Project
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

#ifndef VULKAN_VULKAN_LOADER_DATA_H
#define VULKAN_VULKAN_LOADER_DATA_H

#include <string>

namespace android {
class NativeLoaderNamespace;
}

namespace vulkan {
    struct LoaderData {
        std::string layer_path;
        android::NativeLoaderNamespace* app_namespace;

        __attribute__((visibility("default"))) static LoaderData& GetInstance();
    };
}

#endif
