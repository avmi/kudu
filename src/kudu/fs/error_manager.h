// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <glog/logging.h>

#include "kudu/fs/dir_manager.h"
#include "kudu/fs/dir_util.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/port.h"
#include "kudu/gutil/ref_counted.h"

namespace kudu {
namespace fs {

// Callback to error-handling code. The input string is the UUID a failed
// component and the ID of the corresponding tenant.
//
// e.g. the ErrorNotificationCb for disk failure handling takes the UUID of a
// directory, marks it failed, and shuts down the tablets in that directory.
typedef std::function<void(const std::string&, const std::string&)> ErrorNotificationCb;

// Evaluates the expression and handles it if it results in an error.
// Returns if the status is an error.
#define RETURN_NOT_OK_HANDLE_ERROR(status_expr) do { \
  const Status& _s = (status_expr); \
  if (PREDICT_TRUE(_s.ok())) { \
    break; \
  } \
  HandleError(_s); \
  return _s; \
} while (0)

// Evaluates the expression and runs 'err_handler' if it results in a disk
// failure. Returns if the expression results in an error.
#define RETURN_NOT_OK_HANDLE_DISK_FAILURE(status_expr, err_handler) do { \
  const Status& _s = (status_expr); \
  if (PREDICT_TRUE(_s.ok())) { \
    break; \
  } \
  if (_s.IsDiskFailure()) { \
    (err_handler); \
  } \
  return _s; \
} while (0)

// Evaluates the expression and runs 'err_handler' if it results in a
// corruption. Returns if the expression results in an error.
#define RETURN_NOT_OK_HANDLE_CORRUPTION(status_expr, err_handler) do { \
  const Status& _s = (status_expr); \
  if (PREDICT_TRUE(_s.ok())) { \
    break; \
  } \
  if (_s.IsCorruption()) { \
    (err_handler); \
  } \
  return _s; \
} while (0)

// Evaluates the expression and runs 'err_handler' if it results in a disk
// failure.
#define HANDLE_DISK_FAILURE(status_expr, err_handler) do { \
  const Status& _s = (status_expr); \
  if (PREDICT_FALSE(_s.IsDiskFailure())) { \
    (err_handler); \
  } \
} while (0)

enum ErrorHandlerType : uint8_t {
  // For disk failures.
  DISK_ERROR = 0,

  // For errors that caused by no data dirs being available (e.g. if all disks
  // are full or failed when creating a block).
  //
  // TODO(awong): Register an actual error-handling callback for
  // NO_AVAILABLE_DISKS. Some errors may surface indirectly due to disk errors,
  // but may not have touched disk, and thus may have not called the DISK_ERROR
  // error handler.
  //
  // For example, if all of the disks in a tablet's directory group have
  // already failed due to disk errors, the tablet would not be able to create
  // a new block and return an error, despite CreateNewBlock() not actually
  // touching disk and triggering running error handling. Callers of
  // CreateNewBlock() will expect that if an error is returned, it has been
  // handled, and may hit a CHECK failure otherwise. As such, before returning
  // an error, CreateNewBlock() must wait for any in-flight error handling to
  // finish.
  //
  // While this currently runs a no-op, it serves to enforce that any
  // error-handling caused by ERROR1 that may have indirectly caused ERROR2
  // (e.g. if ERROR1 is a disk error of the only disk on the server, and ERROR2
  // is the subsequent failure to create a block because all disks have been
  // marked as failed) must complete before ERROR2 can be returned to its
  // caller.
  NO_AVAILABLE_DISKS = 1,

  // For CFile corruptions.
  CFILE_CORRUPTION = 2,

  // For broken invariants caused by KUDU-2233.
  KUDU_2233_CORRUPTION = 3,

  // Update ERROR_HANDLER_TYPE_MAX if adding new elements into the enum.
  ERROR_HANDLER_TYPE_MAX = KUDU_2233_CORRUPTION,
};

// When certain operations fail, the side effects of the error can span multiple
// layers, many of which we prefer to keep separate. The FsErrorManager
// registers and runs error handlers without adding cross-layer dependencies.
// Additionally, it enforces one callback is run at a time, and that each
// callback fully completes before returning.
//
// e.g. the TSTabletManager registers a callback to handle disk failure.
// Blocks and other entities that may hit disk failures can call it without
// knowing about the TSTabletManager.
class FsErrorManager : public RefCountedThreadSafe<FsErrorManager> {
 public:
  FsErrorManager();
  ~FsErrorManager() = default;

  // Sets the error notification callback.
  //
  // This should be called when the callback's callee is initialized.
  void SetErrorNotificationCb(ErrorHandlerType e, ErrorNotificationCb cb);

  // Resets the error notification callback.
  //
  // This must be called before the callback's callee is destroyed.
  void UnsetErrorNotificationCb(ErrorHandlerType e);

  // Runs the error notification callback.
  //
  // 'uuid' is the full UUID of the component that failed.
  // 'tenant_id' is used to indicate the corresponding tenant, if not specified,
  // we will treat it as the default tenant.
  void RunErrorNotificationCb(
      ErrorHandlerType e,
      const std::string& uuid,
      const std::string& tenant_id = fs::kDefaultTenantID) const;

  // Runs the error notification callback with the UUID of 'dir'.
  //
  // 'tenant_id' is used to indicate the corresponding tenant, if not specified,
  // we will treat it as the default tenant.
  void RunErrorNotificationCb(ErrorHandlerType e,
                              const Dir* dir,
                              const std::string& tenant_id = fs::kDefaultTenantID) const {
    DCHECK_EQ(e, ErrorHandlerType::DISK_ERROR);
    RunErrorNotificationCb(e, dir->instance()->uuid(), tenant_id);
  }

 private:
  // Callbacks to be run when an error occurs.
  std::array<ErrorNotificationCb,
             ErrorHandlerType::ERROR_HANDLER_TYPE_MAX + 1> callbacks_;

  // Protects calls to notifications, enforcing that a single callback runs at
  // a time. Since callbacks might lead to IO and memory allocation, using a
  // busy-waiting primitive isn't an option here.
  mutable std::mutex lock_;

  DISALLOW_COPY_AND_ASSIGN(FsErrorManager);
};

}  // namespace fs
}  // namespace kudu
