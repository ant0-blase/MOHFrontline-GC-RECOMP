// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <windows.h>
#include <windowsx.h>
#include <hidusage.h>
#include <climits>
#include <dwmapi.h>
#include <thread>
#include <vector>

#include "VideoCommon/Present.h"
#include "VideoCommon/MohPcLayer.h"
#include "resource.h"

namespace
{
class PlatformWin32 final : public Platform
{
public:
  ~PlatformWin32() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  static constexpr TCHAR WINDOW_CLASS_NAME[] = _T("DolphinNoGUI");

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  static bool RegisterRenderWindowClass();
  bool CreateRenderWindow();
  void UpdateWindowPosition();
  void UpdateMouseCapture();
  void ProcessEvents();
  static u32 TranslateKey(WPARAM key);

  HWND m_hwnd{};
  bool m_mouse_captured = false;

  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformWin32::~PlatformWin32()
{
  if (m_hwnd)
    DestroyWindow(m_hwnd);
}

bool PlatformWin32::RegisterRenderWindowClass()
{
  WNDCLASSEX wc = {};
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = WndProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hIcon = LoadIcon(nullptr, IDI_ICON1);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = WINDOW_CLASS_NAME;
  wc.hIconSm = LoadIcon(nullptr, IDI_ICON1);

  if (!RegisterClassEx(&wc))
  {
    MessageBox(nullptr, _T("Window registration failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  return true;
}

bool PlatformWin32::CreateRenderWindow()
{
  m_hwnd = CreateWindowEx(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, _T("Dolphin"), WS_OVERLAPPEDWINDOW,
                          m_window_x < 0 ? CW_USEDEFAULT : m_window_x,
                          m_window_y < 0 ? CW_USEDEFAULT : m_window_y, m_window_width,
                          m_window_height, nullptr, nullptr, GetModuleHandle(nullptr), this);
  if (!m_hwnd)
  {
    MessageBox(nullptr, _T("CreateWindowEx failed."), _T("Error"), MB_ICONERROR | MB_OK);
    return false;
  }

  ShowWindow(m_hwnd, SW_SHOW);
  UpdateWindow(m_hwnd);
  return true;
}

bool PlatformWin32::Init()
{
  if (!RegisterRenderWindowClass() || !CreateRenderWindow())
    return false;

  MohPcLayer::SetPlatformName("Windows/RawInput");
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
  rid.usUsage = HID_USAGE_GENERIC_MOUSE;
  rid.dwFlags = 0;
  rid.hwndTarget = m_hwnd;
  RegisterRawInputDevices(&rid, 1, sizeof(rid));

  // TODO: Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    ProcessEvents();
  }

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);

  UpdateWindowPosition();
  return true;
}

void PlatformWin32::SetTitle(const std::string& string)
{
  SetWindowTextW(m_hwnd, UTF8ToWString(string).c_str());
}

void PlatformWin32::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();
    UpdateMouseCapture();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformWin32::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Windows;
  wsi.render_window = reinterpret_cast<void*>(m_hwnd);
  wsi.render_surface = reinterpret_cast<void*>(m_hwnd);
  return wsi;
}

void PlatformWin32::UpdateWindowPosition()
{
  RECT client{};
  if (GetClientRect(m_hwnd, &client))
    MohPcLayer::SetWindowSize(std::max(client.right - client.left, 1L),
                              std::max(client.bottom - client.top, 1L));

  if (m_window_fullscreen)
    return;

  RECT rc = {};
  if (!GetWindowRect(m_hwnd, &rc))
    return;

  m_window_x = rc.left;
  m_window_y = rc.top;
  m_window_width = rc.right - rc.left;
  m_window_height = rc.bottom - rc.top;
}

void PlatformWin32::UpdateMouseCapture()
{
  const bool want = MohPcLayer::WantsRelativeMouse() && GetForegroundWindow() == m_hwnd;
  if (want == m_mouse_captured)
    return;

  m_mouse_captured = want;
  if (want)
  {
    RECT rect{};
    GetClientRect(m_hwnd, &rect);
    POINT tl{rect.left, rect.top};
    POINT br{rect.right, rect.bottom};
    ClientToScreen(m_hwnd, &tl);
    ClientToScreen(m_hwnd, &br);
    rect = {tl.x, tl.y, br.x, br.y};
    ClipCursor(&rect);
    SetCapture(m_hwnd);
    while (ShowCursor(FALSE) >= 0) {}
  }
  else
  {
    ClipCursor(nullptr);
    if (GetCapture() == m_hwnd)
      ReleaseCapture();
    while (ShowCursor(TRUE) < 0) {}
  }
}

u32 PlatformWin32::TranslateKey(WPARAM key)
{
  if (key >= 'A' && key <= 'Z')
    return static_cast<u32>('a' + (key - 'A'));
  if (key >= '0' && key <= '9')
    return static_cast<u32>(key);
  switch (key)
  {
  case VK_ESCAPE: return 0xff1b;
  case VK_TAB: return 0xff09;
  case VK_SPACE: return 0x20;
  case VK_LCONTROL: return 0xffe3;
  case VK_RCONTROL: return 0xffe4;
  case VK_CONTROL: return 0xffe3;
  case VK_HOME: return 0xff50;
  case VK_OEM_3: return static_cast<u32>('`');
  default: return 0;
  }
}

void PlatformWin32::ProcessEvents()
{
  MSG msg;
  while (PeekMessage(&msg, m_hwnd, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

LRESULT PlatformWin32::WndProc(const HWND hwnd, const UINT msg, const WPARAM wParam,
                               const LPARAM lParam)
{
  PlatformWin32* platform = reinterpret_cast<PlatformWin32*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  switch (msg)
  {
  case WM_NCCREATE:
  {
    platform = static_cast<PlatformWin32*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(platform));
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  case WM_CREATE:
  {
    if (hwnd)
    {
      // Remove rounded corners from the render window on Windows 11
      constexpr DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
      DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_preference,
                            sizeof(corner_preference));
    }
  }
  break;

  case WM_SIZE:
  {
    if (g_presenter)
      g_presenter->ResizeSurface();
    if (platform)
    {
      const int width = std::max<int>(LOWORD(lParam), 1);
      const int height = std::max<int>(HIWORD(lParam), 1);
      MohPcLayer::SetWindowSize(width, height);
    }
  }
  break;

  case WM_INPUT:
  {
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) == 0 && size > 0)
    {
      std::vector<std::byte> data(size);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, data.data(), &size,
                          sizeof(RAWINPUTHEADER)) == size)
      {
        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(data.data());
        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
          const RAWMOUSE& mouse = raw->data.mouse;
          if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
              (mouse.lLastX != 0 || mouse.lLastY != 0))
            MohPcLayer::RelativeMotion(mouse.lLastX, mouse.lLastY);
          const USHORT flags = mouse.usButtonFlags;
          if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) MohPcLayer::PointerButton(0, true);
          if (flags & RI_MOUSE_LEFT_BUTTON_UP) MohPcLayer::PointerButton(0, false);
          if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) MohPcLayer::PointerButton(1, true);
          if (flags & RI_MOUSE_RIGHT_BUTTON_UP) MohPcLayer::PointerButton(1, false);
          if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) MohPcLayer::PointerButton(2, true);
          if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) MohPcLayer::PointerButton(2, false);
          if (flags & RI_MOUSE_WHEEL)
          {
            const SHORT delta = static_cast<SHORT>(mouse.usButtonData);
            MohPcLayer::PointerAxis(-static_cast<double>(delta) / WHEEL_DELTA);
          }
        }
      }
    }
  }
  break;

  case WM_MOUSEMOVE:
    if (platform && !MohPcLayer::WantsRelativeMouse())
      MohPcLayer::PointerAbsolute(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    break;

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
  case WM_KEYUP:
  case WM_SYSKEYUP:
  {
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const u32 key = TranslateKey(wParam);
    if (key)
      MohPcLayer::KeyEvent(key, down);
    if (down && wParam == VK_F10 && (GetKeyState(VK_CONTROL) & 0x8000))
    {
      MohPcLayer::ToggleSettings();
      if (platform) platform->UpdateMouseCapture();
      return 0;
    }
    if (down && wParam == VK_F8 && (GetKeyState(VK_CONTROL) & 0x8000))
    {
      MohPcLayer::ToggleDebug();
      return 0;
    }
    if (down && wParam == VK_OEM_3)
    {
      MohPcLayer::ToggleSettings();
      if (platform) platform->UpdateMouseCapture();
      return 0;
    }
    if (down && wParam == VK_ESCAPE && (GetKeyState(VK_CONTROL) & 0x8000) && platform)
      platform->RequestShutdown();
  }
  break;

  case WM_KILLFOCUS:
    if (platform)
    {
      platform->m_mouse_captured = true;
      platform->UpdateMouseCapture();
    }
    break;

  case WM_CLOSE:
    platform->RequestShutdown();
    break;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  return 0;
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32Platform()
{
  return std::make_unique<PlatformWin32>();
}
