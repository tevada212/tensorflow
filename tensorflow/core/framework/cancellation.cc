/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/framework/cancellation.h"

#include "absl/memory/memory.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/logging.h"

namespace tensorflow {

const CancellationToken CancellationManager::kInvalidToken = -1;

CancellationManager::CancellationManager()
    : is_cancelling_(false),
      is_cancelled_(false),
      next_cancellation_token_(0) {}

CancellationManager::CancellationManager(CancellationManager* parent)
    : is_cancelling_(false),
      is_cancelled_(false),
      next_cancellation_token_(0),
      parent_(parent),
      parent_token_(parent->get_cancellation_token()) {
  bool registered = parent->RegisterCallback(parent_token_,
                                             [this]() { this->StartCancel(); });
  if (!registered) {
    is_cancelled_ = true;
  }
}

void CancellationManager::StartCancel() {
  gtl::FlatMap<CancellationToken, CancelCallback> callbacks_to_run;
  Notification* cancelled_notification = nullptr;
  {
    mutex_lock l(mu_);
    if (is_cancelled_.load(std::memory_order_relaxed) || is_cancelling_) {
      return;
    }
    is_cancelling_ = true;
    if (state_) {
      std::swap(state_->callbacks, callbacks_to_run);
      cancelled_notification = &state_->cancelled_notification;
    }
  }
  // We call these callbacks without holding mu_, so that concurrent
  // calls to DeregisterCallback, which can happen asynchronously, do
  // not block. The callbacks remain valid because any concurrent call
  // to DeregisterCallback will block until the
  // cancelled_notification_ is notified.
  for (auto key_and_value : callbacks_to_run) {
    key_and_value.second();
  }
  {
    mutex_lock l(mu_);
    is_cancelling_ = false;
    is_cancelled_.store(true, std::memory_order_release);
  }
  if (cancelled_notification) {
    cancelled_notification->Notify();
  }
}

bool CancellationManager::RegisterCallback(CancellationToken token,
                                           CancelCallback callback) {
  DCHECK_LT(token, next_cancellation_token_) << "Invalid cancellation token";
  mutex_lock l(mu_);
  bool should_register = !is_cancelled_ && !is_cancelling_;
  if (should_register) {
    if (!state_) {
      state_ = absl::make_unique<State>();
    }
    std::swap(state_->callbacks[token], callback);
  }
  return should_register;
}

bool CancellationManager::DeregisterCallback(CancellationToken token) {
  mu_.lock();
  if (is_cancelled_) {
    mu_.unlock();
    return false;
  } else if (is_cancelling_) {
    Notification* cancelled_notification =
        state_ ? &state_->cancelled_notification : nullptr;
    mu_.unlock();
    // Wait for all of the cancellation callbacks to be called. This
    // wait ensures that the caller of DeregisterCallback does not
    // return immediately and free objects that may be used in the
    // execution of any currently pending callbacks in StartCancel.
    if (cancelled_notification) {
      cancelled_notification->WaitForNotification();
    }
    return false;
  } else {
    if (state_) {
      state_->callbacks.erase(token);
    }
    mu_.unlock();
    return true;
  }
}

bool CancellationManager::TryDeregisterCallback(CancellationToken token) {
  mutex_lock lock(mu_);
  if (is_cancelled_ || is_cancelling_) {
    return false;
  } else {
    if (state_) {
      state_->callbacks.erase(token);
    }
    return true;
  }
}

CancellationManager::~CancellationManager() {
  if (parent_) {
    parent_->DeregisterCallback(parent_token_);
  }
  if (state_) {
    StartCancel();
  }
}

}  // end namespace tensorflow
