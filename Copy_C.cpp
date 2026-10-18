#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"CopyCWindow";
constexpr UINT kEncodeFinished = WM_APP + 1;
constexpr int kChooseButtonId = 1001;
constexpr int kStatusId = 1002;
constexpr int kProgressId = 1003;

HWND g_status = nullptr;
HWND g_progress = nullptr;
HWND g_choose_button = nullptr;
HWND g_window = nullptr;
std::thread g_worker;
std::atomic<bool> g_busy = false;
std::wstring g_output_path;
std::wstring g_temporary_video;
std::wstring g_temporary_log;
std::wstring g_worker_error;
bool g_overwrite_confirmed = false;

std::wstring QuoteArgument(const std::wstring& argument) {
  std::wstring quoted = L"\"";
  size_t backslashes = 0;
  for (wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
    } else if (character == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslashes = 0;
    } else {
      quoted.append(backslashes, L'\\');
      backslashes = 0;
      quoted.push_back(character);
    }
  }
  quoted.append(backslashes * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

bool RunAndCapture(const std::wstring& command, std::wstring& output,
                   std::wstring& error_message) {
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
    error_message = L"Could not create a pipe to run ffprobe.";
    return false;
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  HANDLE null_device = CreateFileW(
      L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_device == INVALID_HANDLE_VALUE) {
    CloseHandle(read_pipe);
    CloseHandle(write_pipe);
    error_message = L"Could not open the Windows NUL device.";
    return false;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = null_device;
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr,
                                nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &process);
  CloseHandle(write_pipe);
  CloseHandle(null_device);
  if (!created) {
    CloseHandle(read_pipe);
    error_message = L"Could not start ffprobe.exe. Install FFmpeg and add it to PATH.";
    return false;
  }

  std::string bytes;
  char buffer[4096];
  DWORD bytes_read = 0;
  while (ReadFile(read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) &&
         bytes_read != 0) {
    bytes.append(buffer, bytes_read);
  }
  CloseHandle(read_pipe);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  output.assign(bytes.begin(), bytes.end());
  if (exit_code != 0) {
    error_message = L"ffprobe could not read this video. Check that it is a valid MP4 file.";
    return false;
  }
  return true;
}

bool GetVideoDuration(const std::wstring& source, double& duration,
                      std::wstring& error_message) {
  std::wstring command =
      L"ffprobe.exe -v error -show_entries format=duration "
      L"-of default=noprint_wrappers=1:nokey=1 " +
      QuoteArgument(source);
  std::wstring result;
  if (!RunAndCapture(command, result, error_message)) return false;

  try {
    size_t consumed = 0;
    duration = std::stod(result, &consumed);
    if (consumed == 0 || duration < 0 || !std::isfinite(duration)) {
      error_message = L"ffprobe did not return a valid video duration.";
      return false;
    }
  } catch (...) {
    error_message = L"ffprobe did not return a valid video duration.";
    return false;
  }
  return true;
}

std::wstring MakeTemporaryVideoPath(const std::wstring& output) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    std::wstring candidate =
        output + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(ticks + attempt) + L".mp4";
    HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      return candidate;
    }
  }
  return {};
}

std::wstring MakeTemporaryLogPath() {
  wchar_t directory[MAX_PATH]{};
  if (!GetTempPathW(MAX_PATH, directory)) return {};
  wchar_t path[MAX_PATH]{};
  if (!GetTempFileNameW(directory, L"cpy", 0, path)) return {};
  return path;
}

void SetStatus(const std::wstring& message) {
  SetWindowTextW(g_status, message.c_str());
}

void ShowError(const std::wstring& message) {
  SetStatus(message);
  MessageBoxW(g_window, message.c_str(), L"Copy_C", MB_OK | MB_ICONERROR);
}

void RemoveTemporaryFiles() {
  if (!g_temporary_video.empty()) {
    DeleteFileW(g_temporary_video.c_str());
    g_temporary_video.clear();
  }
  if (!g_temporary_log.empty()) {
    DeleteFileW(g_temporary_log.c_str());
    g_temporary_log.clear();
  }
}

void EncodeMiddle(const std::wstring& source, const std::wstring& temporary,
                  const std::wstring& log_path, double start) {
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE log_file = CreateFileW(log_path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (log_file == INVALID_HANDLE_VALUE) {
    g_worker_error = L"Could not create a temporary FFmpeg log file.";
    PostMessageW(g_window, kEncodeFinished, 0, 0);
    return;
  }

  HANDLE null_device = CreateFileW(L"NUL", GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_device == INVALID_HANDLE_VALUE) {
    CloseHandle(log_file);
    g_worker_error = L"Could not open the Windows NUL device.";
    PostMessageW(g_window, kEncodeFinished, 0, 0);
    return;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_device;
  startup.hStdOutput = null_device;
  startup.hStdError = log_file;
  PROCESS_INFORMATION process{};
  std::wstring command =
      L"ffmpeg.exe -y -nostdin -hide_banner -loglevel error -ss " +
      std::to_wstring(start) + L" -i " + QuoteArgument(source) +
      L" -t 20 -map 0:v:0 -map 0:a? -c:v libx264 -preset veryfast "
      L"-crf 18 -c:a aac -b:a 192k -map_metadata 0 -movflags +faststart " +
      QuoteArgument(temporary);
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr,
                                nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &process);
  CloseHandle(null_device);
  CloseHandle(log_file);
  if (!created) {
    g_worker_error = L"Could not start ffmpeg.exe. Install FFmpeg and add it to PATH.";
    PostMessageW(g_window, kEncodeFinished, 0, 0);
    return;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  if (exit_code != 0) {
    std::ifstream log(fs::path(log_path), std::ios::binary);
    std::string details((std::istreambuf_iterator<char>(log)),
                        std::istreambuf_iterator<char>());
    g_worker_error = L"FFmpeg could not create the clip.";
    if (!details.empty()) {
      g_worker_error += L"\n";
      g_worker_error.append(details.begin(), details.end());
    }
  }
  PostMessageW(g_window, kEncodeFinished, 0, 0);
}

void StartEncoding(const std::wstring& source, double duration) {
  g_temporary_video = MakeTemporaryVideoPath(g_output_path);
  g_temporary_log = MakeTemporaryLogPath();
  if (g_temporary_video.empty() || g_temporary_log.empty()) {
    RemoveTemporaryFiles();
    ShowError(L"Could not prepare temporary files for the clip.");
    return;
  }

  g_worker_error.clear();
  g_busy = true;
  EnableWindow(g_choose_button, FALSE);
  EnableMenuItem(GetSystemMenu(g_window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
  SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 30);
  SetStatus(L"Creating the middle 20-second clip...");
  const double start = (duration - 20.0) / 2.0;
  g_worker = std::thread(EncodeMiddle, source, g_temporary_video,
                         g_temporary_log, start);
}

void OnVideoSelected(const std::wstring& source) {
  std::wstring extension = fs::path(source).extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](wchar_t character) { return std::towlower(character); });
  if (extension != L".mp4") {
    ShowError(L"Please select an MP4 file.");
    return;
  }

  double duration = 0;
  std::wstring error_message;
  if (!GetVideoDuration(source, duration, error_message)) {
    ShowError(error_message);
    return;
  }
  if (duration < 20.0) {
    ShowError(L"This video is shorter than 20 seconds.");
    return;
  }

  fs::path source_path(source);
  fs::path output_path = source_path.parent_path() /
      (source_path.stem().wstring() + L"_20onec" + source_path.extension().wstring());
  g_output_path = output_path.wstring();

  if (GetFileAttributesW(g_output_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    int answer = MessageBoxW(
        g_window, (L"Replace the existing clip?\n\n" + g_output_path).c_str(),
        L"Copy_C", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) return;
  }
  StartEncoding(source, duration);
}

void ChooseVideo() {
  IFileOpenDialog* dialog = nullptr;
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
  if (FAILED(result)) {
    ShowError(L"Could not open the Windows file picker.");
    return;
  }

  COMDLG_FILTERSPEC filters[] = {{L"MP4 videos", L"*.mp4;*.MP4"}};
  dialog->SetFileTypes(1, filters);
  dialog->SetTitle(L"Select an MP4 video");

  PWSTR downloads = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT,
                                     nullptr, &downloads))) {
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(downloads, nullptr,
                                               IID_PPV_ARGS(&folder)))) {
      dialog->SetFolder(folder);
      folder->Release();
    }
    CoTaskMemFree(downloads);
  }

  result = dialog->Show(g_window);
  if (SUCCEEDED(result)) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        OnVideoSelected(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  } else if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    ShowError(L"The Windows file picker could not select a file.");
  }
  dialog->Release();
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                 WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      g_window = window;
      HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
      HWND title = CreateWindowW(
          L"STATIC", L"Copy the middle 20 seconds of an MP4",
          WS_CHILD | WS_VISIBLE, 22, 22, 430, 28, window, nullptr,
          nullptr, nullptr);
      SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

      HWND description = CreateWindowW(
          L"STATIC", L"Choose a video at least 20 seconds long. The clip is saved beside it.",
          WS_CHILD | WS_VISIBLE, 22, 62, 430, 42, window, nullptr,
          nullptr, nullptr);
      SendMessageW(description, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

      g_status = CreateWindowW(
          L"STATIC", L"Output name: original-name_20onec.mp4",
          WS_CHILD | WS_VISIBLE | SS_LEFT, 22, 112, 430, 42, window,
          reinterpret_cast<HMENU>(kStatusId), nullptr, nullptr);
      SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

      g_progress = CreateWindowExW(
          0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE,
          22, 165, 430, 22, window, reinterpret_cast<HMENU>(kProgressId),
          nullptr, nullptr);

      g_choose_button = CreateWindowW(
          L"BUTTON", L"Choose MP4...", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          BS_DEFPUSHBUTTON, 318, 196, 134, 34, window,
          reinterpret_cast<HMENU>(kChooseButtonId), nullptr, nullptr);
      SendMessageW(g_choose_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kChooseButtonId) ChooseVideo();
      return 0;
    case kEncodeFinished: {
      if (g_worker.joinable()) g_worker.join();
      SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
      EnableWindow(g_choose_button, TRUE);
      EnableMenuItem(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
      g_busy = false;

      if (!g_worker_error.empty()) {
        RemoveTemporaryFiles();
        ShowError(g_worker_error);
      } else if (!MoveFileExW(g_temporary_video.c_str(), g_output_path.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        RemoveTemporaryFiles();
        ShowError(L"Could not save the clip. Windows error " +
                  std::to_wstring(error) + L".");
      } else {
        g_temporary_video.clear();
        if (!g_temporary_log.empty()) {
          DeleteFileW(g_temporary_log.c_str());
          g_temporary_log.clear();
        }
        SetStatus(L"Created " + g_output_path);
        SendMessageW(g_progress, PBM_SETPOS, 100, 0);
      }
      return 0;
    }
    case WM_CLOSE:
      if (g_busy) return 0;
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) return 1;

  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&controls);

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProcedure;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = kWindowClass;
  if (!RegisterClassW(&window_class)) {
    CoUninitialize();
    return 1;
  }

  HWND window = CreateWindowExW(
      0, kWindowClass, L"Copy_C", WS_OVERLAPPED | WS_CAPTION |
      WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
      490, 285, nullptr, nullptr, instance, nullptr);
  if (!window) {
    CoUninitialize();
    return 1;
  }

  ShowWindow(window, show_command);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  CoUninitialize();
  return static_cast<int>(message.wParam);
}
