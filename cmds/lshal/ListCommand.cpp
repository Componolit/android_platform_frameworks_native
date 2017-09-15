/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "ListCommand.h"

#include <getopt.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <regex>

#include <android-base/parseint.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <hidl-util/FQName.h>
#include <private/android_filesystem_config.h>
#include <sys/stat.h>
#include <vintf/HalManifest.h>
#include <vintf/parse_xml.h>

#include "Lshal.h"
#include "PipeRelay.h"
#include "Timeout.h"
#include "utils.h"

using ::android::hardware::hidl_string;
using ::android::hidl::manager::V1_0::IServiceManager;

namespace android {
namespace lshal {

NullableOStream<std::ostream> ListCommand::out() const {
    return mLshal.out();
}

NullableOStream<std::ostream> ListCommand::err() const {
    return mLshal.err();
}

std::string ListCommand::GetName() {
    return "list";
}
std::string ListCommand::getSimpleDescription() const {
    return "List HALs.";
}

std::string ListCommand::parseCmdline(pid_t pid) const {
    std::ifstream ifs("/proc/" + std::to_string(pid) + "/cmdline");
    std::string cmdline;
    if (!ifs.is_open()) {
        return "";
    }
    ifs >> cmdline;
    return cmdline;
}

const std::string &ListCommand::getCmdline(pid_t pid) {
    auto pair = mCmdlines.find(pid);
    if (pair != mCmdlines.end()) {
        return pair->second;
    }
    mCmdlines[pid] = parseCmdline(pid);
    return mCmdlines[pid];
}

void ListCommand::removeDeadProcesses(Pids *pids) {
    static const pid_t myPid = getpid();
    pids->erase(std::remove_if(pids->begin(), pids->end(), [this](auto pid) {
        return pid == myPid || this->getCmdline(pid).empty();
    }), pids->end());
}

static bool scanBinderContext(pid_t pid,
        const std::string &contextName,
        std::function<void(const std::string&)> eachLine) {
    std::ifstream ifs("/d/binder/proc/" + std::to_string(pid));
    if (!ifs.is_open()) {
        return false;
    }

    static const std::regex kContextLine("^context (\\w+)$");

    bool isDesiredContext = false;
    std::string line;
    std::smatch match;
    while(getline(ifs, line)) {
        if (std::regex_search(line, match, kContextLine)) {
            isDesiredContext = match.str(1) == contextName;
            continue;
        }

        if (!isDesiredContext) {
            continue;
        }

        eachLine(line);
    }
    return true;
}

bool ListCommand::getPidInfo(
        pid_t serverPid, PidInfo *pidInfo) const {
    static const std::regex kReferencePrefix("^\\s*node \\d+:\\s+u([0-9a-f]+)\\s+c([0-9a-f]+)\\s+");
    static const std::regex kThreadPrefix("^\\s*thread \\d+:\\s+l\\s+(\\d)(\\d)");

    std::smatch match;
    return scanBinderContext(serverPid, "hwbinder", [&](const std::string& line) {
        if (std::regex_search(line, match, kReferencePrefix)) {
            const std::string &ptrString = "0x" + match.str(2); // use number after c
            uint64_t ptr;
            if (!::android::base::ParseUint(ptrString.c_str(), &ptr)) {
                // Should not reach here, but just be tolerant.
                err() << "Could not parse number " << ptrString << std::endl;
                return;
            }
            const std::string proc = " proc ";
            auto pos = line.rfind(proc);
            if (pos != std::string::npos) {
                for (const std::string &pidStr : split(line.substr(pos + proc.size()), ' ')) {
                    int32_t pid;
                    if (!::android::base::ParseInt(pidStr, &pid)) {
                        err() << "Could not parse number " << pidStr << std::endl;
                        return;
                    }
                    pidInfo->refPids[ptr].push_back(pid);
                }
            }

            return;
        }

        if (std::regex_search(line, match, kThreadPrefix)) {
            // "1" is waiting in binder driver
            // "2" is poll. It's impossible to tell if these are in use.
            //     and HIDL default code doesn't use it.
            bool isInUse = match.str(1) != "1";
            // "0" is a thread that has called into binder
            // "1" is looper thread
            // "2" is main looper thread
            bool isHwbinderThread = match.str(2) != "0";

            if (!isHwbinderThread) {
                return;
            }

            if (isInUse) {
                pidInfo->threadUsage++;
            }

            pidInfo->threadCount++;
            return;
        }

        // not reference or thread line
        return;
    });
}

const PidInfo* ListCommand::getPidInfoCached(pid_t serverPid) {
    auto pair = mCachedPidInfos.insert({serverPid, PidInfo{}});
    if (pair.second /* did insertion take place? */) {
        if (!getPidInfo(serverPid, &pair.first->second)) {
            return nullptr;
        }
    }
    return &pair.first->second;
}

// Must process hwbinder services first, then passthrough services.
void ListCommand::forEachTable(const std::function<void(Table &)> &f) {
    f(mServicesTable);
    f(mPassthroughRefTable);
    f(mImplementationsTable);
}
void ListCommand::forEachTable(const std::function<void(const Table &)> &f) const {
    f(mServicesTable);
    f(mPassthroughRefTable);
    f(mImplementationsTable);
}

void ListCommand::postprocess() {
    forEachTable([this](Table &table) {
        if (mSortColumn) {
            std::sort(table.begin(), table.end(), mSortColumn);
        }
        for (TableEntry &entry : table) {
            entry.serverCmdline = getCmdline(entry.serverPid);
            removeDeadProcesses(&entry.clientPids);
            for (auto pid : entry.clientPids) {
                entry.clientCmdlines.push_back(this->getCmdline(pid));
            }
        }
    });
    // use a double for loop here because lshal doesn't care about efficiency.
    for (TableEntry &packageEntry : mImplementationsTable) {
        std::string packageName = packageEntry.interfaceName;
        FQName fqPackageName{packageName.substr(0, packageName.find("::"))};
        if (!fqPackageName.isValid()) {
            continue;
        }
        for (TableEntry &interfaceEntry : mPassthroughRefTable) {
            if (interfaceEntry.arch != ARCH_UNKNOWN) {
                continue;
            }
            FQName interfaceName{splitFirst(interfaceEntry.interfaceName, '/').first};
            if (!interfaceName.isValid()) {
                continue;
            }
            if (interfaceName.getPackageAndVersion() == fqPackageName) {
                interfaceEntry.arch = packageEntry.arch;
            }
        }
    }

    mServicesTable.setDescription(
            "All binderized services (registered services through hwservicemanager)");
    mPassthroughRefTable.setDescription(
            "All interfaces that getService() has ever return as a passthrough interface;\n"
            "PIDs / processes shown below might be inaccurate because the process\n"
            "might have relinquished the interface or might have died.\n"
            "The Server / Server CMD column can be ignored.\n"
            "The Clients / Clients CMD column shows all process that have ever dlopen'ed \n"
            "the library and successfully fetched the passthrough implementation.");
    mImplementationsTable.setDescription(
            "All available passthrough implementations (all -impl.so files)");
}

static inline bool findAndBumpVersion(vintf::ManifestHal* hal, const vintf::Version& version) {
    for (vintf::Version& v : hal->versions) {
        if (v.majorVer == version.majorVer) {
            v.minorVer = std::max(v.minorVer, version.minorVer);
            return true;
        }
    }
    return false;
}

void ListCommand::dumpVintf(const NullableOStream<std::ostream>& out) const {
    using vintf::operator|=;
    out << "<!-- " << std::endl
         << "    This is a skeleton device manifest. Notes: " << std::endl
         << "    1. android.hidl.*, android.frameworks.*, android.system.* are not included." << std::endl
         << "    2. If a HAL is supported in both hwbinder and passthrough transport, " << std::endl
         << "       only hwbinder is shown." << std::endl
         << "    3. It is likely that HALs in passthrough transport does not have" << std::endl
         << "       <interface> declared; users will have to write them by hand." << std::endl
         << "    4. A HAL with lower minor version can be overridden by a HAL with" << std::endl
         << "       higher minor version if they have the same name and major version." << std::endl
         << "    5. sepolicy version is set to 0.0. It is recommended that the entry" << std::endl
         << "       is removed from the manifest file and written by assemble_vintf" << std::endl
         << "       at build time." << std::endl
         << "-->" << std::endl;

    vintf::HalManifest manifest;
    forEachTable([this, &manifest] (const Table &table) {
        for (const TableEntry &entry : table) {

            std::string fqInstanceName = entry.interfaceName;

            if (&table == &mImplementationsTable) {
                // Quick hack to work around *'s
                replaceAll(&fqInstanceName, '*', 'D');
            }
            auto splittedFqInstanceName = splitFirst(fqInstanceName, '/');
            FQName fqName(splittedFqInstanceName.first);
            if (!fqName.isValid()) {
                err() << "Warning: '" << splittedFqInstanceName.first
                     << "' is not a valid FQName." << std::endl;
                continue;
            }
            // Strip out system libs.
            if (fqName.inPackage("android.hidl") ||
                fqName.inPackage("android.frameworks") ||
                fqName.inPackage("android.system")) {
                continue;
            }
            std::string interfaceName =
                    &table == &mImplementationsTable ? "" : fqName.name();
            std::string instanceName =
                    &table == &mImplementationsTable ? "" : splittedFqInstanceName.second;

            vintf::Version version{fqName.getPackageMajorVersion(),
                                   fqName.getPackageMinorVersion()};
            vintf::Transport transport;
            vintf::Arch arch;
            if (entry.transport == "hwbinder") {
                transport = vintf::Transport::HWBINDER;
                arch = vintf::Arch::ARCH_EMPTY;
            } else if (entry.transport == "passthrough") {
                transport = vintf::Transport::PASSTHROUGH;
                switch (entry.arch) {
                    case lshal::ARCH32:
                        arch = vintf::Arch::ARCH_32;    break;
                    case lshal::ARCH64:
                        arch = vintf::Arch::ARCH_64;    break;
                    case lshal::ARCH_BOTH:
                        arch = vintf::Arch::ARCH_32_64; break;
                    case lshal::ARCH_UNKNOWN: // fallthrough
                    default:
                        err() << "Warning: '" << fqName.package()
                             << "' doesn't have bitness info, assuming 32+64." << std::endl;
                        arch = vintf::Arch::ARCH_32_64;
                }
            } else {
                err() << "Warning: '" << entry.transport << "' is not a valid transport." << std::endl;
                continue;
            }

            bool done = false;
            for (vintf::ManifestHal *hal : manifest.getHals(fqName.package())) {
                if (hal->transport() != transport) {
                    if (transport != vintf::Transport::PASSTHROUGH) {
                        err() << "Fatal: should not reach here. Generated result may be wrong for '"
                             << hal->name << "'."
                             << std::endl;
                    }
                    done = true;
                    break;
                }
                if (findAndBumpVersion(hal, version)) {
                    if (&table != &mImplementationsTable) {
                        hal->interfaces[interfaceName].name = interfaceName;
                        hal->interfaces[interfaceName].instances.insert(instanceName);
                    }
                    hal->transportArch.arch |= arch;
                    done = true;
                    break;
                }
            }
            if (done) {
                continue; // to next TableEntry
            }
            decltype(vintf::ManifestHal::interfaces) interfaces;
            if (&table != &mImplementationsTable) {
                interfaces[interfaceName].name = interfaceName;
                interfaces[interfaceName].instances.insert(instanceName);
            }
            if (!manifest.add(vintf::ManifestHal{
                    .format = vintf::HalFormat::HIDL,
                    .name = fqName.package(),
                    .versions = {version},
                    .transportArch = {transport, arch},
                    .interfaces = interfaces})) {
                err() << "Warning: cannot add hal '" << fqInstanceName << "'" << std::endl;
            }
        }
    });
    out << vintf::gHalManifestConverter(manifest);
}

static Architecture fromBaseArchitecture(::android::hidl::base::V1_0::DebugInfo::Architecture a) {
    switch (a) {
        case ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_64BIT:
            return ARCH64;
        case ::android::hidl::base::V1_0::DebugInfo::Architecture::IS_32BIT:
            return ARCH32;
        case ::android::hidl::base::V1_0::DebugInfo::Architecture::UNKNOWN: // fallthrough
        default:
            return ARCH_UNKNOWN;
    }
}

void ListCommand::dumpTable(const NullableOStream<std::ostream>& out) const {
    if (mNeat) {
        MergedTable({&mServicesTable, &mPassthroughRefTable, &mImplementationsTable})
            .createTextTable().dump(out.buf());
        return;
    }

    forEachTable([this, &out](const Table &table) {

        // We're only interested in dumping debug info for already
        // instantiated services. There's little value in dumping the
        // debug info for a service we create on the fly, so we only operate
        // on the "mServicesTable".
        std::function<std::string(const std::string&)> emitDebugInfo = nullptr;
        if (mEmitDebugInfo && &table == &mServicesTable) {
            emitDebugInfo = [this](const auto& iName) {
                std::stringstream ss;
                auto pair = splitFirst(iName, '/');
                mLshal.emitDebugInfo(pair.first, pair.second, {}, ss,
                                     NullableOStream<std::ostream>(nullptr));
                return ss.str();
            };
        }
        table.createTextTable(mNeat, emitDebugInfo).dump(out.buf());
        out << std::endl;
    });
}

Status ListCommand::dump() {
    auto dump = mVintf ? &ListCommand::dumpVintf : &ListCommand::dumpTable;

    if (mFileOutputPath.empty()) {
        (*this.*dump)(out());
        return OK;
    }

    std::ofstream fileOutput(mFileOutputPath);
    if (!fileOutput.is_open()) {
        err() << "Could not open file '" << mFileOutputPath << "'." << std::endl;
        return IO_ERROR;
    }
    chown(mFileOutputPath.c_str(), AID_SHELL, AID_SHELL);

    (*this.*dump)(NullableOStream<std::ostream>(fileOutput));

    fileOutput.flush();
    fileOutput.close();
    return OK;
}

void ListCommand::putEntry(TableEntrySource source, TableEntry &&entry) {
    Table *table = nullptr;
    switch (source) {
        case HWSERVICEMANAGER_LIST :
            table = &mServicesTable; break;
        case PTSERVICEMANAGER_REG_CLIENT :
            table = &mPassthroughRefTable; break;
        case LIST_DLLIB :
            table = &mImplementationsTable; break;
        default:
            err() << "Error: Unknown source of entry " << source << std::endl;
    }
    if (table) {
        table->add(std::forward<TableEntry>(entry));
    }
}

Status ListCommand::fetchAllLibraries(const sp<IServiceManager> &manager) {
    using namespace ::android::hardware;
    using namespace ::android::hidl::manager::V1_0;
    using namespace ::android::hidl::base::V1_0;
    using std::literals::chrono_literals::operator""s;
    auto ret = timeoutIPC(2s, manager, &IServiceManager::debugDump, [&] (const auto &infos) {
        std::map<std::string, TableEntry> entries;
        for (const auto &info : infos) {
            std::string interfaceName = std::string{info.interfaceName.c_str()} + "/" +
                    std::string{info.instanceName.c_str()};
            entries.emplace(interfaceName, TableEntry{
                .interfaceName = interfaceName,
                .transport = "passthrough",
                .serverPid = NO_PID,
                .serverObjectAddress = NO_PTR,
                .clientPids = info.clientPids,
                .arch = ARCH_UNKNOWN
            }).first->second.arch |= fromBaseArchitecture(info.arch);
        }
        for (auto &&pair : entries) {
            putEntry(LIST_DLLIB, std::move(pair.second));
        }
    });
    if (!ret.isOk()) {
        err() << "Error: Failed to call list on getPassthroughServiceManager(): "
             << ret.description() << std::endl;
        return DUMP_ALL_LIBS_ERROR;
    }
    return OK;
}

Status ListCommand::fetchPassthrough(const sp<IServiceManager> &manager) {
    using namespace ::android::hardware;
    using namespace ::android::hardware::details;
    using namespace ::android::hidl::manager::V1_0;
    using namespace ::android::hidl::base::V1_0;
    auto ret = timeoutIPC(manager, &IServiceManager::debugDump, [&] (const auto &infos) {
        for (const auto &info : infos) {
            if (info.clientPids.size() <= 0) {
                continue;
            }
            putEntry(PTSERVICEMANAGER_REG_CLIENT, {
                .interfaceName =
                        std::string{info.interfaceName.c_str()} + "/" +
                        std::string{info.instanceName.c_str()},
                .transport = "passthrough",
                .serverPid = info.clientPids.size() == 1 ? info.clientPids[0] : NO_PID,
                .serverObjectAddress = NO_PTR,
                .clientPids = info.clientPids,
                .arch = fromBaseArchitecture(info.arch)
            });
        }
    });
    if (!ret.isOk()) {
        err() << "Error: Failed to call debugDump on defaultServiceManager(): "
             << ret.description() << std::endl;
        return DUMP_PASSTHROUGH_ERROR;
    }
    return OK;
}

Status ListCommand::fetchBinderized(const sp<IServiceManager> &manager) {
    using namespace ::std;
    using namespace ::android::hardware;
    using namespace ::android::hidl::manager::V1_0;
    using namespace ::android::hidl::base::V1_0;
    const std::string mode = "hwbinder";

    hidl_vec<hidl_string> fqInstanceNames;
    // copying out for timeoutIPC
    auto listRet = timeoutIPC(manager, &IServiceManager::list, [&] (const auto &names) {
        fqInstanceNames = names;
    });
    if (!listRet.isOk()) {
        err() << "Error: Failed to list services for " << mode << ": "
             << listRet.description() << std::endl;
        return DUMP_BINDERIZED_ERROR;
    }

    Status status = OK;
    // server pid, .ptr value of binder object, child pids
    std::map<std::string, DebugInfo> allDebugInfos;
    std::map<pid_t, PidInfo> allPids;
    for (const auto &fqInstanceName : fqInstanceNames) {
        const auto pair = splitFirst(fqInstanceName, '/');
        const auto &serviceName = pair.first;
        const auto &instanceName = pair.second;
        auto getRet = timeoutIPC(manager, &IServiceManager::get, serviceName, instanceName);
        if (!getRet.isOk()) {
            err() << "Warning: Skipping \"" << fqInstanceName << "\": "
                 << "cannot be fetched from service manager:"
                 << getRet.description() << std::endl;
            status |= DUMP_BINDERIZED_ERROR;
            continue;
        }
        sp<IBase> service = getRet;
        if (service == nullptr) {
            err() << "Warning: Skipping \"" << fqInstanceName << "\": "
                 << "cannot be fetched from service manager (null)"
                 << std::endl;
            status |= DUMP_BINDERIZED_ERROR;
            continue;
        }
        auto debugRet = timeoutIPC(service, &IBase::getDebugInfo, [&] (const auto &debugInfo) {
            allDebugInfos[fqInstanceName] = debugInfo;
            if (debugInfo.pid >= 0) {
                allPids[static_cast<pid_t>(debugInfo.pid)] = PidInfo();
            }
        });
        if (!debugRet.isOk()) {
            err() << "Warning: Skipping \"" << fqInstanceName << "\": "
                 << "debugging information cannot be retrieved:"
                 << debugRet.description() << std::endl;
            status |= DUMP_BINDERIZED_ERROR;
        }
    }

    for (auto &pair : allPids) {
        pid_t serverPid = pair.first;
        if (!getPidInfo(serverPid, &allPids[serverPid])) {
            err() << "Warning: no information for PID " << serverPid
                      << ", are you root?" << std::endl;
            status |= DUMP_BINDERIZED_ERROR;
        }
    }
    for (const auto &fqInstanceName : fqInstanceNames) {
        auto it = allDebugInfos.find(fqInstanceName);
        if (it == allDebugInfos.end()) {
            putEntry(HWSERVICEMANAGER_LIST, {
                .interfaceName = fqInstanceName,
                .transport = mode,
                .serverPid = NO_PID,
                .serverObjectAddress = NO_PTR,
                .clientPids = {},
                .threadUsage = 0,
                .threadCount = 0,
                .arch = ARCH_UNKNOWN
            });
            continue;
        }
        const DebugInfo &info = it->second;
        bool writePidInfo = info.pid != NO_PID && info.ptr != NO_PTR;

        putEntry(HWSERVICEMANAGER_LIST, {
            .interfaceName = fqInstanceName,
            .transport = mode,
            .serverPid = info.pid,
            .serverObjectAddress = info.ptr,
            .clientPids = writePidInfo ? allPids[info.pid].refPids[info.ptr] : Pids{},
            .threadUsage = writePidInfo ? allPids[info.pid].threadUsage : 0,
            .threadCount = writePidInfo ? allPids[info.pid].threadCount : 0,
            .arch = fromBaseArchitecture(info.arch),
        });
    }
    return status;
}

Status ListCommand::fetch() {
    Status status = OK;
    auto bManager = mLshal.serviceManager();
    if (bManager == nullptr) {
        err() << "Failed to get defaultServiceManager()!" << std::endl;
        status |= NO_BINDERIZED_MANAGER;
    } else {
        status |= fetchBinderized(bManager);
        // Passthrough PIDs are registered to the binderized manager as well.
        status |= fetchPassthrough(bManager);
    }

    auto pManager = mLshal.passthroughManager();
    if (pManager == nullptr) {
        err() << "Failed to get getPassthroughServiceManager()!" << std::endl;
        status |= NO_PASSTHROUGH_MANAGER;
    } else {
        status |= fetchAllLibraries(pManager);
    }
    return status;
}

void ListCommand::registerAllOptions() {
    int v = mOptions.size();
    // A list of acceptable command line options
    // key: value returned by getopt_long
    // long options with short alternatives
    mOptions.push_back({'h', "help", no_argument, v++, [](ListCommand*, const char*) {
        return USAGE;
    }, ""});
    mOptions.push_back({'i', "interface", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::INTERFACE_NAME);
        return OK;
    }, "print the instance name column"});
    mOptions.push_back({'t', "transport", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::TRANSPORT);
        return OK;
    }, "print the transport mode column"});
    mOptions.push_back({'r', "arch", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::ARCH);
        return OK;
    }, "print the bitness column"});
    mOptions.push_back({'p', "pid", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::SERVER_PID);
        return OK;
    }, "print the server PID, or server cmdline if -m is set"});
    mOptions.push_back({'a', "address", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::SERVER_ADDR);
        return OK;
    }, "print the server object address column"});
    mOptions.push_back({'c', "clients", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::CLIENT_PIDS);
        return OK;
    }, "print the client PIDs, or client cmdlines if -m is set"});
    mOptions.push_back({'e', "threads", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mSelectedColumns.push_back(TableColumnType::THREADS);
        return OK;
    }, "print currently used/available threads\n(note, available threads created lazily)"});
    mOptions.push_back({'m', "cmdline", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mEnableCmdlines = true;
        return OK;
    }, "print cmdline instead of PIDs"});
    mOptions.push_back({'d', "debug", optional_argument, v++, [](ListCommand* thiz, const char* arg) {
        thiz->mEmitDebugInfo = true;
        if (arg) thiz->mFileOutputPath = arg;
        return OK;
    }, "Emit debug info from\nIBase::debug with empty options. Cannot be used with --neat.\n"
        "Writes to specified file if 'arg' is provided, otherwise stdout."});

    // long options without short alternatives
    mOptions.push_back({'\0', "init-vintf", no_argument, v++, [](ListCommand* thiz, const char* arg) {
        thiz->mVintf = true;
        if (arg) thiz->mFileOutputPath = arg;
        return OK;
    }, "form a skeleton HAL manifest to specified file,\nor stdout if no file specified."});
    mOptions.push_back({'\0', "sort", required_argument, v++, [](ListCommand* thiz, const char* arg) {
        if (strcmp(arg, "interface") == 0 || strcmp(arg, "i") == 0) {
            thiz->mSortColumn = TableEntry::sortByInterfaceName;
        } else if (strcmp(arg, "pid") == 0 || strcmp(arg, "p") == 0) {
            thiz->mSortColumn = TableEntry::sortByServerPid;
        } else {
            thiz->err() << "Unrecognized sorting column: " << arg << std::endl;
            return USAGE;
        }
        return OK;
    }, "sort by a column. 'arg' can be (i|interface) or (p|pid)."});
    mOptions.push_back({'\0', "neat", no_argument, v++, [](ListCommand* thiz, const char*) {
        thiz->mNeat = true;
        return OK;
    }, "output is machine parsable (no explanatory text).\nCannot be used with --debug."});
}

// Create 'longopts' argument to getopt_long. Caller is responsible for maintaining
// the lifetime of "options" during the usage of the returned array.
static std::unique_ptr<struct option[]> getLongOptions(
        const ListCommand::RegisteredOptions& options,
        int* longOptFlag) {
    std::unique_ptr<struct option[]> ret{new struct option[options.size() + 1]};
    int i = 0;
    for (const auto& e : options) {
        ret[i].name = e.longOption.c_str();
        ret[i].has_arg = e.hasArg;
        ret[i].flag = longOptFlag;
        ret[i].val = e.val;

        i++;
    }
    // getopt_long last option has all zeros
    ret[i].name = NULL;
    ret[i].has_arg = 0;
    ret[i].flag = NULL;
    ret[i].val = 0;

    return ret;
}

// Create 'optstring' argument to getopt_long.
static std::string getShortOptions(const ListCommand::RegisteredOptions& options) {
    std::stringstream ss;
    for (const auto& e : options) {
        if (e.shortOption != '\0') {
            ss << e.shortOption;
        }
    }
    return ss.str();
}

Status ListCommand::parseArgs(const Arg &arg) {

    if (mOptions.empty()) {
        registerAllOptions();
    }
    int longOptFlag;
    std::unique_ptr<struct option[]> longOptions = getLongOptions(mOptions, &longOptFlag);
    std::string shortOptions = getShortOptions(mOptions);

    // suppress output to std::err for unknown options
    opterr = 0;

    int optionIndex;
    int c;
    // Lshal::parseArgs has set optind to the next option to parse
    for (;;) {
        c = getopt_long(arg.argc, arg.argv,
                shortOptions.c_str(), longOptions.get(), &optionIndex);
        if (c == -1) {
            break;
        }
        const RegisteredOption* found = nullptr;
        if (c == 0) {
            // see long option
            for (const auto& e : mOptions) {
                if (longOptFlag == e.val) found = &e;
            }
        } else {
            // see short option
            for (const auto& e : mOptions) {
                if (c == e.shortOption) found = &e;
            }
        }

        if (found == nullptr) {
            // see unrecognized options
            err() << "unrecognized option `" << arg.argv[optind - 1] << "'" << std::endl;
            return USAGE;
        }

        Status status = found->op(this, optarg);
        if (status != OK) {
            return status;
        }
    }
    if (optind < arg.argc) {
        // see non option
        err() << "unrecognized option `" << arg.argv[optind] << "'" << std::endl;
        return USAGE;
    }

    if (mNeat && mEmitDebugInfo) {
        err() << "Error: --neat should not be used with --debug." << std::endl;
        return USAGE;
    }

    if (mSelectedColumns.empty()) {
        mSelectedColumns = {TableColumnType::INTERFACE_NAME, TableColumnType::THREADS,
                            TableColumnType::SERVER_PID, TableColumnType::CLIENT_PIDS};
    }

    if (mEnableCmdlines) {
        for (size_t i = 0; i < mSelectedColumns.size(); ++i) {
            if (mSelectedColumns[i] == TableColumnType::SERVER_PID) {
                mSelectedColumns[i] = TableColumnType::SERVER_CMD;
            }
            if (mSelectedColumns[i] == TableColumnType::CLIENT_PIDS) {
                mSelectedColumns[i] = TableColumnType::CLIENT_CMDS;
            }
        }
    }

    forEachTable([this] (Table& table) {
        table.setSelectedColumns(this->mSelectedColumns);
    });

    return OK;
}

Status ListCommand::main(const Arg &arg) {
    Status status = parseArgs(arg);
    if (status != OK) {
        return status;
    }
    status = fetch();
    postprocess();
    status |= dump();
    return status;
}

static std::vector<std::string> splitString(const std::string &s, char c) {
    std::vector<std::string> components;

    size_t startPos = 0;
    size_t matchPos;
    while ((matchPos = s.find(c, startPos)) != std::string::npos) {
        components.push_back(s.substr(startPos, matchPos - startPos));
        startPos = matchPos + 1;
    }

    if (startPos <= s.length()) {
        components.push_back(s.substr(startPos));
    }
    return components;
}

const std::string& ListCommand::RegisteredOption::getHelpMessageForArgument() const {
    static const std::string empty{};
    static const std::string optional{"[=<arg>]"};
    static const std::string required{"=<arg>"};

    if (hasArg == optional_argument) {
        return optional;
    }
    if (hasArg == required_argument) {
        return required;
    }
    return empty;
}

void ListCommand::usage() const {

    err() << "list:" << std::endl
          << "    lshal" << std::endl
          << "    lshal list" << std::endl
          << "        List all hals with default ordering and columns (`lshal list -iepc`)" << std::endl
          << "    lshal list [-h|--help]" << std::endl
          << "        -h, --help: Print help message for list (`lshal help list`)" << std::endl
          << "    lshal [list] [OPTIONS...]" << std::endl;
    for (const auto& e : mOptions) {
        if (e.help.empty()) {
            continue;
        }
        err() << "        ";
        if (e.shortOption != '\0')
            err() << "-" << e.shortOption << e.getHelpMessageForArgument();
        if (e.shortOption != '\0' && !e.longOption.empty())
            err() << ", ";
        if (!e.longOption.empty())
            err() << "--" << e.longOption << e.getHelpMessageForArgument();
        err() << ": ";
        std::vector<std::string> lines = splitString(e.help, '\n');
        for (const auto& line : lines) {
            if (&line != &lines.front())
                err() << "            ";
            err() << line << std::endl;
        }
    }
}

}  // namespace lshal
}  // namespace android

