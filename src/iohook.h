#pragma once

#include <napi.h>
#include <queue>
#include "uiohook.h"

class IOHookHandler
{
  private:
    Napi::ThreadSafeFunction _onKeyEvent;
    static Napi::Object FillEventObject(Napi::Env env, uiohook_event * event);

  public:
    IOHookHandler(const Napi::CallbackInfo& info);
    static void KeyEventCallback(Napi::Env env, Napi::Function jsCallback, uiohook_event * event);
    void HandleEvent(const uiohook_event * event, size_t size);
};