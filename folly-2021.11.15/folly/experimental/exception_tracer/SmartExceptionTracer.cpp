/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/experimental/exception_tracer/SmartExceptionTracer.h>

#include <glog/logging.h>
#include <folly/MapUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/experimental/exception_tracer/ExceptionTracerLib.h>
#include <folly/experimental/exception_tracer/StackTrace.h>
#include <folly/experimental/symbolizer/Symbolizer.h>

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

#if defined(__GLIBCXX__)

namespace folly {
namespace exception_tracer {
namespace {

struct ExceptionMeta {
  void (*deleter)(void*);
  // normal stack trace
  StackTrace trace;
  // async stack trace
  StackTrace traceAsync;
};

using SynchronizedExceptionMeta = folly::Synchronized<ExceptionMeta>;

auto& getMetaMap() {
  // Leaky Meyers Singleton
  static Indestructible<Synchronized<
      folly::F14FastMap<void*, std::unique_ptr<SynchronizedExceptionMeta>>>>
      meta;
  return *meta;
}

void metaDeleter(void* ex) noexcept {
  auto syncMeta = getMetaMap().withWLock([ex](auto& locked) noexcept {
    auto iter = locked.find(ex);
    auto ret = std::move(iter->second);
    locked.erase(iter);
    return ret;
  });

  // If the thrown object was allocated statically it may not have a deleter.
  auto meta = syncMeta->wlock();
  if (meta->deleter) {
    meta->deleter(ex);
  }
}

// This callback runs when an exception is thrown so we can grab the stack
// trace. To manage the lifetime of the stack trace we override the deleter with
// our own wrapper.
void throwCallback(
    void* ex, std::type_info*, void (**deleter)(void*)) noexcept {
  // Make this code reentrant safe in case we throw an exception while
  // handling an exception. Thread local variables are zero initialized.
  static thread_local bool handlingThrow;
  if (handlingThrow) {
    return;
  }
  SCOPE_EXIT { handlingThrow = false; };
  handlingThrow = true;

  // This can allocate memory potentially causing problems in an OOM
  // situation so we catch and short circuit.
  try {
    auto newMeta = std::make_unique<SynchronizedExceptionMeta>();
    newMeta->withWLock([&deleter](auto& lockedMeta) {
      // Override the deleter with our custom one and capture the old one.
      lockedMeta.deleter = std::exchange(*deleter, metaDeleter);

      ssize_t n =
          symbolizer::getStackTrace(lockedMeta.trace.addresses, kMaxFrames);
      if (n != -1) {
        lockedMeta.trace.frameCount = n;
      }
      ssize_t nAsync = symbolizer::getAsyncStackTraceSafe(
          lockedMeta.traceAsync.addresses, kMaxFrames);
      if (nAsync != -1) {
        lockedMeta.traceAsync.frameCount = nAsync;
      }
    });

    auto oldMeta = getMetaMap().withWLock([ex, &newMeta](auto& wlock) {
      return std::exchange(wlock[ex], std::move(newMeta));
    });
    DCHECK(oldMeta == nullptr);

  } catch (const std::bad_alloc&) {
  }
}

struct Initialize {
  Initialize() { registerCxaThrowCallback(throwCallback); }
};

Initialize initialize;

// ExceptionMetaFunc takes a `const ExceptionMeta&` and
// returns a pair of iterators to represent a range of addresses for
// the stack frames to use.
template <typename ExceptionMetaFunc>
ExceptionInfo getTraceWithFunc(
    const std::exception& ex, ExceptionMetaFunc func) {
  ExceptionInfo info;
  info.type = &typeid(ex);
  auto rlockedMeta = getMetaMap().withRLock(
      [&](const auto& locked) noexcept
      -> SynchronizedExceptionMeta::RLockedPtr {
        auto* meta = get_ptr(locked, (void*)&ex);
        // If we can't find the exception, return an empty stack trace.
        if (!meta) {
          return {};
        }
        CHECK(*meta);
        // Acquire the meta rlock while holding the map's rlock, to block meta's
        // destruction.
        return (*meta)->rlock();
      });

  if (!rlockedMeta) {
    return info;
  }

  auto [traceBeginIt, traceEndIt] = func(*rlockedMeta);
  info.frames.assign(traceBeginIt, traceEndIt);
  return info;
}

template <typename ExceptionMetaFunc>
ExceptionInfo getTraceWithFunc(
    const std::exception_ptr& ptr, ExceptionMetaFunc func) {
  try {
    // To get a pointer to the actual exception we need to rethrow the
    // exception_ptr and catch it.
    std::rethrow_exception(ptr);
  } catch (std::exception& ex) {
    return getTraceWithFunc(ex, std::move(func));
  } catch (...) {
    return ExceptionInfo();
  }
}

template <typename ExceptionMetaFunc>
ExceptionInfo getTraceWithFunc(
    const exception_wrapper& ew, ExceptionMetaFunc func) {
  if (auto* ex = ew.get_exception()) {
    return getTraceWithFunc(*ex, std::move(func));
  }
  return ExceptionInfo();
}

auto getAsyncStackTraceItPair(const ExceptionMeta& meta) {
  return std::make_pair(
      meta.traceAsync.addresses,
      meta.traceAsync.addresses + meta.traceAsync.frameCount);
}

auto getNormalStackTraceItPair(const ExceptionMeta& meta) {
  return std::make_pair(
      meta.trace.addresses, meta.trace.addresses + meta.trace.frameCount);
}

} // namespace

ExceptionInfo getTrace(const std::exception_ptr& ptr) {
  return getTraceWithFunc(ptr, getNormalStackTraceItPair);
}

ExceptionInfo getTrace(const exception_wrapper& ew) {
  return getTraceWithFunc(ew, getNormalStackTraceItPair);
}

ExceptionInfo getTrace(const std::exception& ex) {
  return getTraceWithFunc(ex, getNormalStackTraceItPair);
}

ExceptionInfo getAsyncTrace(const std::exception_ptr& ptr) {
  return getTraceWithFunc(ptr, getAsyncStackTraceItPair);
}

ExceptionInfo getAsyncTrace(const exception_wrapper& ew) {
  return getTraceWithFunc(ew, getAsyncStackTraceItPair);
}

ExceptionInfo getAsyncTrace(const std::exception& ex) {
  return getTraceWithFunc(ex, getAsyncStackTraceItPair);
}

} // namespace exception_tracer
} // namespace folly

#endif // defined(__GLIBCXX__)

#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF
