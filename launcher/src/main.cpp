#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct DialogState
{
  std::mutex mutex;
  std::optional<fs::path> selected;
  std::string error;
};

struct Settings
{
  fs::path iso;
  fs::path ps3_files;
  bool ps3_assets = true;
  bool enhanced_graphics = true;
};

fs::path CanonicalOr(const fs::path& path, const fs::path& fallback)
{
  std::error_code ec;
  const fs::path value = fs::weakly_canonical(path, ec);
  return ec ? fallback : value;
}

fs::path FindProjectRoot(const char* argv0)
{
  std::error_code ec;
#if defined(__linux__)
  const fs::path proc = fs::read_symlink("/proc/self/exe", ec);
  if (!ec)
  {
    fs::path current = proc.parent_path();
    for (int i = 0; i < 6; ++i)
    {
      if (fs::is_regular_file(current / "run.sh") &&
          fs::is_directory(current / "ModernGekko"))
        return current;
      if (!current.has_parent_path())
        break;
      current = current.parent_path();
    }
  }
#endif
  const fs::path exe = CanonicalOr(argv0, fs::current_path());
  fs::path current = exe.has_parent_path() ? exe.parent_path() : fs::current_path();
  for (int i = 0; i < 6; ++i)
  {
    if (fs::is_regular_file(current / "run.sh") &&
        fs::is_directory(current / "ModernGekko"))
      return current;
    if (!current.has_parent_path())
      break;
    current = current.parent_path();
  }
  return fs::current_path();
}

fs::path SettingsPath(const fs::path& root)
{
  return root / "user" / "launcher.ini";
}

Settings LoadSettings(const fs::path& root)
{
  Settings settings;
  settings.ps3_files = root / "HD" / "PS3_FILES";

  std::ifstream input(SettingsPath(root));
  std::string line;
  while (std::getline(input, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const auto split = line.find('=');
    if (split == std::string::npos)
      continue;
    const std::string key = line.substr(0, split);
    const std::string value = line.substr(split + 1);
    if (key == "iso")
      settings.iso = value;
    else if (key == "ps3_files" && !value.empty())
      settings.ps3_files = value;
    else if (key == "ps3_assets")
      settings.ps3_assets = value == "1" || value == "true";
    else if (key == "enhanced_graphics")
      settings.enhanced_graphics = value == "1" || value == "true";
  }
  return settings;
}

bool SaveSettings(const fs::path& root, const Settings& settings, std::string* error)
{
  std::error_code ec;
  fs::create_directories(root / "user", ec);
  if (ec)
  {
    *error = "Unable to create user directory: " + ec.message();
    return false;
  }

  std::ofstream output(SettingsPath(root), std::ios::trunc);
  if (!output)
  {
    *error = "Unable to write launcher.ini";
    return false;
  }
  output << "iso=" << settings.iso.string() << '\n'
         << "ps3_files=" << settings.ps3_files.string() << '\n'
         << "ps3_assets=" << (settings.ps3_assets ? 1 : 0) << '\n'
         << "enhanced_graphics=" << (settings.enhanced_graphics ? 1 : 0) << '\n';
  return static_cast<bool>(output);
}

void SDLCALL FileDialogCallback(void* userdata, const char* const* filelist, int)
{
  auto* state = static_cast<DialogState*>(userdata);
  std::lock_guard lock(state->mutex);
  if (!filelist)
    state->error = SDL_GetError();
  else if (filelist[0])
    state->selected = fs::path(filelist[0]);
}

bool Spawn(const std::vector<std::string>& storage, SDL_Process** process, std::string* error)
{
  std::vector<const char*> args;
  args.reserve(storage.size() + 1);
  for (const auto& item : storage)
    args.push_back(item.c_str());
  args.push_back(nullptr);

  *process = SDL_CreateProcess(args.data(), false);
  if (!*process)
  {
    *error = SDL_GetError();
    return false;
  }
  return true;
}

bool OpenFolder(const fs::path& path, std::string* error)
{
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec)
  {
    *error = "Unable to create folder: " + ec.message();
    return false;
  }

  SDL_Process* process = nullptr;
#if defined(_WIN32)
  const std::vector<std::string> command = {"explorer.exe", path.string()};
#else
  const std::vector<std::string> command = {"xdg-open", path.string()};
#endif
  if (!Spawn(command, &process, error))
    return false;
  SDL_DestroyProcess(process);
  return true;
}

bool IsReady(const fs::path& root, const Settings& settings)
{
  if (settings.iso.empty() || !fs::is_regular_file(settings.iso))
    return false;
#if defined(_WIN32)
  return fs::is_regular_file(root / "runtime" / "moderngekko-run.exe") &&
         fs::is_regular_file(root / "module" / "gGMFE69_recomp.dll");
#else
  return fs::is_regular_file(root / "runtime" / "moderngekko-run") &&
         fs::is_regular_file(root / "module" / "gGMFE69_recomp.so");
#endif
}

std::string ReadyText(const fs::path& root, const Settings& settings)
{
  if (IsReady(root, settings))
    return "Ready to launch directly from disc image";
  if (settings.iso.empty() || !fs::is_regular_file(settings.iso))
    return "Select a supported GMFE69 disc image";
  return "Native module has not been built yet";
}

bool StartBuild(const fs::path& root, const Settings& settings, SDL_Process** process,
                std::string* error)
{
  if (settings.iso.empty() || !fs::is_regular_file(settings.iso))
  {
    *error = "Select your legally owned GMFE69 disc image first.";
    return false;
  }

#if defined(_WIN32)
  return Spawn({"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", (root / "scripts" / "build-portable.ps1").string(),
                "-DiscImage", settings.iso.string()},
               process, error);
#else
  return Spawn({"bash", (root / "scripts" / "build-portable.sh").string(),
                settings.iso.string()},
               process, error);
#endif
}

bool StartGame(const fs::path& root, const Settings& settings, SDL_Process** process,
               std::string* error)
{
  if (!IsReady(root, settings))
  {
    *error = "Build the recompilation before launching.";
    return false;
  }

#if defined(_WIN32)
  std::vector<std::string> command = {
      "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
      "-File", (root / "scripts" / "run-windows.ps1").string(),
      "-Ps3Files", settings.ps3_files.string()};
  command.push_back(settings.ps3_assets ? "-Ps3Assets" : "-NoPs3Assets");
  command.push_back(settings.enhanced_graphics ? "-EnhancedGraphics" : "-OriginalGraphics");
  return Spawn(command, process, error);
#else
  std::vector<std::string> command = {
      "bash", (root / "run.sh").string(),
      "--ps3-files", settings.ps3_files.string(),
      settings.ps3_assets ? "--ps3-assets" : "--no-ps3-assets",
      settings.enhanced_graphics ? "--enhanced-graphics" : "--original-graphics"};
  return Spawn(command, process, error);
#endif
}
} // namespace

int main(int argc, char** argv)
{
  const fs::path root = FindProjectRoot(argc > 0 ? argv[0] : "");
  Settings settings = LoadSettings(root);

  std::error_code ec;
  fs::create_directories(root / "user", ec);
  fs::create_directories(settings.ps3_files, ec);

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    return 1;

  const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  SDL_Window* window =
      SDL_CreateWindow("Medal of Honor: Frontline - Recompiled",
                       static_cast<int>(760 * scale), static_cast<int>(570 * scale),
                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window)
    return 1;

  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (!renderer)
  {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetStyle().FontScaleDpi = scale;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  DialogState iso_dialog;
  DialogState ps3_dialog;
  SDL_Process* build_process = nullptr;
  SDL_Process* game_process = nullptr;
  std::string status = ReadyText(root, settings);
  std::string error;
  bool done = false;

  while (!done)
  {
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        done = true;
    }

    {
      std::lock_guard lock(iso_dialog.mutex);
      if (iso_dialog.selected)
      {
        settings.iso = std::move(*iso_dialog.selected);
        iso_dialog.selected.reset();
        error.clear();
        SaveSettings(root, settings, &error);
      }
      if (!iso_dialog.error.empty())
      {
        error = std::move(iso_dialog.error);
        iso_dialog.error.clear();
      }
    }
    {
      std::lock_guard lock(ps3_dialog.mutex);
      if (ps3_dialog.selected)
      {
        settings.ps3_files = std::move(*ps3_dialog.selected);
        ps3_dialog.selected.reset();
        settings.ps3_assets = true;
        settings.enhanced_graphics = true;
        error.clear();
        SaveSettings(root, settings, &error);
      }
      if (!ps3_dialog.error.empty())
      {
        error = std::move(ps3_dialog.error);
        ps3_dialog.error.clear();
      }
    }

    if (build_process)
    {
      int exit_code = 0;
      if (SDL_WaitProcess(build_process, false, &exit_code))
      {
        SDL_DestroyProcess(build_process);
        build_process = nullptr;
        status = exit_code == 0 ? "Build complete - ready to launch"
                                : "Build failed - see user/launcher-build.log";
      }
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("MOHFrontline-Recompiled", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("Medal of Honor: Frontline - Recompiled");
    ImGui::TextDisabled("GMFE69 static recompilation launcher");
    ImGui::Separator();

    ImGui::TextUnformatted("GameCube game");
    if (settings.iso.empty())
      ImGui::TextDisabled("No disc image selected");
    else
      ImGui::TextWrapped("%s", settings.iso.string().c_str());

    if (ImGui::Button("Select GMFE69 disc image"))
    {
      static constexpr SDL_DialogFileFilter filters[] = {
          {"GameCube disc images", "iso;gcm;rvz;wia;wbfs;ciso;gcz;tgc;nfs"},
          {"ISO / GCM", "iso;gcm"},
          {"Compressed images", "rvz;wia;wbfs;ciso;gcz;tgc;nfs"}};
      SDL_ShowOpenFileDialog(FileDialogCallback, &iso_dialog, window, filters,
                             static_cast<int>(std::size(filters)), nullptr, false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(settings.iso.empty() || build_process != nullptr);
    if (ImGui::Button(build_process ? "Building..." : "Prepare / Build recompilation"))
    {
      error.clear();
      if (StartBuild(root, settings, &build_process, &error))
        status = "Building recompilation...";
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextUnformatted("PS3 remaster assets");
    ImGui::TextWrapped("%s", settings.ps3_files.string().c_str());

    if (ImGui::Button("Choose PS3_FILES folder"))
      SDL_ShowOpenFolderDialog(FileDialogCallback, &ps3_dialog, window,
                               settings.ps3_files.string().c_str(), false);
    ImGui::SameLine();
    if (ImGui::Button("Open PS3 assets"))
      OpenFolder(settings.ps3_files, &error);

    if (ImGui::Checkbox("Enable PS3 assets", &settings.ps3_assets))
    {
      if (settings.ps3_assets)
        settings.enhanced_graphics = true;
      SaveSettings(root, settings, &error);
    }
    if (ImGui::Checkbox("Enhanced graphics / post-processing", &settings.enhanced_graphics))
      SaveSettings(root, settings, &error);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Folders");
    if (ImGui::Button("Open user folder"))
      OpenFolder(root / "user", &error);
    ImGui::SameLine();
    if (ImGui::Button("Open logs"))
      OpenFolder(root / "user" / "Logs", &error);
    ImGui::SameLine();
    if (ImGui::Button("Open AOT cache"))
      OpenFolder(root / "disc-cache" / "GMFE69", &error);

    ImGui::Spacing();
    ImGui::Separator();
    status = build_process ? "Building recompilation..." : ReadyText(root, settings);
    ImGui::Text("Status: %s", status.c_str());
    if (!error.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.30f, 1.0f), "%s", error.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(!IsReady(root, settings));
    if (ImGui::Button("PLAY", ImVec2(220 * scale, 52 * scale)))
    {
      error.clear();
      if (game_process)
      {
        SDL_DestroyProcess(game_process);
        game_process = nullptr;
      }
      if (StartGame(root, settings, &game_process, &error))
        SDL_MinimizeWindow(window);
    }
    ImGui::EndDisabled();

    ImGui::End();

    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 18, 20, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  if (build_process)
    SDL_DestroyProcess(build_process);
  if (game_process)
    SDL_DestroyProcess(game_process);

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
