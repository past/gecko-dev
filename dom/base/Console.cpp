/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/Console.h"
#include "mozilla/dom/ConsoleBinding.h"

#include "mozilla/dom/Exceptions.h"
#include "nsCycleCollectionParticipant.h"
#include "nsDocument.h"
#include "nsDOMNavigationTiming.h"
#include "nsGlobalWindow.h"
#include "nsJSUtils.h"
#include "nsPerformance.h"

#include "nsIConsoleAPIStorage.h"
#include "nsIDOMWindowUtils.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsILoadContext.h"
#include "nsIServiceManager.h"
#include "nsISupportsPrimitives.h"
#include "nsIWebNavigation.h"

// The maximum allowed number of concurrent timers per page.
#define MAX_PAGE_TIMERS 10000

// The maximum stacktrace depth when populating the stacktrace array used for
// console.trace().
#define DEFAULT_MAX_STACKTRACE_DEPTH 200

// The console API methods are async and their action is executed later. This
// delay tells how much later.
#define CALL_DELAY 15 // milliseconds

// This constant tells how many messages to process in a single timer execution.
#define MESSAGES_IN_INTERVAL 1500

using namespace mozilla::dom::exceptions;

namespace mozilla {
namespace dom {

class ConsoleCallData
{
public:
  ConsoleCallData()
    : mMethodName(Console::MethodLog)
    , mPrivate(false)
    , mTimeStamp(JS_Now())
    , mMonotonicTimer(0)
  {
  }

  void
  Initialize(JSContext* aCx, Console::MethodName aName,
             const nsAString& aString, const Sequence<JS::Value>& aArguments)
  {
    mGlobal = JS::CurrentGlobalOrNull(aCx);
    mMethodName = aName;
    mMethodString = aString;

    for (uint32_t i = 0; i < aArguments.Length(); ++i) {
      mArguments.AppendElement(aArguments[i]);
    }
  }

  JS::Heap<JSObject*> mGlobal;

  Console::MethodName mMethodName;
  bool mPrivate;
  int64_t mTimeStamp;
  DOMHighResTimeStamp mMonotonicTimer;

  nsString mMethodString;
  nsTArray<JS::Heap<JS::Value>> mArguments;
  Sequence<ConsoleStackEntry> mStack;
};

// This class is used to clear any exception at the end of this method.
class ClearException
{
public:
  ClearException(JSContext* aCx)
    : mCx(aCx)
  {
  }

  ~ClearException()
  {
    JS_ClearPendingException(mCx);
  }

private:
  JSContext* mCx;
};

NS_IMPL_CYCLE_COLLECTION_CLASS(Console)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(Console)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mWindow)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mTimer)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mStorage)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER

  tmp->mQueuedCalls.Clear();

NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(Console)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mWindow)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTimer)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mStorage)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(Console)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER

  for (uint32_t i = 0; i < tmp->mQueuedCalls.Length(); ++i) {
    NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mQueuedCalls[i].mGlobal);

    for (uint32_t j = 0; j < tmp->mQueuedCalls[i].mArguments.Length(); ++j) {
      NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mQueuedCalls[i].mArguments[j]);
    }
  }

NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(Console)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Console)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Console)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsITimerCallback)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

Console::Console(nsPIDOMWindow* aWindow)
  : mWindow(aWindow)
  , mOuterID(0)
  , mInnerID(0)
{
  if (mWindow) {
    MOZ_ASSERT(mWindow->IsInnerWindow());
    mInnerID = mWindow->WindowID();

    nsPIDOMWindow* outerWindow = mWindow->GetOuterWindow();
    MOZ_ASSERT(outerWindow);
    mOuterID = outerWindow->WindowID();
  }

  SetIsDOMBinding();
  mozilla::HoldJSObjects(this);
}

Console::~Console()
{
  mozilla::DropJSObjects(this);
}

JSObject*
Console::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return ConsoleBinding::Wrap(aCx, aScope, this);
}

#define METHOD(name, string)                                          \
  void                                                                \
  Console::name(JSContext* aCx, const Sequence<JS::Value>& aData)     \
  {                                                                   \
    Method(aCx, Method##name, NS_LITERAL_STRING(string), aData);      \
  }

METHOD(Log, "log")
METHOD(Info, "info")
METHOD(Warn, "warn")
METHOD(Error, "error")
METHOD(Exception, "exception")
METHOD(Debug, "debug")

void
Console::Trace(JSContext* aCx)
{
  const Sequence<JS::Value> data;
  Method(aCx, MethodTrace, NS_LITERAL_STRING("trace"), data);
}

// Displays an interactive listing of all the properties of an object.
METHOD(Dir, "dir");

METHOD(Group, "group")
METHOD(GroupCollapsed, "groupCollapsed")
METHOD(GroupEnd, "groupEnd")

void
Console::Time(JSContext* aCx, const JS::Handle<JS::Value> aTime)
{
  Sequence<JS::Value> data;
  SequenceRooter<JS::Value> rooter(aCx, &data);

  if (!aTime.isUndefined()) {
    data.AppendElement(aTime);
  }

  Method(aCx, MethodTime, NS_LITERAL_STRING("time"), data);
}

void
Console::TimeEnd(JSContext* aCx, const JS::Handle<JS::Value> aTime)
{
  Sequence<JS::Value> data;
  SequenceRooter<JS::Value> rooter(aCx, &data);

  if (!aTime.isUndefined()) {
    data.AppendElement(aTime);
  }

  Method(aCx, MethodTimeEnd, NS_LITERAL_STRING("timeEnd"), data);
}

void
Console::Profile(JSContext* aCx, const Sequence<JS::Value>& aData,
                 ErrorResult& aRv)
{
  ProfileMethod(aCx, NS_LITERAL_STRING("profile"), aData, aRv);
}

void
Console::ProfileEnd(JSContext* aCx, const Sequence<JS::Value>& aData,
                    ErrorResult& aRv)
{
  ProfileMethod(aCx, NS_LITERAL_STRING("profileEnd"), aData, aRv);
}

void
Console::ProfileMethod(JSContext* aCx, const nsAString& aAction,
                       const Sequence<JS::Value>& aData,
                       ErrorResult& aRv)
{
  RootedDictionary<ConsoleProfileEvent> event(aCx);
  event.mAction = aAction;

  event.mArguments.Construct();
  Sequence<JS::Value>& sequence = event.mArguments.Value();

  for (uint32_t i = 0; i < aData.Length(); ++i) {
    sequence.AppendElement(aData[i]);
  }

  JS::Rooted<JS::Value> eventValue(aCx);
  if (!event.ToObject(aCx, JS::NullPtr(), &eventValue)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  JS::Rooted<JSObject*> eventObj(aCx, &eventValue.toObject());
  MOZ_ASSERT(eventObj);

  if (!JS_DefineProperty(aCx, eventObj, "wrappedJSObject", eventValue,
                         nullptr, nullptr, JSPROP_ENUMERATE)) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  nsXPConnect*  xpc = nsXPConnect::XPConnect();
  nsCOMPtr<nsISupports> wrapper;
  const nsIID& iid = NS_GET_IID(nsISupports);

  if (NS_FAILED(xpc->WrapJS(aCx, eventObj, iid, getter_AddRefs(wrapper)))) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }

  nsCOMPtr<nsIObserverService> obs =
    do_GetService("@mozilla.org/observer-service;1");
  if (obs) {
    obs->NotifyObservers(wrapper, "console-api-profiler", nullptr);
  }
}

void
Console::Assert(JSContext* aCx, bool aCondition,
                const Sequence<JS::Value>& aData)
{
  if (!aCondition) {
    Method(aCx, MethodAssert, NS_LITERAL_STRING("assert"), aData);
  }
}

void
Console::__noSuchMethod__()
{
  // Nothing to do.
}

// Queue a call to a console method. See the CALL_DELAY constant.
void
Console::Method(JSContext* aCx, MethodName aMethodName,
                const nsAString& aMethodString,
                const Sequence<JS::Value>& aData)
{
  // This RAII class removes the last element of the mQueuedCalls if something
  // goes wrong.
  class RAII {
  public:
    RAII(nsTArray<ConsoleCallData>& aArray)
      : mArray(aArray)
      , mUnfinished(true)
    {
    }

    ~RAII()
    {
      if (mUnfinished) {
        mArray.RemoveElementAt(mArray.Length() - 1);
      }
    }

    void
    Finished()
    {
      mUnfinished = false;
    }

  private:
    nsTArray<ConsoleCallData>& mArray;
    bool mUnfinished;
  };

  ConsoleCallData& callData = *mQueuedCalls.AppendElement();
  callData.Initialize(aCx, aMethodName, aMethodString, aData);
  RAII raii(mQueuedCalls);

  if (mWindow) {
    nsCOMPtr<nsIWebNavigation> webNav = do_GetInterface(mWindow);
    if (!webNav) {
      Throw(aCx, NS_ERROR_FAILURE);
      return;
    }

    nsCOMPtr<nsILoadContext> loadContext = do_QueryInterface(webNav);
    MOZ_ASSERT(loadContext);

    loadContext->GetUsePrivateBrowsing(&callData.mPrivate);
  }

  uint32_t maxDepth = aMethodName == MethodTrace ?
                       DEFAULT_MAX_STACKTRACE_DEPTH : 1;
  nsCOMPtr<nsIStackFrame> stack = CreateStack(aCx, maxDepth);

  if (!stack) {
    Throw(aCx, NS_ERROR_FAILURE);
    return;
  }

  // nsIStackFrame is not thread-safe so we take what we need and we store in
  // an array of ConsoleStackEntry objects.
  do {
    uint32_t language;
    nsresult rv = stack->GetLanguage(&language);
    if (NS_FAILED(rv)) {
      Throw(aCx, rv);
      return;
    }

    if (language == nsIProgrammingLanguage::JAVASCRIPT ||
        language == nsIProgrammingLanguage::JAVASCRIPT2) {
      ConsoleStackEntry& data = *callData.mStack.AppendElement();

      nsCString string;
      rv = stack->GetFilename(string);
      if (NS_FAILED(rv)) {
        Throw(aCx, rv);
        return;
      }

      CopyUTF8toUTF16(string, data.mFilename);

      int32_t lineNumber;
      rv = stack->GetLineNumber(&lineNumber);
      if (NS_FAILED(rv)) {
        Throw(aCx, rv);
        return;
      }

      data.mLineNumber = lineNumber;

      rv = stack->GetName(string);
      if (NS_FAILED(rv)) {
        Throw(aCx, rv);
        return;
      }

      CopyUTF8toUTF16(string, data.mFunctionName);

      data.mLanguage = language;
    }

    nsCOMPtr<nsIStackFrame> caller;
    rv = stack->GetCaller(getter_AddRefs(caller));
    if (NS_FAILED(rv)) {
      Throw(aCx, rv);
      return;
    }

    stack.swap(caller);
  } while (stack);

  // Monotonic timer for 'time' and 'timeEnd'
  if ((aMethodName == MethodTime || aMethodName == MethodTimeEnd) && mWindow) {
    nsGlobalWindow *win = static_cast<nsGlobalWindow*>(mWindow.get());
    MOZ_ASSERT(win);

    ErrorResult rv;
    nsRefPtr<nsPerformance> performance = win->GetPerformance(rv);
    if (rv.Failed()) {
      Throw(aCx, rv.ErrorCode());
      return;
    }

    callData.mMonotonicTimer = performance->Now();
  }

  // The operation is completed. RAII class has to be disabled.
  raii.Finished();

  if (!mTimer) {
    mTimer = do_CreateInstance("@mozilla.org/timer;1");
    mTimer->InitWithCallback(this, CALL_DELAY,
                             nsITimer::TYPE_REPEATING_SLACK);
  }
}

// Timer callback used to process each of the queued calls.
NS_IMETHODIMP
Console::Notify(nsITimer *timer)
{
  MOZ_ASSERT(!mQueuedCalls.IsEmpty());

  uint32_t i = 0;
  for (; i < MESSAGES_IN_INTERVAL && i < mQueuedCalls.Length();
       ++i) {
    ProcessCallData(mQueuedCalls[i]);
  }

  mQueuedCalls.RemoveElementsAt(0, i);

  if (mQueuedCalls.IsEmpty()) {
    mTimer->Cancel();
    mTimer = nullptr;
  }

  return NS_OK;
}

void
Console::ProcessCallData(ConsoleCallData& aData)
{
  ConsoleStackEntry frame;
  if (!aData.mStack.IsEmpty()) {
    frame = aData.mStack[0];
  }

  AutoSafeJSContext cx;
  ClearException ce(cx);
  RootedDictionary<ConsoleEvent> event(cx);

  JSAutoCompartment ac(cx, aData.mGlobal);

  event.mID.Construct();
  event.mInnerID.Construct();
  if (mWindow) {
    event.mID.Value().SetAsUnsignedLong() = mOuterID;
    event.mInnerID.Value().SetAsUnsignedLong() = mInnerID;
  } else {
    // If we are in a JSM, the window doesn't exist.
    event.mID.Value().SetAsString() = NS_LITERAL_STRING("jsm");
    event.mInnerID.Value().SetAsString() = frame.mFilename;
  }

  event.mLevel = aData.mMethodString;
  event.mFilename = frame.mFilename;
  event.mLineNumber = frame.mLineNumber;
  event.mFunctionName = frame.mFunctionName;
  event.mTimeStamp = aData.mTimeStamp;
  event.mPrivate = aData.mPrivate;

  switch (aData.mMethodName) {
    case MethodLog:
    case MethodInfo:
    case MethodWarn:
    case MethodError:
    case MethodException:
    case MethodDebug:
    case MethodAssert:
      event.mArguments.Construct();
      ProcessArguments(cx, aData.mArguments, event.mArguments.Value());
      break;

    default:
      event.mArguments.Construct();
      ArgumentsToValueList(aData.mArguments, event.mArguments.Value());
  }

  if (aData.mMethodName == MethodTrace) {
    event.mStacktrace.Construct();
    event.mStacktrace.Value().SwapElements(aData.mStack);
  }

  else if (aData.mMethodName == MethodGroup ||
           aData.mMethodName == MethodGroupCollapsed ||
           aData.mMethodName == MethodGroupEnd) {
    ComposeGroupName(cx, aData.mArguments, event.mGroupName);
  }

  else if (aData.mMethodName == MethodTime && !aData.mArguments.IsEmpty()) {
    event.mTimer = StartTimer(cx, aData.mArguments[0], aData.mMonotonicTimer);
  }

  else if (aData.mMethodName == MethodTimeEnd && !aData.mArguments.IsEmpty()) {
    event.mTimer = StopTimer(cx, aData.mArguments[0], aData.mMonotonicTimer);
  }

  JS::Rooted<JS::Value> eventValue(cx);
  if (!event.ToObject(cx, JS::NullPtr(), &eventValue)) {
    Throw(cx, NS_ERROR_FAILURE);
    return;
  }

  JS::Rooted<JSObject*> eventObj(cx, &eventValue.toObject());
  MOZ_ASSERT(eventObj);

  if (!JS_DefineProperty(cx, eventObj, "wrappedJSObject", eventValue,
                         nullptr, nullptr, JSPROP_ENUMERATE)) {
    return;
  }

  if (!mStorage) {
    mStorage = do_GetService("@mozilla.org/consoleAPI-storage;1");
  }

  if (!mStorage) {
    NS_WARNING("Failed to get the ConsoleAPIStorage service.");
    return;
  }

  nsAutoString innerID;
  innerID.AppendInt(mInnerID);

  if (NS_FAILED(mStorage->RecordEvent(innerID, eventValue))) {
    NS_WARNING("Failed to record a console event.");
  }

  nsXPConnect*  xpc = nsXPConnect::XPConnect();
  nsCOMPtr<nsISupports> wrapper;
  const nsIID& iid = NS_GET_IID(nsISupports);

  if (NS_FAILED(xpc->WrapJS(cx, eventObj, iid, getter_AddRefs(wrapper)))) {
    return;
  }

  nsCOMPtr<nsIObserverService> obs =
    do_GetService("@mozilla.org/observer-service;1");
  if (obs) {
    nsAutoString outerID;
    outerID.AppendInt(mOuterID);

    obs->NotifyObservers(wrapper, "console-api-log-event", outerID.get());
  }
}

void
Console::ProcessArguments(JSContext* aCx,
                          const nsTArray<JS::Heap<JS::Value>>& aData,
                          Sequence<JS::Value>& aSequence)
{
  if (aData.IsEmpty()) {
    return;
  }

  if (aData.Length() == 1 || !aData[0].isString()) {
    ArgumentsToValueList(aData, aSequence);
    return;
  }

  SequenceRooter<JS::Value> rooter(aCx, &aSequence);

  JS::Rooted<JS::Value> format(aCx, aData[0]);
  JS::Rooted<JSString*> jsString(aCx, JS::ToString(aCx, format));
  if (!jsString) {
    return;
  }

  nsDependentJSString string;
  if (!string.init(aCx, jsString)) {
    return;
  }

  nsString::const_iterator start, end;
  string.BeginReading(start);
  string.EndReading(end);

  nsString output;
  uint32_t index = 1;

  while (start != end) {
    if (*start != '%') {
      output.Append(*start);
      ++start;
      continue;
    }

    ++start;

    if (*start == '%') {
      output.Append(*start);
      ++start;
      continue;
    }

    nsAutoString tmp;
    tmp.Append('%');

    int32_t integer = -1;
    int32_t mantissa = -1;

    // Let's parse %<number>.<number> for %d and %f
    if (*start >= '0' && *start <= '9') {
      integer = 0;

      do {
        integer = integer * 10 + *start - '0';
        tmp.Append(*start);
        ++start;
      } while (*start >= '0' && *start <= '9');
    }

    if (*start == '.') {
      tmp.Append(*start);
      ++start;

      // '.' must be followed by a number.
      if (*start < '0' || *start > '9') {
        output.Append(tmp);
        continue;
      }

      mantissa = 0;

      do {
        mantissa = mantissa * 10 + *start - '0';
        tmp.Append(*start);
        ++start;
      } while (*start >= '0' && *start <= '9');
    }

    char ch = *start;
    tmp.Append(ch);
    ++start;

    switch (ch) {
      case 'o':
      {
        if (!output.IsEmpty()) {
          JS::Rooted<JSString*> str(aCx, JS_NewUCStringCopyN(aCx,
                                                             output.get(),
                                                             output.Length()));
          if (!str) {
            return;
          }

          aSequence.AppendElement(JS::StringValue(str));
          output.Truncate();
        }

        JS::Rooted<JS::Value> v(aCx);
        if (index < aData.Length()) {
          v = aData[index++];
        }

        aSequence.AppendElement(v);
        break;
      }

      case 's':
        if (index < aData.Length()) {
          JS::Rooted<JS::Value> value(aCx, aData[index++]);
          JS::Rooted<JSString*> jsString(aCx, JS::ToString(aCx, value));
          if (!jsString) {
            return;
          }

          nsDependentJSString v;
          if (!v.init(aCx, jsString)) {
            return;
          }

          output.Append(v);
        }
        break;

      case 'd':
      case 'i':
        if (index < aData.Length()) {
          JS::Rooted<JS::Value> value(aCx, aData[index++]);

          int32_t v;
          if (!JS::ToInt32(aCx, value, &v)) {
            return;
          }

          nsCString format;
          MakeFormatString(format, integer, mantissa, 'd');
          output.AppendPrintf(format.get(), v);
        }
        break;

      case 'f':
        if (index < aData.Length()) {
          JS::Rooted<JS::Value> value(aCx, aData[index++]);

          double v;
          if (!JS::ToNumber(aCx, value, &v)) {
            return;
          }

          nsCString format;
          MakeFormatString(format, integer, mantissa, 'f');
          output.AppendPrintf(format.get(), v);
        }
        break;

      default:
        output.Append(tmp);
        break;
    }
  }

  if (!output.IsEmpty()) {
    JS::Rooted<JSString*> str(aCx, JS_NewUCStringCopyN(aCx, output.get(),
                                                       output.Length()));
    if (!str) {
      return;
    }

    aSequence.AppendElement(JS::StringValue(str));
  }

  // The rest of the array, if unused by the format string.
  for (; index < aData.Length(); ++index) {
    aSequence.AppendElement(aData[index]);
  }
}

void
Console::MakeFormatString(nsCString& aFormat, int32_t aInteger,
                          int32_t aMantissa, char aCh)
{
  aFormat.Append("%");
  if (aInteger >= 0) {
    aFormat.AppendInt(aInteger);
  }

  if (aMantissa >= 0) {
    aFormat.Append(".");
    aFormat.AppendInt(aMantissa);
  }

  aFormat.Append(aCh);
}

void
Console::ComposeGroupName(JSContext* aCx,
                          const nsTArray<JS::Heap<JS::Value>>& aData,
                          nsAString& aName)
{
  for (uint32_t i = 0; i < aData.Length(); ++i) {
    if (i != 0) {
      aName.AppendASCII(" ");
    }

    JS::Rooted<JS::Value> value(aCx, aData[i]);
    JS::Rooted<JSString*> jsString(aCx, JS::ToString(aCx, value));
    if (!jsString) {
      return;
    }

    nsDependentJSString string;
    if (!string.init(aCx, jsString)) {
      return;
    }

    aName.Append(string);
  }
}

JS::Value
Console::StartTimer(JSContext* aCx, const JS::Value& aName,
                    DOMHighResTimeStamp aTimestamp)
{
  if (mTimerRegistry.Count() >= MAX_PAGE_TIMERS) {
    RootedDictionary<ConsoleTimerError> error(aCx);

    JS::Rooted<JS::Value> value(aCx);
    if (!error.ToObject(aCx, JS::NullPtr(), &value)) {
      return JS::UndefinedValue();
    }

    return value;
  }

  RootedDictionary<ConsoleTimerStart> timer(aCx);

  JS::Rooted<JS::Value> name(aCx, aName);
  JS::Rooted<JSString*> jsString(aCx, JS::ToString(aCx, name));
  if (!jsString) {
    return JS::UndefinedValue();
  }

  nsDependentJSString key;
  if (!key.init(aCx, jsString)) {
    return JS::UndefinedValue();
  }

  timer.mName = key;

  DOMHighResTimeStamp entry;
  if (!mTimerRegistry.Get(key, &entry)) {
    mTimerRegistry.Put(key, aTimestamp);
  } else {
    aTimestamp = entry;
  }

  timer.mStarted = aTimestamp;

  JS::Rooted<JS::Value> value(aCx);
  if (!timer.ToObject(aCx, JS::NullPtr(), &value)) {
    return JS::UndefinedValue();
  }

  return value;
}

JS::Value
Console::StopTimer(JSContext* aCx, const JS::Value& aName,
                   DOMHighResTimeStamp aTimestamp)
{
  JS::Rooted<JS::Value> name(aCx, aName);
  JS::Rooted<JSString*> jsString(aCx, JS::ToString(aCx, name));
  if (!jsString) {
    return JS::UndefinedValue();
  }

  nsDependentJSString key;
  if (!key.init(aCx, jsString)) {
    return JS::UndefinedValue();
  }

  DOMHighResTimeStamp entry;
  if (!mTimerRegistry.Get(key, &entry)) {
    return JS::UndefinedValue();
  }

  mTimerRegistry.Remove(key);

  RootedDictionary<ConsoleTimerEnd> timer(aCx);
  timer.mName = key;
  timer.mDuration = aTimestamp - entry;

  JS::Rooted<JS::Value> value(aCx);
  if (!timer.ToObject(aCx, JS::NullPtr(), &value)) {
    return JS::UndefinedValue();
  }

  return value;
}

void
Console::ArgumentsToValueList(const nsTArray<JS::Heap<JS::Value>>& aData,
                              Sequence<JS::Value>& aSequence)
{
  for (uint32_t i = 0; i < aData.Length(); ++i) {
    aSequence.AppendElement(aData[i]);
  }
}

} // namespace dom
} // namespace mozilla
