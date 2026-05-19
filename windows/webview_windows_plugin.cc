#include "include/webview_windows/webview_windows_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include "util/string_converter.h"
#include "webview_bridge.h"
#include "webview_host.h"
#include "webview_platform.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

namespace {

constexpr auto kMethodInitialize = "initialize";
constexpr auto kMethodDispose = "dispose";
constexpr auto kMethodInitializeEnvironment = "initializeEnvironment";
constexpr auto kMethodGetWebViewVersion = "getWebViewVersion";

constexpr auto kErrorCodeInvalidId = "invalid_id";
constexpr auto kErrorCodeEnvironmentCreationFailed =
    "environment_creation_failed";
constexpr auto kErrorCodeEnvironmentAlreadyInitialized =
    "environment_already_initialized";
constexpr auto kErrorCodeWebviewCreationFailed = "webview_creation_failed";
constexpr auto kErrorUnsupportedPlatform = "unsupported_platform";

template <typename T>
std::optional<T> GetOptionalValue(const flutter::EncodableMap& map,
                                  const std::string& key) {
  const auto it = map.find(flutter::EncodableValue(key));
  if (it != map.end()) {
    const auto val = std::get_if<T>(&it->second);
    if (val) {
      return *val;
    }
  }
  return std::nullopt;
}

class WebviewWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  WebviewWindowsPlugin(flutter::TextureRegistrar* textures,
                       flutter::BinaryMessenger* messenger);

  virtual ~WebviewWindowsPlugin();

  // Transfer ownership of the plugin-level method channel to the
  // plugin so it outlives the RegisterWithRegistrar scope. The
  // channel is intentionally NEVER asked to null its handler in
  // the destructor — that path AVed on the engine's destroy order
  // (Sentry MOBILE-NEWS-CN). The alive_ flag below makes that
  // unnecessary: any callback that fires after destruction sees
  // alive_=false and returns without touching `*this`.
  void set_method_channel(
      std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
          channel) {
    channel_ = std::move(channel);
  }

  // Shared "alive" flag — see WebviewBridge::alive() for full
  // rationale. Captured by-value (shared_ptr) into the plugin-level
  // MethodCallHandler lambda. ~WebviewWindowsPlugin sets this to
  // false first thing, so any callback the messenger dispatches
  // afterwards no-ops cleanly.
  std::shared_ptr<std::atomic<bool>> alive() const { return alive_; }

 private:
  std::shared_ptr<std::atomic<bool>> alive_;
  std::unique_ptr<WebviewPlatform> platform_;
  std::unique_ptr<WebviewHost> webview_host_;
  std::unordered_map<int64_t, std::unique_ptr<WebviewBridge>> instances_;
  // Plugin-level method channel. Held as a member so the lambda we
  // pass to SetMethodCallHandler keeps a stable target across the
  // RegisterWithRegistrar scope. We do NOT null the handler in the
  // destructor (see set_method_channel comment above).
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;

  WNDCLASS window_class_ = {};
  flutter::TextureRegistrar* textures_;
  flutter::BinaryMessenger* messenger_;

  bool InitPlatform();

  void CreateWebviewInstance(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>);
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

// static
void WebviewWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  // Defensive null checks: on some Windows engine builds (and during
  // partially-initialised plugin registration ordering) the registrar or
  // its messenger/texture_registrar can be null or point at an
  // incompletely-constructed Flutter wrapper. Dereferencing them here
  // surfaces as `EXCEPTION_ACCESS_VIOLATION_READ` at offsets like 0x1e8
  // inside `FlutterDesktopMessengerSetCallback`, which crashes the host
  // app before it ever paints. Bail out gracefully instead — the plugin
  // will be non-functional this session, but the app keeps running.
  if (!registrar) {
    OutputDebugStringW(
        L"[webview_windows] RegisterWithRegistrar: registrar is null; "
        L"skipping plugin registration.\n");
    return;
  }

  auto* messenger = registrar->messenger();
  if (!messenger) {
    OutputDebugStringW(
        L"[webview_windows] RegisterWithRegistrar: registrar->messenger() "
        L"returned null; skipping plugin registration.\n");
    return;
  }

  auto* texture_registrar = registrar->texture_registrar();
  if (!texture_registrar) {
    OutputDebugStringW(
        L"[webview_windows] RegisterWithRegistrar: "
        L"registrar->texture_registrar() returned null; skipping plugin "
        L"registration.\n");
    return;
  }

  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          messenger, "io.jns.webview.win",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin =
      std::make_unique<WebviewWindowsPlugin>(texture_registrar, messenger);

  // Capture the alive flag by VALUE (shared_ptr) into the lambda.
  // The flag outlives the plugin for as long as the messenger still
  // holds this lambda — so a late-firing WM_ message that arrives
  // after the plugin destructed atomic-loads alive_=false and the
  // callback returns an error rather than dereferencing freed
  // `*plugin_pointer`. This replaces the earlier defence (commit
  // 6939335) which tried to null the handler in the destructor;
  // that path itself AVed inside FlutterDesktopMessengerSetCallback
  // when the engine had already invalidated the messenger by the
  // time the plugin destructed (Sentry MOBILE-NEWS-CN, 2026-05).
  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get(), alive = plugin->alive()](
          const auto& call, auto result) {
        if (!alive->load(std::memory_order_acquire)) {
          result->Error("plugin_destroyed",
                        "WebviewWindowsPlugin no longer alive");
          return;
        }
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  // Hand the channel to the plugin so it outlives this scope. We
  // never null the handler in the destructor — the alive_ flag
  // above is the safety net for callbacks queued past teardown.
  plugin->set_method_channel(std::move(channel));

  registrar->AddPlugin(std::move(plugin));
}

WebviewWindowsPlugin::WebviewWindowsPlugin(flutter::TextureRegistrar* textures,
                                           flutter::BinaryMessenger* messenger)
    : alive_(std::make_shared<std::atomic<bool>>(true)),
      textures_(textures),
      messenger_(messenger) {
  window_class_.lpszClassName = L"FlutterWebviewMessage";
  window_class_.lpfnWndProc = &DefWindowProc;
  RegisterClass(&window_class_);
}

WebviewWindowsPlugin::~WebviewWindowsPlugin() {
  // Flip the alive flag FIRST. Any callback the engine messenger
  // dispatches after this point — including ones queued in the
  // Win32 message pump before WM_CLOSE — sees alive_=false at its
  // atomic_load and returns an error without dereferencing freed
  // `*this`. memory_order_release pairs with the acquire load in
  // the lambda (RegisterWithRegistrar).
  alive_->store(false, std::memory_order_release);

  // Tear down child bridges. Each bridge sets its own alive_=false
  // first thing and SEH-guards any messenger-adjacent cleanup
  // (see WebviewBridge::~WebviewBridge for the full rationale).
  instances_.clear();

  UnregisterClass(window_class_.lpszClassName, nullptr);

  // Deliberately NOT calling channel_->SetMethodCallHandler(nullptr).
  // The previous fix (commit 6939335) added that call to prevent
  // the lambda from being dispatched on a freed `plugin_pointer`,
  // but the call itself routes through
  // FlutterDesktopMessengerSetCallback, which AVs at offset 0x1e8
  // when the engine's destroy order has already invalidated the
  // messenger before the plugin destructs (Sentry MOBILE-NEWS-CN,
  // re-fired 2026-05-19 on the same signature it was supposed to
  // address). The alive_ flag above provides the same protection
  // without touching the messenger.
}

void WebviewWindowsPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare(kMethodInitializeEnvironment) == 0) {
    if (webview_host_) {
      return result->Error(kErrorCodeEnvironmentAlreadyInitialized,
                           "The webview environment is already initialized");
    }

    if (!InitPlatform()) {
      return result->Error(kErrorUnsupportedPlatform,
                           "The platform is not supported");
    }

    const auto& map = std::get<flutter::EncodableMap>(*method_call.arguments());

    std::optional<std::wstring> browser_exe_wpath = std::nullopt;
    std::optional<std::string> browser_exe_path =
        GetOptionalValue<std::string>(map, "browserExePath");
    if (browser_exe_path) {
      browser_exe_wpath = util::Utf16FromUtf8(*browser_exe_path);
    }

    std::optional<std::wstring> user_data_wpath = std::nullopt;
    std::optional<std::string> user_data_path =
        GetOptionalValue<std::string>(map, "userDataPath");
    if (user_data_path) {
      user_data_wpath = util::Utf16FromUtf8(*user_data_path);
    } else {
      user_data_wpath = platform_->GetDefaultDataDirectory();
    }

    std::optional<std::string> additional_args =
        GetOptionalValue<std::string>(map, "additionalArguments");

    webview_host_ = std::move(WebviewHost::Create(
        platform_.get(), user_data_wpath, browser_exe_wpath, additional_args));
    if (!webview_host_) {
      return result->Error(kErrorCodeEnvironmentCreationFailed);
    }

    return result->Success();
  }

  if (method_call.method_name().compare(kMethodGetWebViewVersion) == 0) {
    LPWSTR version_info = nullptr;
    auto hr =
        GetAvailableCoreWebView2BrowserVersionString(nullptr, &version_info);
    if (SUCCEEDED(hr) && version_info != nullptr) {
      return result->Success(
          flutter::EncodableValue(util::Utf8FromUtf16(version_info)));
    } else {
      return result->Success();
    }
  }

  if (method_call.method_name().compare(kMethodInitialize) == 0) {
    return CreateWebviewInstance(std::move(result));
  }

  if (method_call.method_name().compare(kMethodDispose) == 0) {
    if (const auto texture_id = std::get_if<int64_t>(method_call.arguments())) {
      const auto it = instances_.find(*texture_id);
      if (it != instances_.end()) {
        instances_.erase(it);
        return result->Success();
      }
    }
    return result->Error(kErrorCodeInvalidId);
  } else {
    result->NotImplemented();
  }
}

void WebviewWindowsPlugin::CreateWebviewInstance(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (!InitPlatform()) {
    return result->Error(kErrorUnsupportedPlatform,
                         "The platform is not supported");
  }

  if (!webview_host_) {
    webview_host_ = std::move(WebviewHost::Create(
        platform_.get(), platform_->GetDefaultDataDirectory()));
    if (!webview_host_) {
      return result->Error(kErrorCodeEnvironmentCreationFailed);
    }
  }

  auto hwnd =
      CreateWindowEx(0, window_class_.lpszClassName, L"", 0, 0, 0, 0, 0,
                     HWND_MESSAGE, nullptr, window_class_.hInstance, nullptr);

  std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
      shared_result = std::move(result);
  webview_host_->CreateWebview(
      hwnd, true, true,
      [shared_result, this](std::unique_ptr<Webview> webview,
                            std::unique_ptr<WebviewCreationError> error) {
        if (!webview) {
          if (error) {
            return shared_result->Error(
                kErrorCodeWebviewCreationFailed,
                std::format(
                    "Creating the webview failed: {} (HRESULT: {:#010x})",
                    error->message, error->hr));
          }
          return shared_result->Error(kErrorCodeWebviewCreationFailed,
                                      "Creating the webview failed.");
        }

        auto bridge = std::make_unique<WebviewBridge>(
            messenger_, textures_, platform_->graphics_context(),
            std::move(webview));
        auto texture_id = bridge->texture_id();
        instances_[texture_id] = std::move(bridge);

        auto response = flutter::EncodableValue(flutter::EncodableMap{
            {flutter::EncodableValue("textureId"),
             flutter::EncodableValue(texture_id)},
        });

        shared_result->Success(response);
      });
}

bool WebviewWindowsPlugin::InitPlatform() {
  if (!platform_) {
    platform_ = std::make_unique<WebviewPlatform>();
  }
  return platform_->IsSupported();
}

}  // namespace

void WebviewWindowsPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  WebviewWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
