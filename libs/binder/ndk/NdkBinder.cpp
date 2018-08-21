/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "NdkBinder.h"

#include "AIBinder_internal.h"
#include "AParcel_internal.h"

#include <android-base/logging.h>

namespace android {

void LocalNdkBinder::setAIBinder(AIBinder* binder) {
    mBinder = binder;
}

const String16& LocalNdkBinder::getInterfaceDescriptor() const {
    return mBinder->getClass()->getInterfaceDescriptor();
}

binder_status_t LocalNdkBinder::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                           uint32_t flags) {
    // FIXME: doing it this way too weird? I think it makes more sense
    if (code >= FIRST_CALL_TRANSACTION && code < LAST_CALL_TRANSACTION) {
        // FIXME: check interface token
        // FIXME: write status
        const AParcel in = AParcel::readOnly(&data);
        AParcel out = AParcel(reply, false /*owns*/);

        return mBinder->getClass()->onTransact(code, mBinder, &in, &out);
    } else {
        return BBinder::onTransact(code, data, reply, flags);
    }
}

RemoteNdkBinder::RemoteNdkBinder(const sp<IBinder>& remote) : BppBinder(remote) {
    // FIXME: see comment in AIBinder.cpp newRemoteBinder with regards to remote binder interface
    // descriptor

    // guarantee that the interface descriptor is cached
    remote->getInterfaceDescriptor();
}

status_t RemoteNdkBinder::transact(uint32_t code, const Parcel& data, Parcel* reply,
                                   uint32_t flags) {
    if (code >= FIRST_CALL_TRANSACTION && code < LAST_CALL_TRANSACTION) {
        // FIXME: write interface token to transaction if in range
        return remote()->transact(code, data, reply, flags);
    } else {
        LOG(ERROR) << "Currently, only-user defined transactions are allowed for interfaces "
                      "defined in the NDK.";
        // return remote()->transact(code, data, reply, flags);
        return EX_UNSUPPORTED_OPERATION;
    }
}

} // namespace android