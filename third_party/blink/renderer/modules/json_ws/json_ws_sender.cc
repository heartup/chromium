// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/json_ws/json_ws_sender.h"

#include "base/memory/scoped_refptr.h"
#include "base/containers/span.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "net/cookies/site_for_cookies.h"
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

// Lazy initialization to avoid global constructor
namespace {
void EnsureFactoryRegistered() {
  static bool registered = false;
  if (!registered) {
    JSONWebSocketSenderRegistry::RegisterSenderFactory(
        &JSONWebSocketSender::CreateForContext);
    registered = true;
  }
}
}  // namespace

// static
JSONWebSocketSenderInterface* JSONWebSocketSender::CreateForContext(ExecutionContext* context) {
  EnsureFactoryRegistered();
  if (!context) {
    return nullptr;
  }
  return From(context);
}

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
      ExecutionContextLifecycleObserver(&context),
      websocket_(&context),
      handshake_receiver_(this, &context) {}

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

  // Send message via WebSocket
  std::string utf8_data = json_data.Utf8();
  websocket_->SendMessage(network::mojom::blink::WebSocketMessageType::TEXT, 
                          utf8_data.length());
  
  // Send the actual data through the data pipe
  SendDataThroughPipe(json_data);
}

bool JSONWebSocketSender::IsEnabled() const {
  return state_ == State::kConnected && websocket_.is_bound();
}

void JSONWebSocketSender::Shutdown() {
  if (websocket_.is_bound()) {
    websocket_.reset();
  }
  if (handshake_receiver_.is_bound()) {
    handshake_receiver_.reset();
  }
  data_pipe_producer_.reset();
  state_ = State::kDisconnected;
}

void JSONWebSocketSender::Trace(Visitor* visitor) const {
  visitor->Trace(websocket_);
  visitor->Trace(handshake_receiver_);
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
  mojo::Remote<blink::mojom::blink::WebSocketConnector> connector;
  context->GetBrowserInterfaceBroker().GetInterface(
      connector.BindNewPipeAndPassReceiver());

  // Create SiteForCookies - use an empty one for this purpose
  net::SiteForCookies site_for_cookies = net::SiteForCookies();

  // Connect to WebSocket
  connector->Connect(url, Vector<String>(), 
                     site_for_cookies,
                     String(), // user_agent
                     net::StorageAccessApiStatus::kNone,
                     handshake_receiver_.BindNewPipeAndPassRemote(
                         context->GetTaskRunner(TaskType::kNetworking)),
                     std::nullopt); // throttling_profile_id
}

void JSONWebSocketSender::OnOpeningHandshakeStarted(
    network::mojom::blink::WebSocketHandshakeRequestPtr request) {
  // Nothing special to do when handshake starts
}

void JSONWebSocketSender::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::blink::WebSocket> websocket,
    mojo::PendingReceiver<network::mojom::blink::WebSocketClient> client_receiver,
    network::mojom::blink::WebSocketHandshakeResponsePtr response,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  
  ExecutionContext* context = GetExecutionContext();
  if (!context) {
    return;
  }
  
  websocket_.Bind(std::move(websocket), 
                  context->GetTaskRunner(TaskType::kNetworking));
  data_pipe_producer_ = std::move(writable);
  state_ = State::kConnected;
  
  // Set up disconnect handler
  websocket_.set_disconnect_handler(WTF::BindOnce(
      &JSONWebSocketSender::OnWebSocketError, WrapWeakPersistent(this)));
}

void JSONWebSocketSender::OnFailure(const String& message,
                                   int32_t net_error,
                                   int32_t response_code) {
  state_ = State::kError;
  websocket_.reset();
  handshake_receiver_.reset();
  data_pipe_producer_.reset();
}

void JSONWebSocketSender::OnWebSocketError() {
  state_ = State::kError;
  websocket_.reset();
  handshake_receiver_.reset();
  data_pipe_producer_.reset();
}

void JSONWebSocketSender::SendDataThroughPipe(const String& data) {
  if (!data_pipe_producer_.is_valid()) {
    return;
  }

  std::string utf8_data = data.Utf8();
  base::span<const uint8_t> data_span = base::as_bytes(base::span(utf8_data));
  
  size_t actually_written_bytes = 0;
  MojoResult result = data_pipe_producer_->WriteData(
      data_span, MOJO_WRITE_DATA_FLAG_NONE, actually_written_bytes);
  
  if (result != MOJO_RESULT_OK) {
    // If write fails, we could queue the data for later retry, 
    // but for simplicity we just ignore the error here
  }
}

}  // namespace blink
