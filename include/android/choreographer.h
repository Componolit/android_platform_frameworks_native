/*
 * Copyright (C) 2015 The Android Open Source Project
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

/**
 * @addtogroup Choreographer
 * @{
 */

/**
 * @file choreographer.h
 */

#ifndef ANDROID_CHOREOGRAPHER_H
#define ANDROID_CHOREOGRAPHER_H

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct AChoreographer;
typedef struct AChoreographer AChoreographer;

/**
 * Prototype of the function that is called when a new frame is being rendered.
 * It's passed the time that the frame is being rendered as nanoseconds in the
 * CLOCK_MONOTONIC time base, as well as the data pointer provided by the
 * application that registered a callback. All callbacks that run as part of
 * rendering a frame will observe the same frame time, so it should be used
 * whenever events need to be synchronized (e.g. animations).
 */
typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void* data);

/**
 * Prototype of the function that is called when a new frame is being rendered.
 * It's passed the time that the frame is being rendered as nanoseconds in the
 * CLOCK_MONOTONIC time base, as well as the data pointer provided by the
 * application that registered a callback. All callbacks that run as part of
 * rendering a frame will observe the same frame time, so it should be used
 * whenever events need to be synchronized (e.g. animations).
 */
typedef void (*AChoreographer_frameCallback64)(int64_t frameTimeNanos, void* data);

#if __ANDROID_API__ >= 24

/**
 * Get the AChoreographer instance for the current thread. This must be called
 * on an ALooper thread.
 *
 * Available since API level 24.
 */
AChoreographer* AChoreographer_getInstance() __INTRODUCED_IN(24);

/**
 * Deprecated: Use AChoreographer_postFrameCallback64 instead.
 */
void AChoreographer_postFrameCallback(AChoreographer* choreographer,
        AChoreographer_frameCallback callback, void* data) __INTRODUCED_IN(24) __DEPRECATED_IN(29);

/**
 * Deprecated: Use AChoreographer_postFrameCallbackDelayed64 instead.
 */
void AChoreographer_postFrameCallbackDelayed(AChoreographer* choreographer,
                AChoreographer_frameCallback callback, void* data,
                long delayMillis) __INTRODUCED_IN(24) __DEPRECATED_IN(29);

#endif /* __ANDROID_API__ >= 24 */

#if __ANDROID_API__ >= 29

/**
 * Power a callback to be run on the next frame.  The data pointer provided will
 * be passed to the callback function when it's called.
 *
 * Available since API level 29.
 */
void AChoreographer_postFrameCallback64(AChoreographer* chroreographer,
                AChoreographer_frameCallback64 callback, void* data) __INTRODUCED_IN(29);

/**
 * Post a callback to be run on the frame following the specified delay.  The
 * data pointer provided will be passed to the callback function when it's
 * called.
 *
 * Available since API level 29.
 */
void AChoreographer_postFrameCallbackDelayed64(AChoreographer* choreographer,
                AChoreographer_frameCallback64 callback, void* data, uint32_t delayMillis) __INTRODUCED_IN(29);

#endif /* __ANDROID_API__ >= 29 */

__END_DECLS

#endif // ANDROID_CHOREOGRAPHER_H

/** @} */
