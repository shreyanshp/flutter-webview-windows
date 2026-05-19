#pragma once

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <atomic>
#include <memory>

#include "graphics_context.h"
#include "texture_bridge.h"
#include "webview.h"

class WebviewBridge {
 public:
  WebviewBridge(flutter::BinaryMessenger* messenger,
                flutter::TextureRegistrar* texture_registrar,
                GraphicsContext* graphics_context,
                std::unique_ptr<Webview> webview);
  ~WebviewBridge();

  TextureBridge* texture_bridge() const { return texture_bridge_.get(); }

  int64_t texture_id() const { return texture_id_; }

  // Shared "alive" flag. Captured by-value (shared_ptr) into every
  // lambda this bridge registers with the engine's messenger,
  // texture registrar, and WebView2 event sources. ~WebviewBridge
  // stores `false` here BEFORE freeing any bridge state. Late-firing
  // callbacks (Win32 message queue, WebView2 worker threads) atomic-
  // load the flag, see false, and no-op — they never deref freed
  // `this`. Replaces the earlier pattern of calling
  // SetMethodCallHandler(nullptr) / SetStreamHandler(nullptr) in
  // the destructor, which itself AVs inside
  // FlutterDesktopMessengerSetCallback at offset 0x1e8 when the
  // engine has already invalidated the messenger by the time the
  // bridge destructs (Sentry MOBILE-NEWS-CN, 2026-05).
  std::shared_ptr<std::atomic<bool>> alive() const { return alive_; }

 private:
  std::shared_ptr<std::atomic<bool>> alive_;
  std::unique_ptr<flutter::TextureVariant> flutter_texture_;
  std::unique_ptr<TextureBridge> texture_bridge_;
  std::unique_ptr<Webview> webview_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;

  flutter::TextureRegistrar* texture_registrar_;
  int64_t texture_id_;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void RegisterEventHandlers();

  template <typename T>
  void EmitEvent(const T& value) {
    // Late callback from a WebView2 worker thread after the bridge
    // started teardown — drop on the floor instead of dispatching
    // through a potentially-invalid event_sink_.
    if (!alive_->load(std::memory_order_acquire)) return;
    if (event_sink_) {
      event_sink_->Success(value);
    }
  }

  void OnPermissionRequested(
      const std::string& url, WebviewPermissionKind permissionKind,
      bool is_user_initiated,
      Webview::WebviewPermissionRequestedCompleter completer);
};
