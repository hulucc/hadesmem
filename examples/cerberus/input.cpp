// Copyright (C) 2010-2015 Joshua Boyce
// See the file COPYING for copying permission.

#include "input.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <queue>

#include <windows.h>
#include <winnt.h>
#include <winternl.h>

#include <hadesmem/config.hpp>
#include <hadesmem/detail/smart_handle.hpp>
#include <hadesmem/detail/str_conv.hpp>
#include <hadesmem/detail/winternl.hpp>

#include "callbacks.hpp"
#include "cursor.hpp"
#include "direct_input.hpp"
#include "hook_disabler.hpp"
#include "main.hpp"
#include "raw_input.hpp"
#include "render.hpp"
#include "window.hpp"

namespace
{
hadesmem::cerberus::Callbacks<hadesmem::cerberus::OnInputQueueEntry>&
  GetOnInputQueueEntryCallbacks()
{
  static hadesmem::cerberus::Callbacks<hadesmem::cerberus::OnInputQueueEntry>
    callbacks;
  return callbacks;
}

class InputImpl : public hadesmem::cerberus::InputInterface
{
public:
  virtual std::size_t RegisterOnInputQueueEntry(
    std::function<hadesmem::cerberus::OnInputQueueEntry> const& callback) final
  {
    auto& callbacks = GetOnInputQueueEntryCallbacks();
    return callbacks.Register(callback);
  }

  virtual void UnregisterOnInputQueueEntry(std::size_t id) final
  {
    auto& callbacks = GetOnInputQueueEntryCallbacks();
    return callbacks.Unregister(id);
  }
};

int& GetShowCursorCount() HADESMEM_DETAIL_NOEXCEPT
{
  static int show_cursor_count{};
  return show_cursor_count;
}

std::pair<bool, HCURSOR>& GetOldCursor() HADESMEM_DETAIL_NOEXCEPT
{
  static std::pair<bool, HCURSOR> old_cursor{};
  return old_cursor;
}

std::pair<bool, POINT>& GetOldCursorPos() HADESMEM_DETAIL_NOEXCEPT
{
  static std::pair<bool, POINT> cursor_pos{};
  return cursor_pos;
}

RECT& GetOldClipCursor() HADESMEM_DETAIL_NOEXCEPT
{
  static RECT old_clip_cursor{};
  return old_clip_cursor;
}

std::vector<RAWINPUTDEVICE>& GetOldRawInputDevices() HADESMEM_DETAIL_NOEXCEPT
{
  static std::vector<RAWINPUTDEVICE> old_devices;
  return old_devices;
}

struct WndProcInputMsg
{
  WndProcInputMsg(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, DWORD tid)
    : hwnd_{hwnd}, msg_{msg}, wparam_{wparam}, lparam_{lparam}, tid_{tid}
  {
    thread_ = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread_.IsValid())
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"OpenThread failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }
  }

  WndProcInputMsg(WndProcInputMsg const&) = delete;
  WndProcInputMsg& operator=(WndProcInputMsg const&) = delete;

  WndProcInputMsg(WndProcInputMsg&& other)
    : hwnd_{other.hwnd_},
      msg_{other.msg_},
      wparam_{other.wparam_},
      lparam_{other.lparam_},
      tid_{other.tid_},
      thread_{std::move(other.thread_)}
  {
  }

  WndProcInputMsg& operator=(WndProcInputMsg&& other)
  {
    hwnd_ = other.hwnd_;
    msg_ = other.msg_;
    wparam_ = other.wparam_;
    lparam_ = other.lparam_;
    tid_ = other.tid_;
    thread_ = std::move(other.thread_);
  }

  HWND hwnd_;
  UINT msg_;
  WPARAM wparam_;
  LPARAM lparam_;
  DWORD tid_;
  hadesmem::detail::SmartHandle thread_;
};

std::queue<WndProcInputMsg>& GetWndProcInputMsgQueue()
{
  static std::queue<WndProcInputMsg> queue;
  return queue;
}

std::recursive_mutex& GetWndProcInputMsgQueueMutex()
{
  static std::recursive_mutex mutex;
  return mutex;
}

void SetOrRestoreCursor(bool visible)
{
  hadesmem::cerberus::HookDisabler disable_set_cursor_hook{
    &hadesmem::cerberus::GetDisableSetCursorHook()};

  auto& old_cursor = GetOldCursor();

  if (visible)
  {
    HCURSOR const arrow_cursor = ::LoadCursorW(nullptr, IDC_ARROW);
    if (!arrow_cursor)
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"LoadCursorW failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }

    HADESMEM_DETAIL_TRACE_A("Setting arrow cursor.");
    old_cursor.second = ::SetCursor(arrow_cursor);
  }
  else if (old_cursor.first)
  {
    HADESMEM_DETAIL_TRACE_A("Setting old cursor.");
    old_cursor.second = ::SetCursor(old_cursor.second);
  }

  old_cursor.first = true;
}

void SaveCurrentCursorPos()
{
  auto& old_cursor_pos = GetOldCursorPos();

  hadesmem::cerberus::HookDisabler disable_get_cursor_pos_hook{
    &hadesmem::cerberus::GetDisableGetCursorPosHook()};

  POINT cur_cursor_pos{};
  if (!::GetCursorPos(&cur_cursor_pos))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"GetCursorPos failed."}
                        << hadesmem::ErrorCodeWinLast{last_error});
  }

  old_cursor_pos.first = true;
  old_cursor_pos.second = cur_cursor_pos;
}

void ClearOldCursorPos()
{
  auto& old_cursor_pos = GetOldCursorPos();
  old_cursor_pos.first = false;
  old_cursor_pos.second = POINT{};
}

void RestoreOldCursorPos()
{
  auto& old_cursor_pos = GetOldCursorPos();

  if (!old_cursor_pos.first)
  {
    return;
  }

  hadesmem::cerberus::HookDisabler disable_set_cursor_pos_hook{
    &hadesmem::cerberus::GetDisableSetCursorPosHook()};

  if (!::SetCursorPos(old_cursor_pos.second.x, old_cursor_pos.second.y))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"SetCursorPos failed."}
                        << hadesmem::ErrorCodeWinLast{last_error});
  }

  ClearOldCursorPos();
}

void ShowCursor()
{
  hadesmem::cerberus::HookDisabler disable_show_cursor_hook{
    &hadesmem::cerberus::GetDisableShowCursorHook()};

  auto& show_cursor_count = GetShowCursorCount();
  do
  {
    HADESMEM_DETAIL_TRACE_A("Showing cursor.");
    ++show_cursor_count;
  } while (::ShowCursor(TRUE) < 0);
}

void HideCursor()
{
  hadesmem::cerberus::HookDisabler disable_show_cursor_hook{
    &hadesmem::cerberus::GetDisableShowCursorHook()};

  auto& show_cursor_count = GetShowCursorCount();
  while (show_cursor_count > 0)
  {
    HADESMEM_DETAIL_TRACE_A("Hiding cursor.");
    --show_cursor_count;
    ::ShowCursor(FALSE);
  }
}

void SaveCurrentClipCursor()
{
  hadesmem::cerberus::HookDisabler disable_get_clip_cursor_hook{
    &hadesmem::cerberus::GetDisableGetClipCursorHook()};

  RECT clip_cursor{};
  if (!::GetClipCursor(&clip_cursor))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"GetClipCursor failed."}
                        << hadesmem::ErrorCodeWinLast{last_error});
  }

  HADESMEM_DETAIL_TRACE_FORMAT_A("Saving current clip cursor: Left [%ld] Top "
                                 "[%ld] Right [%ld] Bottom [%ld]",
                                 clip_cursor.left,
                                 clip_cursor.top,
                                 clip_cursor.right,
                                 clip_cursor.bottom);

  auto& old_clip_cursor = GetOldClipCursor();
  old_clip_cursor = clip_cursor;
}

void ClipCursorWrap(RECT clip_cursor)
{
  hadesmem::cerberus::HookDisabler disable_clip_cursor_hook{
    &hadesmem::cerberus::GetDisableClipCursorHook()};

  if (!::ClipCursor(&clip_cursor))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"ClipCursor failed."}
                        << hadesmem::ErrorCodeWinLast{last_error});
  }
}

void SetNewClipCursor()
{
  auto& window_interface = hadesmem::cerberus::GetWindowInterface();
  auto const wnd = window_interface.GetCurrentWindow();
  if (!::IsWindow(wnd))
  {
    HADESMEM_DETAIL_TRACE_A("WARNING! Invalid window.");
    return;
  }

  RECT new_clip_cursor{};
  if (!::GetWindowRect(wnd, &new_clip_cursor))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{} << hadesmem::ErrorString{"GetWindowRect failed."}
                        << hadesmem::ErrorCodeWinLast{last_error});
  }

  auto const& old_clip_cursor = GetOldClipCursor();
  new_clip_cursor.left = (std::min)(old_clip_cursor.left, new_clip_cursor.left);
  new_clip_cursor.top = (std::min)(old_clip_cursor.top, new_clip_cursor.top);
  new_clip_cursor.right =
    (std::max)(old_clip_cursor.right, new_clip_cursor.right);
  new_clip_cursor.bottom =
    (std::max)(old_clip_cursor.bottom, new_clip_cursor.bottom);

  HADESMEM_DETAIL_TRACE_FORMAT_A(
    "Setting new clip cursor: Left [%ld] Top [%ld] Right [%ld] Bottom [%ld]",
    new_clip_cursor.left,
    new_clip_cursor.top,
    new_clip_cursor.right,
    new_clip_cursor.bottom);

  ClipCursorWrap(new_clip_cursor);
}

void RestoreOldClipCursor()
{
  auto& clip_cursor = GetOldClipCursor();

  HADESMEM_DETAIL_TRACE_FORMAT_A("Restoring old clip cursor: Left [%ld] Top "
                                 "[%ld] Right [%ld] Bottom [%ld]",
                                 clip_cursor.left,
                                 clip_cursor.top,
                                 clip_cursor.right,
                                 clip_cursor.bottom);

  ClipCursorWrap(clip_cursor);
}

void ToggleGuiVisible()
{
  auto const visible = !hadesmem::cerberus::GetGuiVisible();
  HADESMEM_DETAIL_TRACE_A(visible ? "Showing GUI." : "Hiding GUI.");
  hadesmem::cerberus::SetGuiVisible(visible, !visible);
}

void WindowProcCallback(
  HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool* handled)
{
  {
    auto& queue = GetWndProcInputMsgQueue();
    auto& mutex = GetWndProcInputMsgQueueMutex();
    std::lock_guard<std::recursive_mutex> lock{mutex};
    queue.push(
      WndProcInputMsg{hwnd, msg, wparam, lparam, ::GetCurrentThreadId()});
  }

  if (msg == WM_KEYDOWN && !((lparam >> 30) & 1) && wparam == VK_F9 &&
      ::GetAsyncKeyState(VK_SHIFT) & 0x8000)
  {
    ToggleGuiVisible();
    *handled = true;
    return;
  }

  auto const visible = hadesmem::cerberus::GetGuiVisible();
  bool const blocked_msg = msg == WM_INPUT ||
                           (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                           (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
  if (visible && blocked_msg)
  {
    *handled = true;
    return;
  }
}

void OnSetCursor(HCURSOR cursor,
                 bool* handled,
                 HCURSOR* retval) HADESMEM_DETAIL_NOEXCEPT
{
  auto& old_cursor = GetOldCursor();
  HCURSOR const old_cursor_raw = old_cursor.second;
  old_cursor.first = true;
  old_cursor.second = cursor;

  if (hadesmem::cerberus::GetGuiVisible())
  {
    *retval = old_cursor_raw;
    *handled = true;
  }
}

void OnDirectInput(bool* handled) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible())
  {
    *handled = true;
  }
}

void OnGetCursorPos(LPPOINT point, bool* handled) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible() && point)
  {
    auto& old_cursor_pos = GetOldCursorPos();
    point->x = old_cursor_pos.second.x;
    point->y = old_cursor_pos.second.y;

    *handled = true;
  }
}

void OnSetCursorPos(int x, int y, bool* handled) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible())
  {
    auto& old_cursor_pos = GetOldCursorPos();
    old_cursor_pos.first = true;
    old_cursor_pos.second.x = x;
    old_cursor_pos.second.y = y;

    *handled = true;
  }
}

void OnShowCursor(BOOL show,
                  bool* handled,
                  int* retval) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible())
  {
    auto& show_cursor_count = GetShowCursorCount();
    if (show)
    {
      ++show_cursor_count;
    }
    else
    {
      --show_cursor_count;
    }

    *retval = show_cursor_count;
    *handled = true;
  }
}

void OnClipCursor(RECT const* rect,
                  bool* handled,
                  BOOL* retval) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible() && rect)
  {
    auto& clip_cursor = GetOldClipCursor();
    clip_cursor = *rect;
    *retval = TRUE;
    *handled = true;
  }
}

void OnGetClipCursor(RECT* rect,
                     bool* handled,
                     BOOL* retval) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible() && rect)
  {
    auto& clip_cursor = GetOldClipCursor();
    *rect = clip_cursor;
    *retval = TRUE;
    *handled = true;
  }
}

void OnGetRawInputBuffer(PRAWINPUT /*data*/,
                         PUINT /*size*/,
                         UINT /*size_header*/,
                         bool* handled,
                         UINT* retval) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible())
  {
    // If you see this being spammed, please let me know along with the name of
    // the game you've observed it in.
    HADESMEM_DETAIL_TRACE_FORMAT_A("Fix me.");
    *retval = static_cast<UINT>(-1);
    *handled = true;
  }
}

void OnGetRawInputData(HRAWINPUT /*raw_input*/,
                       UINT /*command*/,
                       LPVOID data,
                       PUINT size,
                       UINT /*size_header*/,
                       bool* handled,
                       UINT* retval) HADESMEM_DETAIL_NOEXCEPT
{
  if (hadesmem::cerberus::GetGuiVisible() && data && size)
  {
    ::ZeroMemory(data, *size);
    *retval = static_cast<UINT>(-1);
    *handled = true;
  }
}

void OnRegisterRawInputDevices(PCRAWINPUTDEVICE raw_input_devices,
                               UINT num_devices,
                               UINT /*size*/,
                               bool* handled,
                               BOOL* retval)
{
  auto r = const_cast<RAWINPUTDEVICE*>(raw_input_devices);
  if (!r)
  {
    return;
  }

  for (UINT i = 0; i < num_devices; ++i)
  {
    HADESMEM_DETAIL_TRACE_FORMAT_A("Device: [%u]. UsagePage: [%u]. Usage: "
                                   "[%u]. Flags: [%08X]. Target: [%p].",
                                   i,
                                   r[i].usUsagePage,
                                   r[i].usUsage,
                                   r[i].dwFlags,
                                   r[i].hwndTarget);

    if ((r[i].dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY)
    {
      HADESMEM_DETAIL_TRACE_FORMAT_A(
        "Raw input device %u registered with RIDEV_NOLEGACY.", i);
      // r[i].dwFlags &= ~(RIDEV_NOLEGACY | RIDEV_APPKEYS);
    }

    if ((r[i].dwFlags & RIDEV_REMOVE) == RIDEV_REMOVE)
    {
      HADESMEM_DETAIL_TRACE_FORMAT_A("Raw input device %u removed.", i);
    }
  }

  if (hadesmem::cerberus::GetGuiVisible())
  {
    *handled = true;
    *retval = FALSE;
    return;
  }
}

void LazyAttachThreadInput(DWORD tid)
{
  static __declspec(thread) DWORD last_attached_tid = 0;
  static __declspec(thread) HANDLE last_attached_thread = nullptr;

  DWORD const current_tid = ::GetCurrentThreadId();
  if (current_tid != tid && last_attached_tid != tid)
  {
    if (last_attached_tid)
    {
      if (!::AttachThreadInput(current_tid, last_attached_tid, FALSE))
      {
        DWORD const last_error = ::GetLastError();
        HADESMEM_DETAIL_THROW_EXCEPTION(
          hadesmem::Error{}
          << hadesmem::ErrorString{"AttachThreadInput failed."}
          << hadesmem::ErrorCodeWinLast{last_error});
      }
      last_attached_tid = 0;

      if (last_attached_thread)
      {
        ::CloseHandle(last_attached_thread);
        last_attached_thread = nullptr;
      }
    }

    HADESMEM_DETAIL_TRACE_FORMAT_A("Attaching thread input. TID: [%u].", tid);

    if (!::AttachThreadInput(current_tid, tid, TRUE))
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"AttachThreadInput failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }
    last_attached_tid = tid;

    HANDLE const thread =
      ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread)
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        hadesmem::Error{} << hadesmem::ErrorString{"OpenThread failed."}
                          << hadesmem::ErrorCodeWinLast{last_error});
    }
    last_attached_thread = thread;
  }
}

void RegisterRawInputDevicesWrapper(PCRAWINPUTDEVICE raw_input_devices,
                                    UINT num_devices,
                                    UINT size)
{
  if (!::RegisterRawInputDevices(raw_input_devices, num_devices, size))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
      << hadesmem::ErrorString{"RegisterRawInputDevices failed."}
      << hadesmem::ErrorCodeWinLast{last_error});
  }
}

void LogRawInputDevice(RAWINPUTDEVICE const& device)
{
  (void)device;
  HADESMEM_DETAIL_TRACE_FORMAT_A(
    "UsagePage: [%u]. Usage: [%u]. Flags: [%08X]. Target: [%p].",
    device.usUsagePage,
    device.usUsage,
    device.dwFlags,
    device.hwndTarget);
}

void DoesRawInputDeviceHaveMouseOrKeyboardDevice(
  std::vector<RAWINPUTDEVICE> const& devices,
  bool& has_mouse_device,
  bool& has_keyboard_device)
{
  auto const mouse_iter = std::find_if(
    std::begin(devices),
    std::end(devices),
    [](RAWINPUTDEVICE const& device)
    {
      return device.usUsagePage == HADESMEM_DETAIL_HID_USAGE_PAGE_GENERIC &&
             device.usUsage == HADESMEM_DETAIL_HID_USAGE_GENERIC_MOUSE;
    });
  has_mouse_device = mouse_iter != std::end(devices);

  auto const keyboard_iter = std::find_if(
    std::begin(devices),
    std::end(devices),
    [](RAWINPUTDEVICE const& device)
    {
      return device.usUsagePage == HADESMEM_DETAIL_HID_USAGE_PAGE_GENERIC &&
             device.usUsage == HADESMEM_DETAIL_HID_USAGE_GENERIC_KEYBOARD;
    });
  has_keyboard_device = keyboard_iter != std::end(devices);
}

void SetRawInputDevices()
{
  hadesmem::cerberus::HookDisabler disable_register_raw_input_devices_hook{
    &hadesmem::cerberus::GetDisableRegisterRawInputDevicesHook()};

  HADESMEM_DETAIL_TRACE_FORMAT_A("Setting new raw input devices.");

  UINT num_devices = 0;
  ::GetRegisteredRawInputDevices(nullptr, &num_devices, sizeof(RAWINPUTDEVICE));

  if (!num_devices)
  {
    HADESMEM_DETAIL_TRACE_A("No registered raw input devices.");
    return;
  }

  std::vector<RAWINPUTDEVICE> old_devices(num_devices);
  UINT const num_devices_written = ::GetRegisteredRawInputDevices(
    old_devices.data(), &num_devices, sizeof(RAWINPUTDEVICE));
  if (num_devices_written == static_cast<UINT>(-1))
  {
    DWORD const last_error = ::GetLastError();
    HADESMEM_DETAIL_THROW_EXCEPTION(
      hadesmem::Error{}
      << hadesmem::ErrorString{"GetRegisteredRawInputDevices failed."}
      << hadesmem::ErrorCodeWinLast{last_error});
  }

  old_devices.resize(num_devices_written);

  GetOldRawInputDevices() = old_devices;

  bool has_mouse_device{};
  bool has_keyboard_device{};
  DoesRawInputDeviceHaveMouseOrKeyboardDevice(
    old_devices, has_mouse_device, has_keyboard_device);

  if (!has_mouse_device && !has_keyboard_device)
  {
    HADESMEM_DETAIL_TRACE_A(
      "No registered mouse or keyboard raw input devices.");
    return;
  }

  auto const& window_interface = hadesmem::cerberus::GetWindowInterface();

  if (has_mouse_device)
  {
    HADESMEM_DETAIL_TRACE_A("Setting new mouse device.");

    RAWINPUTDEVICE new_device = {HADESMEM_DETAIL_HID_USAGE_PAGE_GENERIC,
                                 HADESMEM_DETAIL_HID_USAGE_GENERIC_MOUSE,
                                 0,
                                 window_interface.GetCurrentWindow()};
    RegisterRawInputDevicesWrapper(&new_device, 1, sizeof(RAWINPUTDEVICE));
  }

  if (has_keyboard_device)
  {
    HADESMEM_DETAIL_TRACE_A("Setting new keyboard device.");

    RAWINPUTDEVICE new_device = {HADESMEM_DETAIL_HID_USAGE_PAGE_GENERIC,
                                 HADESMEM_DETAIL_HID_USAGE_GENERIC_KEYBOARD,
                                 0,
                                 window_interface.GetCurrentWindow()};
    RegisterRawInputDevicesWrapper(&new_device, 1, sizeof(RAWINPUTDEVICE));
  }
}

void RestoreRawInputDevices()
{
  hadesmem::cerberus::HookDisabler disable_register_raw_input_devices_hook{
    &hadesmem::cerberus::GetDisableRegisterRawInputDevicesHook()};

  HADESMEM_DETAIL_TRACE_A("Restoring old raw input devices.");

  auto const& old_devices = GetOldRawInputDevices();
  for (auto const& device : old_devices)
  {
    LogRawInputDevice(device);

    if (device.usUsagePage == HADESMEM_DETAIL_HID_USAGE_PAGE_GENERIC &&
        device.usUsage == HADESMEM_DETAIL_HID_USAGE_GENERIC_MOUSE)
    {
      HADESMEM_DETAIL_TRACE_A("Restoring old mouse device.");
      RegisterRawInputDevicesWrapper(&device, 1, sizeof(device));
    }
    else if (device.usUsagePage == HADESMEM_DETAIL_HID_USAGE_PAGE_GENERIC &&
             device.usUsage == HADESMEM_DETAIL_HID_USAGE_GENERIC_KEYBOARD)
    {
      HADESMEM_DETAIL_TRACE_A("Restoring old keyboard device.");
      RegisterRawInputDevicesWrapper(&device, 1, sizeof(device));
    }
    else
    {
      HADESMEM_DETAIL_TRACE_A("Skipping unknown device.");
    }
  }
}
}

namespace hadesmem
{
namespace cerberus
{
void SetGuiVisibleForInput(bool visible, bool old_visible)
{
  if (visible != old_visible)
  {
    SetOrRestoreCursor(visible);

    if (visible)
    {
      SaveCurrentCursorPos();

      ShowCursor();

      SaveCurrentClipCursor();

      SetNewClipCursor();

      SetRawInputDevices();
    }
    else
    {
      RestoreOldCursorPos();

      HideCursor();

      RestoreOldClipCursor();

      RestoreRawInputDevices();
    }
  }
  else
  {
    ClearOldCursorPos();

    if (visible)
    {
      SetNewClipCursor();
    }
  }
}

void HandleInputQueue()
{
  auto& queue = GetWndProcInputMsgQueue();
  auto& mutex = GetWndProcInputMsgQueueMutex();
  std::lock_guard<std::recursive_mutex> lock{mutex};
  while (!queue.empty())
  {
    WndProcInputMsg& msg = queue.front();

    hadesmem::cerberus::HookDisabler disable_set_cursor_hook{
      &hadesmem::cerberus::GetDisableSetCursorHook()};

    hadesmem::cerberus::HookDisabler disable_get_cursor_pos_hook{
      &hadesmem::cerberus::GetDisableGetCursorPosHook()};

    LazyAttachThreadInput(msg.tid_);

    auto& callbacks = GetOnInputQueueEntryCallbacks();
    callbacks.Run(msg.hwnd_, msg.msg_, msg.wparam_, msg.lparam_);
    queue.pop();
  }
}

InputInterface& GetInputInterface() HADESMEM_DETAIL_NOEXCEPT
{
  static InputImpl input_impl;
  return input_impl;
}

void InitializeInput()
{
  auto& window = GetWindowInterface();
  window.RegisterOnWndProcMsg(WindowProcCallback);

  auto& cursor = GetCursorInterface();
  cursor.RegisterOnSetCursor(OnSetCursor);
  cursor.RegisterOnGetCursorPos(OnGetCursorPos);
  cursor.RegisterOnSetCursorPos(OnSetCursorPos);
  cursor.RegisterOnShowCursor(OnShowCursor);
  cursor.RegisterOnClipCursor(OnClipCursor);
  cursor.RegisterOnGetClipCursor(OnGetClipCursor);

  auto& direct_input = GetDirectInputInterface();
  direct_input.RegisterOnDirectInput(OnDirectInput);

  auto& raw_input = GetRawInputInterface();
  raw_input.RegisterOnGetRawInputBuffer(OnGetRawInputBuffer);
  raw_input.RegisterOnGetRawInputData(OnGetRawInputData);
  raw_input.RegisterOnRegisterRawInputDevices(OnRegisterRawInputDevices);
}
}
}
