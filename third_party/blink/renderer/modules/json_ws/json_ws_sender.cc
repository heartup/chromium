// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/json_ws/json_ws_sender.h"

#include "base/memory/scoped_refptr.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/websocket.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "url/gurl.h"

namespace blink {

const char JSONWebSocketSender::kSupplementName[] = "JSONWebSocketSender";

// static
JSONWebSocketSender* JSONWebSocketSender::From(ExecutionContext* context) {
  if (!context) {
    return nullptr;
  }

  JSONWebSocketSender* sender =
      Supplement<ExecutionContext>::From<JSONWebSocketSender>(context);
  if (!sender) {
    sender = MakeGarbageCollected<JSONWebSocketSender>(*context);
    ProvideTo(*context, sender);
  }
  return sender;
}

JSONWebSocketSender::JSONWebSocketSender(ExecutionContext& context)
    : Supplement<ExecutionContext>(context),
      ExecutionContextLifecycleObserver(&context) {}

JSONWebSocketSender::~JSONWebSocketSender() = default;

void JSONWebSocketSender::Initialize(const String& websocket_url) {
  if (state_ != State::kDisconnected) {
    return;
  }

  websocket_url_ = websocket_url;
  ConnectWebSocket(KURL(websocket_url));
}

void JSONWebSocketSender::Send(const String& json_data) {
  if (state_ != State::kConnected || !websocket_.is_bound()) {
    return;
  }

  // Create WebSocket frame with the JSON data
  auto message = network::mojom::blink::WebSocketMessage::New();
  message->type = network::mojom::blink::WebSocketMessageType::TEXT;
  message->data = json_data.Utf8();
  
  websocket_->SendMessage(std::move(message));
}

bool JSONWebSocketSender::IsEnabled() const {
  return state_ == State::kConnected && websocket_.is_bound();
}

void JSONWebSocketSender::Shutdown() {
  if (websocket_.is_bound()) {
    websocket_.reset();
  }
  if (client_receiver_.is_bound()) {
    client_receiver_.reset();
  }
  state_ = State::kDisconnected;
}

void JSONWebSocketSender::Trace(Visitor* visitor) const {
  Supplement<ExecutionContext>::Trace(visitor);
  ExecutionContextLifecycleObserver::Trace(visitor);
}

void JSONWebSocketSender::ContextDestroyed() {
  Shutdown();
}

void JSONWebSocketSender::ConnectWebSocket(const KURL& url) {
  ExecutionContext* context = GetExecutionContext();
  if (!context) {
    return;
  }

  state_ = State::kConnecting;

  // Get WebSocketConnector from browser
  mojo::Remote<network::mojom::blink::WebSocketConnector> connector;
  context->GetBrowserInterfaceBroker().GetInterface(
      connector.BindNewPipeAndPassReceiver());

  // Prepare WebSocket connection request
  auto request = network::mojom::blink::WebSocketHandshakeRequest::New();
  request->url = url;
  
  // Add basic headers
  Vector<network::mojom::blink::HttpHeaderPtr> headers;
  auto origin_header = network::mojom::blink::HttpHeader::New();
  origin_header->name = "Origin";
  origin_header->value = context->GetSecurityOrigin()->ToString();
  headers.push_back(std::move(origin_header));
  
  request->headers = std::move(headers);

  // Connect to WebSocket
  connector->Connect(url, Vector<String>(), std::move(request),
                     mojo::NullRemote(),
                     mojo::PendingReceiver<network::mojom::blink::WebSocketHandshakeClient>(
                         client_receiver_.BindNewPipeAndPassReceiver()));
}

void JSONWebSocketSender::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::blink::WebSocket> websocket,
    mojo::PendingReceiver<network::mojom::blink::WebSocketClient> client_receiver,
    network::mojom::blink::WebSocketHandshakeResponsePtr response,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  
  websocket_.Bind(std::move(websocket));
  state_ = State::kConnected;
  
  // Set up disconnect handler
  websocket_.set_disconnect_handler(WTF::BindOnce(
      &JSONWebSocketSender::OnWebSocketError, WrapWeakPersistent(this)));
}

void JSONWebSocketSender::OnFailure(const String& message,
                                   uint16_t code,
                                   const String& reason) {
  state_ = State::kError;
  websocket_.reset();
  client_receiver_.reset();
}

void JSONWebSocketSender::OnWebSocketConnected() {
  state_ = State::kConnected;
}

void JSONWebSocketSender::OnWebSocketError() {
  state_ = State::kError;
  websocket_.reset();
  client_receiver_.reset();
}

}  // namespace blink
