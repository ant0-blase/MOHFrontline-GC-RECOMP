// Copyright 2018 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unistd.h>

// X.h defines None to be 0L, but other parts of Dolphin undef that so that
// None can be used in enums.  Work around that here by copying the definition
// before it is undefined.
#include <X11/X.h>
static constexpr auto X_None = None;

#include "DolphinNoGUI/Platform.h"

#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Core/System.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include "UICommon/UICommon.h"
#include "UICommon/X11Utils.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/MohPcLayer.h"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
#endif

namespace
{
class PlatformX11 : public Platform
{
public:
  ~PlatformX11() override;

  bool Init() override;
  void SetTitle(const std::string& string) override;
  void MainLoop() override;

  WindowSystemInfo GetWindowSystemInfo() const override;

private:
  void CloseDisplay();
  void UpdateWindowPosition();
  void UpdatePcMouseCapture();
  void ProcessEvents();

  Display* m_display = nullptr;
  Window m_window = {};
  Cursor m_blank_cursor = X_None;
  bool m_pc_mouse_captured = false;
#ifdef HAVE_XRANDR
  X11Utils::XRRConfiguration* m_xrr_config = nullptr;
#endif
  int m_window_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  int m_window_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  unsigned int m_window_width = Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH);
  unsigned int m_window_height = Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT);
};

PlatformX11::~PlatformX11()
{
#ifdef HAVE_XRANDR
  delete m_xrr_config;
#endif

  if (m_display)
  {
    if (m_blank_cursor != X_None)
      XFreeCursor(m_display, m_blank_cursor);

    XCloseDisplay(m_display);
  }
}

bool PlatformX11::Init()
{
  XInitThreads();
  MohPcLayer::SetPlatformName("Linux/X11");
  m_display = XOpenDisplay(nullptr);
  if (!m_display)
  {
    PanicAlertFmt("No X11 display found");
    return false;
  }

  m_window = XCreateSimpleWindow(m_display, DefaultRootWindow(m_display), m_window_x, m_window_y,
                                 m_window_width, m_window_height, 0, 0, BlackPixel(m_display, 0));
  XSelectInput(m_display, m_window, StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                                      FocusChangeMask | PointerMotionMask | ButtonPressMask |
                                      ButtonReleaseMask);
  Atom wmProtocols[1];
  wmProtocols[0] = XInternAtom(m_display, "WM_DELETE_WINDOW", True);
  XSetWMProtocols(m_display, m_window, wmProtocols, 1);
  pid_t pid = getpid();
  XChangeProperty(m_display, m_window, XInternAtom(m_display, "_NET_WM_PID", False), XA_CARDINAL,
                  32, PropModeReplace, reinterpret_cast<unsigned char*>(&pid), 1);
  char host_name[HOST_NAME_MAX] = "";
  if (!gethostname(host_name, sizeof(host_name)))
  {
    XTextProperty wmClientMachine = {reinterpret_cast<unsigned char*>(host_name), XA_STRING, 8,
                                     strlen(host_name)};
    XSetWMClientMachine(m_display, m_window, &wmClientMachine);
  }
  XMapRaised(m_display, m_window);
  XFlush(m_display);
  XSync(m_display, True);
  ProcessEvents();

  if (Config::Get(Config::MAIN_DISABLE_SCREENSAVER))
    UICommon::InhibitScreenSaver(true);

#ifdef HAVE_XRANDR
  m_xrr_config = new X11Utils::XRRConfiguration(m_display, m_window);
#endif

  // Create a blank cursor once.  The PC mouse layer uses it while relative
  // capture is active; the original ShowCursor setting still works as before.
  {
    Pixmap blank;
    XColor dummy{};
    char zero_data[1] = {0};
    blank = XCreateBitmapFromData(m_display, m_window, zero_data, 1, 1);
    m_blank_cursor = XCreatePixmapCursor(m_display, blank, blank, &dummy, &dummy, 0, 0);
    XFreePixmap(m_display, blank);
    if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
      XDefineCursor(m_display, m_window, m_blank_cursor);
  }

  // Enter fullscreen if enabled.
  if (Config::Get(Config::MAIN_FULLSCREEN))
  {
    m_window_fullscreen = X11Utils::ToggleFullscreen(m_display, m_window);
#ifdef HAVE_XRANDR
    m_xrr_config->ToggleDisplayMode(True);
#endif
    ProcessEvents();
  }

  UpdateWindowPosition();
  return true;
}

void PlatformX11::SetTitle(const std::string& string)
{
  XStoreName(m_display, m_window, string.c_str());
}

void PlatformX11::MainLoop()
{
  while (IsRunning())
  {
    UpdateRunningFlag();
    Core::HostDispatchJobs(Core::System::GetInstance());
    ProcessEvents();
    UpdateWindowPosition();
    UpdatePcMouseCapture();

    // TODO: Is this sleep appropriate?
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WindowSystemInfo PlatformX11::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::X11;
  wsi.display_connection = static_cast<void*>(m_display);
  wsi.render_window = reinterpret_cast<void*>(m_window);
  wsi.render_surface = reinterpret_cast<void*>(m_window);
  return wsi;
}

void PlatformX11::UpdateWindowPosition()
{
  Window winDummy;
  unsigned int borderDummy, depthDummy;
  XGetGeometry(m_display, m_window, &winDummy, &m_window_x, &m_window_y, &m_window_width,
               &m_window_height, &borderDummy, &depthDummy);
  MohPcLayer::SetWindowSize(static_cast<int>(std::max(m_window_width, 1u)),
                            static_cast<int>(std::max(m_window_height, 1u)));
}

void PlatformX11::UpdatePcMouseCapture()
{
  const bool want = m_window_focus && MohPcLayer::WantsRelativeMouse();
  if (want == m_pc_mouse_captured)
    return;
  m_pc_mouse_captured = want;
  if (want)
  {
    XGrabPointer(m_display, m_window, True, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, m_window, X_None, CurrentTime);
    if (m_blank_cursor != X_None)
      XDefineCursor(m_display, m_window, m_blank_cursor);
    const int cx = static_cast<int>(m_window_width / 2);
    const int cy = static_cast<int>(m_window_height / 2);
    XWarpPointer(m_display, X_None, m_window, 0, 0, 0, 0, cx, cy);
  }
  else
  {
    XUngrabPointer(m_display, CurrentTime);
    if (Config::Get(Config::MAIN_SHOW_CURSOR) != Config::ShowCursor::Never)
      XUndefineCursor(m_display, m_window);
  }
  XFlush(m_display);
}

void PlatformX11::ProcessEvents()
{
  XEvent event;
  KeySym key;
  for (int num_events = XPending(m_display); num_events > 0; num_events--)
  {
    XNextEvent(m_display, &event);
    switch (event.type)
    {
    case KeyRelease:
      key = XLookupKeysym((XKeyEvent*)&event, 0);
      MohPcLayer::KeyEvent(static_cast<u32>(key), false);
      break;
    case KeyPress:
      key = XLookupKeysym((XKeyEvent*)&event, 0);
      MohPcLayer::KeyEvent(static_cast<u32>(key), true);
      if (key == XK_Escape && (event.xkey.state & ControlMask))
      {
        RequestShutdown();
      }
      else if ((key == XK_F10 && (event.xkey.state & ControlMask)) || key == XK_grave)
      {
        MohPcLayer::ToggleSettings();
        UpdatePcMouseCapture();
      }
      else if (key == XK_F8 && (event.xkey.state & ControlMask))
      {
        MohPcLayer::ToggleDebug();
      }
      else if (key == XK_F10)
      {
        if (Core::GetState(Core::System::GetInstance()) == Core::State::Running)
          Core::SetState(Core::System::GetInstance(), Core::State::Paused);
        else
          Core::SetState(Core::System::GetInstance(), Core::State::Running);
      }
      else if ((key == XK_Return) && (event.xkey.state & Mod1Mask))
      {
        m_window_fullscreen = !m_window_fullscreen;
        X11Utils::ToggleFullscreen(m_display, m_window);
#ifdef HAVE_XRANDR
        m_xrr_config->ToggleDisplayMode(m_window_fullscreen);
#endif
        UpdateWindowPosition();
      }
      else if (key >= XK_F1 && key <= XK_F8)
      {
        int slot_number = key - XK_F1 + 1;
        if (event.xkey.state & ShiftMask)
          State::Save(Core::System::GetInstance(), slot_number);
        else
          State::Load(Core::System::GetInstance(), slot_number);
      }
      else if (key == XK_F9)
        Core::SaveScreenShot();
      else if (key == XK_F11)
        State::LoadLastSaved(Core::System::GetInstance());
      else if (key == XK_F12)
      {
        if (event.xkey.state & ShiftMask)
          State::UndoLoadState(Core::System::GetInstance());
        else
          State::UndoSaveState(Core::System::GetInstance());
      }
      break;
    case FocusIn:
    {
      m_window_focus = true;
      UpdatePcMouseCapture();
    }
    break;
    case FocusOut:
    {
      m_window_focus = false;
      UpdatePcMouseCapture();
    }
    break;
    case MotionNotify:
    {
      if (MohPcLayer::WantsRelativeMouse())
      {
        const int cx = static_cast<int>(m_window_width / 2);
        const int cy = static_cast<int>(m_window_height / 2);
        const int dx = event.xmotion.x - cx;
        const int dy = event.xmotion.y - cy;
        if (dx != 0 || dy != 0)
        {
          MohPcLayer::RelativeMotion(dx, dy);
          XWarpPointer(m_display, X_None, m_window, 0, 0, 0, 0, cx, cy);
          XFlush(m_display);
        }
      }
      else
      {
        MohPcLayer::PointerAbsolute(event.xmotion.x, event.xmotion.y);
      }
    }
    break;
    case ButtonPress:
    case ButtonRelease:
    {
      const bool down = event.type == ButtonPress;
      if (event.xbutton.button == Button1) MohPcLayer::PointerButton(0, down);
      else if (event.xbutton.button == Button3) MohPcLayer::PointerButton(1, down);
      else if (event.xbutton.button == Button2) MohPcLayer::PointerButton(2, down);
      else if (down && event.xbutton.button == Button4) MohPcLayer::PointerAxis(-1.0);
      else if (down && event.xbutton.button == Button5) MohPcLayer::PointerAxis(1.0);
    }
    break;
    case ClientMessage:
    {
      if ((unsigned long)event.xclient.data.l[0] ==
          XInternAtom(m_display, "WM_DELETE_WINDOW", False))
        Stop();
    }
    break;
    case ConfigureNotify:
    {
      if (g_presenter)
        g_presenter->ResizeSurface();
    }
    break;
    }
  }
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateX11Platform()
{
  return std::make_unique<PlatformX11>();
}
