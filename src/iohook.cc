#include <napi.h>
#include "uiohook.h"


#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define GetCurrentDir _getcwd

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) {
        FILETIME               filetime; /* 64-bit value representing the number of 100-nanosecond intervals since January 1, 1601 00:00 UTC */
        ULARGE_INTEGER         x;
        ULONGLONG              usec;
        static const ULONGLONG epoch_offset_us = 11644473600000000ULL; /* microseconds betweeen Jan 1,1601 and Jan 1,1970 */

#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
        GetSystemTimePreciseAsFileTime(&filetime);
#else
        GetSystemTimeAsFileTime(&filetime);
#endif
        x.LowPart =  filetime.dwLowDateTime;
        x.HighPart = filetime.dwHighDateTime;
        usec = x.QuadPart / 10  -  epoch_offset_us;
        tv->tv_sec  = (time_t)(usec / 1000000ULL);
        tv->tv_usec = (long)(usec % 1000000ULL);
    }
    if (tz) {
        TIME_ZONE_INFORMATION timezone;
        GetTimeZoneInformation(&timezone);
        tz->tz_minuteswest = timezone.Bias;
        tz->tz_dsttime = 0;
    }
    return 0;
}
#else

#if defined(__APPLE__) && defined(__MACH__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#define GetCurrentDir getcwd
#endif

#include <stdio.h>
#include <string.h>

#include <thread>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <chrono>

using namespace Napi;

static bool sIsRunning = false;
static bool sIsDebug = false;

static uiohook_event test_event;

std::mutex g_event_queue;
std::condition_variable cv_event_queue;
static std::queue<uiohook_event> event_queue;

std::thread event_thread;
std::thread hook_thread;

FILE * logFile = nullptr;

using Context = Reference<Napi::Value>;
using DataType = uiohook_event;
void CallJs(Env env, Function callback, Context * context, DataType * event);
using TSFN = TypedThreadSafeFunction<Context, DataType, CallJs>;

TSFN tsfnOnIOHookEvent;

// Native thread errors.
#define UIOHOOK_ERROR_THREAD_CREATE       0x10

long long current_time_milliseconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

bool logger_proc(unsigned int level, const char *format, ...) {
  // if (!sIsDebug) {
  //   return false;
  // }

  bool status = false;

  va_list args;

  long long milliseconds_since_epoch = current_time_milliseconds();

  fprintf(stdout, "[%lld] ", milliseconds_since_epoch);

  if (logFile != nullptr) {
    fprintf(logFile, "[%lld] ", milliseconds_since_epoch);
  }

  switch (level) {
    case LOG_LEVEL_DEBUG:
    case LOG_LEVEL_INFO:
      va_start(args, format);
      status = vfprintf(stdout, format, args) >= 0;
      if (logFile != nullptr) {
        vfprintf(logFile, format, args);
      }
      va_end(args);
      break;

    case LOG_LEVEL_WARN:
    case LOG_LEVEL_ERROR:
      va_start(args, format);
      status = vfprintf(stderr, format, args) >= 0;
      if (logFile != nullptr) {
        vfprintf(logFile, format, args);
      }
      va_end(args);
      break;
  }

  return status;
}

void handle_event(const uiohook_event * event, size_t size)
{
  logger_proc(LOG_LEVEL_DEBUG, "%s [%u]: queue event | type: %u | keycode: %#X\n",
    __FUNCTION__, __LINE__, event->type, event->data.keyboard.keycode);

  uiohook_event event_copy;
  memcpy(&event_copy, event, sizeof(uiohook_event));
  std::lock_guard<std::mutex> lock(g_event_queue);
  event_queue.push(event_copy);
  cv_event_queue.notify_one();
}

// NOTE: The following callback executes on the same thread that hook_run() is called
// from.  This is important because hook_run() attaches to the operating systems
// event dispatcher and may delay event delivery to the target application.
// Furthermore, some operating systems may choose to disable your hook if it
// takes to long to process.  If you need to do any extended processing, please
// do so by copying the event to your own queued dispatch thread.
void dispatch_proc(uiohook_event * const event) {
  logger_proc(LOG_LEVEL_DEBUG,  "%s [%u]: dispatch event | type: %u | keycode: %#X.\n",
    __FUNCTION__, __LINE__, event->type, event->data.keyboard.keycode);

  switch (event->type) {
    case EVENT_HOOK_ENABLED:
      break;

    case EVENT_HOOK_DISABLED:
      break;

    case EVENT_KEY_PRESSED:
    case EVENT_KEY_RELEASED:
    case EVENT_KEY_TYPED:
    case EVENT_MOUSE_PRESSED:
    case EVENT_MOUSE_RELEASED:
    case EVENT_MOUSE_CLICKED:
    case EVENT_MOUSE_MOVED:
    case EVENT_MOUSE_DRAGGED:
    case EVENT_MOUSE_WHEEL:
      handle_event(event, sizeof(uiohook_event));
      break;
  }
}

void hook_thread_proc() {
  logger_proc(LOG_LEVEL_WARN, "%s [%u]: running uiohook thread\n", __FUNCTION__, __LINE__);

  int status = hook_run();
  if (status != UIOHOOK_SUCCESS) {
     logger_proc(LOG_LEVEL_DEBUG,  "%s [%u]: failed to initialize uiohook: (%#X).\n",
        __FUNCTION__, __LINE__, status);
   }
}

void process_events_proc() {
  tsfnOnIOHookEvent.Acquire();

  uiohook_event ev;

  while (sIsRunning) {
    std::unique_lock<std::mutex> lock(g_event_queue);

    cv_event_queue.wait(lock, []{return !event_queue.empty();});

    ev = event_queue.front();

    logger_proc(LOG_LEVEL_WARN, "%s [%u]: received event from queue | type: %u | keycode:  %#X\n", 
      __FUNCTION__, __LINE__, ev.type, ev.data.keyboard.keycode);

    napi_status status = tsfnOnIOHookEvent.NonBlockingCall(&ev);

    if (status != napi_ok) {
      logger_proc(LOG_LEVEL_WARN, "%s [%u]: TSFN callback error: %u\n", __FUNCTION__, __LINE__, status);
    }

    event_queue.pop();
  }

  tsfnOnIOHookEvent.Release();
}

int hook_enable() {
  // Set the initial status.
  int status = UIOHOOK_FAILURE;

  hook_thread = std::thread(hook_thread_proc);
  event_thread = std::thread(process_events_proc);

  status = UIOHOOK_SUCCESS;

  logger_proc(LOG_LEVEL_DEBUG,  "%s [%u]: Thread Result: (%#X). EVENT_THREAD_ID = %u | HOOK_THREAD_ID = %u | MAIN THREAD = %u\n",
        __FUNCTION__, __LINE__, status, event_thread.get_id(), hook_thread.get_id(), std::this_thread::get_id());

  return status;
}

void stop() {
  int status = hook_stop();
  switch (status) {
    // System level errors.
    case UIOHOOK_ERROR_OUT_OF_MEMORY:
      logger_proc(LOG_LEVEL_ERROR, "Failed to allocate memory. (%#X)", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_GET_CONTEXT:
      // NOTE This is the only platform specific error that occurs on hook_stop().
      logger_proc(LOG_LEVEL_ERROR, "Failed to get XRecord context. (%#X)", status);
      break;

    // Default error.
    case UIOHOOK_FAILURE:
    default:
      logger_proc(LOG_LEVEL_ERROR, "An unknown hook error occurred. (%#X)", status);
      break;
  }
}

void CallJs(Env env, Function callback, Context * context, DataType * event) {
  logger_proc(LOG_LEVEL_WARN, "%s [%u]: JS Callback. type: %u | keycode: %#X.\n",
    __FUNCTION__, __LINE__, event->type, event->data.keyboard.keycode);

  Object obj = Object::New(env);

  obj.Set("type", Value::From(env, (uint16_t)event->type));
  obj.Set("mask", Value::From(env, (uint16_t)event->mask));
  obj.Set("time", Value::From(env, (uint16_t)event->time));

  if ((event->type >= EVENT_KEY_TYPED) && (event->type <= EVENT_KEY_RELEASED)) {
    Object keyboard = Object::New(env);

    keyboard.Set("shiftKey", event->data.keyboard.keycode == VC_SHIFT_L || event->data.keyboard.keycode == VC_SHIFT_R);
    keyboard.Set("altKey", event->data.keyboard.keycode == VC_ALT_L || event->data.keyboard.keycode == VC_ALT_R);
    keyboard.Set("ctrlKey", event->data.keyboard.keycode == VC_CONTROL_L || event->data.keyboard.keycode == VC_CONTROL_R);
    keyboard.Set("metaKey", event->data.keyboard.keycode == VC_META_L || event->data.keyboard.keycode == VC_META_R);


    if (event->type == EVENT_KEY_TYPED) {
      keyboard.Set("keychar", Value::From(env, (uint16_t)event->data.keyboard.keychar));
    }

    keyboard.Set("keycode", Value::From(env, (uint16_t)event->data.keyboard.keycode));
    keyboard.Set("rawcode", Value::From(env, (uint16_t)event->data.keyboard.rawcode));

    obj.Set("keyboard", keyboard);
  } else if ((event->type >= EVENT_MOUSE_CLICKED) && (event->type < EVENT_MOUSE_WHEEL)) {
    Object mouse = Object::New(env);
    mouse.Set("button", Value::From(env, (uint16_t)event->data.mouse.button));
    mouse.Set("clicks", Value::From(env, (uint16_t)event->data.mouse.clicks));
    mouse.Set("x", Value::From(env, (int16_t)event->data.mouse.x));
    mouse.Set("y", Value::From(env, (int16_t)event->data.mouse.y));

    obj.Set("mouse", mouse);
  } else if (event->type == EVENT_MOUSE_WHEEL) {
    Object wheel = Object::New(env);
    wheel.Set("amount", Value::From(env, (uint16_t)event->data.wheel.amount));
    wheel.Set("clicks", Value::From(env, (uint16_t)event->data.wheel.clicks));
    wheel.Set("direction", Value::From(env, (int16_t)event->data.wheel.direction));
    wheel.Set("rotation", Value::From(env, (int16_t)event->data.wheel.rotation));
    wheel.Set("type", Value::From(env, (int16_t)event->data.wheel.type));
    wheel.Set("x", Value::From(env, (int16_t)event->data.wheel.x));
    wheel.Set("y", Value::From(env, (int16_t)event->data.wheel.y));

    obj.Set("wheel", wheel);
  }

  callback.Call({obj});
}

void run() {
  // Set the logger callback for library output.
  hook_set_logger_proc(&logger_proc);

  // Set the event callback for uiohook events.
  hook_set_dispatch_proc(&dispatch_proc);

  // Start the hook and block.
  // NOTE If EVENT_HOOK_ENABLED was delivered, the status will always succeed.
  logger_proc(LOG_LEVEL_DEBUG, "%s [%u]: call hook_enable\n",__FUNCTION__, __LINE__);
  int status = hook_enable();
  logger_proc(LOG_LEVEL_DEBUG, "%s [%u]: hook_enable returned. %u\n",  __FUNCTION__, __LINE__, status);
  switch (status) {
    case UIOHOOK_SUCCESS:
      break;

    // System level errors.
    case UIOHOOK_ERROR_OUT_OF_MEMORY:
      logger_proc(LOG_LEVEL_ERROR, "Failed to allocate memory. (%#X)\n", status);
      break;


    // X11 specific errors.
    case UIOHOOK_ERROR_X_OPEN_DISPLAY:
      logger_proc(LOG_LEVEL_ERROR, "Failed to open X11 display. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_NOT_FOUND:
      logger_proc(LOG_LEVEL_ERROR, "Unable to locate XRecord extension. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_ALLOC_RANGE:
      logger_proc(LOG_LEVEL_ERROR, "Unable to allocate XRecord range. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_CREATE_CONTEXT:
      logger_proc(LOG_LEVEL_ERROR, "Unable to allocate XRecord context. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_X_RECORD_ENABLE_CONTEXT:
      logger_proc(LOG_LEVEL_ERROR, "Failed to enable XRecord context. (%#X)\n", status);
      break;


    // Windows specific errors.
    case UIOHOOK_ERROR_SET_WINDOWS_HOOK_EX:
      logger_proc(LOG_LEVEL_ERROR, "Failed to register low level windows hook. (%#X)\n", status);
      break;


    // Darwin specific errors.
    case UIOHOOK_ERROR_AXAPI_DISABLED:
      logger_proc(LOG_LEVEL_ERROR, "Failed to enable access for assistive devices. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_CREATE_EVENT_PORT:
      logger_proc(LOG_LEVEL_ERROR, "Failed to create apple event port. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_CREATE_RUN_LOOP_SOURCE:
      logger_proc(LOG_LEVEL_ERROR, "Failed to create apple run loop source. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_GET_RUNLOOP:
      logger_proc(LOG_LEVEL_ERROR, "Failed to acquire apple run loop. (%#X)\n", status);
      break;

    case UIOHOOK_ERROR_CREATE_OBSERVER:
      logger_proc(LOG_LEVEL_ERROR, "Failed to create apple run loop observer. (%#X)\n", status);
      break;

    // Default error.
    case UIOHOOK_FAILURE:
    default:
      logger_proc(LOG_LEVEL_ERROR, "An unknown hook error occurred. (%#X)\n", status);
      break;
  }
}

void Stop()
{
  stop();
  sIsRunning = false;

  if (logFile != nullptr) {
    fclose(logFile);
    logFile = nullptr;
  }
}

Boolean DebugEnable(const CallbackInfo& info) {
  if (info.Length() > 0)
  {
    sIsDebug = info[0].As<Boolean>();
  }

  return Boolean::New(info.Env(), true);
}

Boolean StartHook(const CallbackInfo& info) {
  logger_proc(LOG_LEVEL_WARN, "%s [%u]: START HOOK\n", __FUNCTION__, __LINE__);

  //allow one single execution
  if (sIsRunning == false)
  {
    if (logFile == nullptr) {
      char buff[FILENAME_MAX];
      GetCurrentDir(buff, FILENAME_MAX);

      char* search = "electron";

      #ifdef _WIN32
      if (strstr(buff, search) != NULL) {
        logFile = fopen("build_app\\iohook.log", "a");
      } else {
        logFile = fopen("electron\\build_app\\iohook.log", "a");
      }
      #else
      if (strstr(buff, search) != NULL) {
        logFile = fopen("build_app/iohook.log", "a");
      } else {
        logFile = fopen("electron/build_app/iohook.log", "a");
      }
      #endif
    }

    if (info.Length() > 0)
    {
      if (info.Length() == 2) {
        sIsDebug = info[1].As<Boolean>();
      }

      if (info[0].IsFunction())
      {
        tsfnOnIOHookEvent = TSFN::New(info.Env(), info[0].As<Function>(), "onKeyEvent Callback", 0, 1);
        sIsRunning = true;
        run();
      }
    }
  }

  return Boolean::New(info.Env(), true);
}

Boolean StopHook(const CallbackInfo& info) {
  logger_proc(LOG_LEVEL_WARN, "%s [%u]: STOP HOOK\n", __FUNCTION__, __LINE__);

  //allow one single execution
  if ((sIsRunning == true))
  {
    Stop();
  }

  return Boolean::New(info.Env(), true);
}


Object Init(Env env, Object exports) {
    exports.Set(String::New(env, "startHook"), Function::New(env, StartHook));
    exports.Set(String::New(env, "stopHook"), Function::New(env, StopHook));
    exports.Set(String::New(env, "debugEnable"), Function::New(env, DebugEnable));

    return exports;
}


NODE_API_MODULE(addon, Init)
