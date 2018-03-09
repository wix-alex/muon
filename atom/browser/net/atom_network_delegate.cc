// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/net/atom_network_delegate.h"

#include <memory>
#include <utility>

#include "atom/browser/extensions/tab_helper.h"
#include "atom/common/native_mate_converters/net_converter.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "chrome/browser/extensions/api/tabs/tabs_constants.h"
#include "content/network/throttling/throttling_network_transaction.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/websocket_handshake_request_info.h"
#include "extensions/features/features.h"
#include "net/url_request/url_request.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "extensions/browser/extension_api_frame_id_map.h"
#endif

using content::BrowserThread;

namespace atom {

const char* ResourceTypeToString(content::ResourceType type) {
  switch (type) {
    case content::RESOURCE_TYPE_MAIN_FRAME:
      return "mainFrame";
    case content::RESOURCE_TYPE_SUB_FRAME:
      return "subFrame";
    case content::RESOURCE_TYPE_STYLESHEET:
      return "stylesheet";
    case content::RESOURCE_TYPE_SCRIPT:
      return "script";
    case content::RESOURCE_TYPE_IMAGE:
      return "image";
    case content::RESOURCE_TYPE_OBJECT:
      return "object";
    case content::RESOURCE_TYPE_XHR:
      return "xhr";
    default:
      return "other";
  }
}

namespace {

struct ResponseHeadersContainer {
  scoped_refptr<net::HttpResponseHeaders>* headers;
  std::string status_line;
  GURL* new_url;

  ResponseHeadersContainer(scoped_refptr<net::HttpResponseHeaders>* headers,
            std::string status_line,
            GURL* new_url)
        : headers(headers), status_line(status_line), new_url(new_url) {}
};

int GetTabId(int frame_tree_node_id,
             int render_frame_id,
             int render_process_id) {
  auto web_contents =
      content::WebContents::FromFrameTreeNodeId(frame_tree_node_id);

  if (!web_contents) {
    content::RenderFrameHost* rfh =
        content::RenderFrameHost::FromID(render_process_id, render_frame_id);
    if (rfh)
      web_contents = content::WebContents::FromRenderFrameHost(rfh);
  }

  return extensions::TabHelper::IdForTab(web_contents);
}

void RunSimpleListener(const AtomNetworkDelegate::SimpleListener& listener,
                       std::unique_ptr<base::DictionaryValue> details,
                       int frame_tree_node_id,
                       int render_frame_id,
                       int render_process_id) {
  details->SetInteger(extensions::tabs_constants::kTabIdKey,
      GetTabId(frame_tree_node_id, render_frame_id, render_process_id));
  return listener.Run(*(details.get()));
}

void RunResponseListener(
    const AtomNetworkDelegate::ResponseListener& listener,
    std::unique_ptr<base::DictionaryValue> details,
    int frame_tree_node_id, int render_frame_id, int render_process_id,
    const AtomNetworkDelegate::ResponseCallback& callback) {
  details->SetInteger(extensions::tabs_constants::kTabIdKey,
      GetTabId(frame_tree_node_id, render_frame_id, render_process_id));
  return listener.Run(*(details.get()), callback);
}

// Test whether the URL of |request| matches |patterns|.
bool MatchesFilterCondition(net::URLRequest* request,
                            const URLPatterns& patterns) {
  if (patterns.empty())
    return true;

  for (const auto& pattern : patterns) {
    if (pattern.MatchesURL(request->url()))
      return true;
  }
  return false;
}

void GetRenderFrameIdAndProcessId(net::URLRequest* request,
    int* render_frame_id,
    int* render_process_id) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  *render_frame_id = -1;
  *render_process_id = -1;

  extensions::ExtensionApiFrameIdMap::FrameData frame_data;
  if (!content::ResourceRequestInfo::GetRenderFrameForRequest(
          request, render_process_id, render_frame_id)) {
    const content::WebSocketHandshakeRequestInfo* websocket_info =
      content::WebSocketHandshakeRequestInfo::ForRequest(request);
    if (websocket_info) {
      *render_frame_id = websocket_info->GetRenderFrameId();
      *render_process_id = websocket_info->GetChildId();
    }
  }
#endif
}

void GetFrameTreeNodeId(net::URLRequest* request, int* frame_tree_node_id) {
  auto request_info = content::ResourceRequestInfo::ForRequest(request);
  if (request_info)
    *frame_tree_node_id = request_info->GetFrameTreeNodeId();
}

// Overloaded by multiple types to fill the |details| object.
void ToDictionary(base::DictionaryValue* details, net::URLRequest* request) {
  FillRequestDetails(details, request);
  details->SetInteger("id", request->identifier());
  details->SetDouble("timestamp", base::Time::Now().ToDoubleT() * 1000);
  details->SetString("firstPartyUrl", request->site_for_cookies().spec());
  auto info = content::ResourceRequestInfo::ForRequest(request);
  details->SetString("resourceType",
                     info ? ResourceTypeToString(info->GetResourceType())
                          : "other");
  net::IPEndPoint request_ip_endpoint;
  bool was_successful = request->GetRemoteEndpoint(&request_ip_endpoint);
  if (was_successful) {
    details->SetString("ip", request_ip_endpoint.ToStringWithoutPort());
    details->SetInteger("port", request_ip_endpoint.port());
  }
}

void ToDictionary(base::DictionaryValue* details,
                  const net::HttpRequestHeaders& headers) {
  std::unique_ptr<base::DictionaryValue> dict(new base::DictionaryValue);
  net::HttpRequestHeaders::Iterator it(headers);
  while (it.GetNext())
    dict->SetKey(it.name(), base::Value(it.value()));
  details->Set("requestHeaders", std::move(dict));
}

void ToDictionary(base::DictionaryValue* details,
                  const net::HttpResponseHeaders* headers) {
  if (!headers)
    return;

  std::unique_ptr<base::DictionaryValue> dict(new base::DictionaryValue);
  size_t iter = 0;
  std::string key;
  std::string value;
  while (headers->EnumerateHeaderLines(&iter, &key, &value)) {
    if (dict->HasKey(key)) {
      base::ListValue* values = nullptr;
      if (dict->GetList(key, &values))
        values->AppendString(value);
    } else {
      std::unique_ptr<base::ListValue> values(new base::ListValue);
      values->AppendString(value);
      dict->Set(key, std::move(values));
    }
  }
  details->Set("responseHeaders", std::move(dict));
  details->SetString("statusLine", headers->GetStatusLine());
  details->SetInteger("statusCode", headers->response_code());
}

void ToDictionary(base::DictionaryValue* details, const GURL& location) {
  details->SetString("redirectURL", location.spec());
}

void ToDictionary(base::DictionaryValue* details,
                  const net::HostPortPair& host_port) {
  if (host_port.host().empty())
    details->SetString("ip", host_port.host());
}

void ToDictionary(base::DictionaryValue* details, bool from_cache) {
  details->SetBoolean("fromCache", from_cache);
}

void ToDictionary(base::DictionaryValue* details,
                  const net::URLRequestStatus& status) {
  details->SetString("error", net::ErrorToString(status.error()));
}

// Helper function to fill |details| with arbitrary |args|.
template<typename Arg>
void FillDetailsObject(base::DictionaryValue* details, Arg arg) {
  ToDictionary(details, arg);
}

template<typename Arg, typename... Args>
void FillDetailsObject(base::DictionaryValue* details, Arg arg, Args... args) {
  ToDictionary(details, arg);
  FillDetailsObject(details, args...);
}

// Fill the native types with the result from the response object.
void ReadFromResponseObject(const base::DictionaryValue& response,
                            GURL* new_location) {
  std::string url;
  if (response.GetString("redirectURL", &url))
    *new_location = GURL(url);
}

void ReadFromResponseObject(const base::DictionaryValue& response,
                            net::HttpRequestHeaders* headers) {
  const base::DictionaryValue* dict;
  if (response.GetDictionary("requestHeaders", &dict)) {
    headers->Clear();
    for (base::DictionaryValue::Iterator it(*dict);
         !it.IsAtEnd();
         it.Advance()) {
      std::string value;
      if (it.value().GetAsString(&value))
        headers->SetHeader(it.key(), value);
    }
  }
}

void ReadFromResponseObject(const base::DictionaryValue& response,
                            const ResponseHeadersContainer& container) {
  const base::DictionaryValue* dict;
  std::string status_line;
  if (!response.GetString("statusLine", &status_line))
    status_line = container.status_line;
  std::string url;
  if (response.GetString("redirectURL", &url))
    *container.new_url = GURL(url);
  if (response.GetDictionary("responseHeaders", &dict)) {
    auto headers = container.headers;
    *headers = new net::HttpResponseHeaders("");
    (*headers)->ReplaceStatusLine(status_line);
    for (base::DictionaryValue::Iterator it(*dict);
         !it.IsAtEnd();
         it.Advance()) {
      const base::ListValue* list;
      if (it.value().GetAsList(&list)) {
        (*headers)->RemoveHeader(it.key());
        for (size_t i = 0; i < list->GetSize(); ++i) {
          std::string value;
          if (list->GetString(i, &value))
            (*headers)->AddHeader(it.key() + " : " + value);
        }
      }
    }
  }
}

}  // namespace

AtomNetworkDelegate::AtomNetworkDelegate() : weak_factory_(this) {
}

AtomNetworkDelegate::~AtomNetworkDelegate() {
}

void AtomNetworkDelegate::SetSimpleListenerInIO(
    SimpleEvent type,
    const URLPatterns& patterns,
    const SimpleListener& callback) {
  if (callback.is_null())
    simple_listeners_.erase(type);
  else
    simple_listeners_[type] = { patterns, callback };
}

void AtomNetworkDelegate::SetResponseListenerInIO(
    ResponseEvent type,
    const URLPatterns& patterns,
    const ResponseListener& callback) {
  if (callback.is_null())
    response_listeners_.erase(type);
  else
    response_listeners_[type] = { patterns, callback };
}

void AtomNetworkDelegate::SetDevToolsNetworkEmulationClientId(
    const std::string& client_id) {
  base::AutoLock auto_lock(lock_);
  client_id_ = client_id;
}

int AtomNetworkDelegate::OnBeforeURLRequest(
    net::URLRequest* request,
    const net::CompletionCallback& callback,
    GURL* new_url) {
  if (!base::ContainsKey(response_listeners_, kOnBeforeRequest))
    return brightray::NetworkDelegate::OnBeforeURLRequest(
        request, callback, new_url);

  return HandleResponseEvent(kOnBeforeRequest, request, callback, new_url);
}

int AtomNetworkDelegate::OnBeforeStartTransaction(
    net::URLRequest* request,
    const net::CompletionCallback& callback,
    net::HttpRequestHeaders* headers) {
  std::string client_id;
  {
    base::AutoLock auto_lock(lock_);
    client_id = client_id_;
  }

  if (!client_id.empty())
    headers->SetHeader(content::ThrottlingNetworkTransaction::
                           kDevToolsEmulateNetworkConditionsClientId,
                       client_id);
  if (!base::ContainsKey(response_listeners_, kOnBeforeSendHeaders))
    return brightray::NetworkDelegate::OnBeforeStartTransaction(
        request, callback, headers);

  return HandleResponseEvent(
      kOnBeforeSendHeaders, request, callback, headers, *headers);
}

void AtomNetworkDelegate::OnStartTransaction(
    net::URLRequest* request,
    const net::HttpRequestHeaders& headers) {
  if (!base::ContainsKey(simple_listeners_, kOnSendHeaders)) {
    brightray::NetworkDelegate::OnStartTransaction(request, headers);
    return;
  }

  HandleSimpleEvent(kOnSendHeaders, request, headers);
}

int AtomNetworkDelegate::OnHeadersReceived(
    net::URLRequest* request,
    const net::CompletionCallback& callback,
    const net::HttpResponseHeaders* original,
    scoped_refptr<net::HttpResponseHeaders>* override,
    GURL* new_url) {
  if (!base::ContainsKey(response_listeners_, kOnHeadersReceived))
    return brightray::NetworkDelegate::OnHeadersReceived(
        request, callback, original, override, new_url);

  return HandleResponseEvent(
      kOnHeadersReceived, request, callback,
      ResponseHeadersContainer(override, original->GetStatusLine(), new_url),
      original);
}

void AtomNetworkDelegate::OnBeforeRedirect(net::URLRequest* request,
                                           const GURL& new_location) {
  if (!base::ContainsKey(simple_listeners_, kOnBeforeRedirect)) {
    brightray::NetworkDelegate::OnBeforeRedirect(request, new_location);
    return;
  }

  HandleSimpleEvent(kOnBeforeRedirect, request, new_location,
                    request->response_headers(), request->GetSocketAddress(),
                    request->was_cached());
}

void AtomNetworkDelegate::OnResponseStarted(net::URLRequest* request,
                                            int net_error) {
  if (!base::ContainsKey(simple_listeners_, kOnResponseStarted)) {
    brightray::NetworkDelegate::OnResponseStarted(request, net_error);
    return;
  }

  if (net_error != net::OK)
    return;

  HandleSimpleEvent(kOnResponseStarted, request, request->response_headers(),
                    request->was_cached());
}

void AtomNetworkDelegate::OnCompleted(net::URLRequest* request,
                                      bool started,
                                      int net_error) {
  // OnCompleted may happen before other events.
  callbacks_.erase(request->identifier());

  if (net_error != net::OK) {
    OnErrorOccurred(request, started, net_error);
    return;
  } else if (request->response_headers() &&
             net::HttpResponseHeaders::IsRedirectResponseCode(
                 request->response_headers()->response_code())) {
    // Redirect event.
    brightray::NetworkDelegate::OnCompleted(request, started, net_error);
    return;
  }

  if (!base::ContainsKey(simple_listeners_, kOnCompleted)) {
    brightray::NetworkDelegate::OnCompleted(request, started, net_error);
    return;
  }

  HandleSimpleEvent(kOnCompleted, request, request->response_headers(),
                    request->was_cached());
}

void AtomNetworkDelegate::OnURLRequestDestroyed(net::URLRequest* request) {
  callbacks_.erase(request->identifier());
}

void AtomNetworkDelegate::OnErrorOccurred(
    net::URLRequest* request, bool started, int net_error) {
  if (!base::ContainsKey(simple_listeners_, kOnErrorOccurred)) {
    brightray::NetworkDelegate::OnCompleted(request, started, net_error);
    return;
  }

  HandleSimpleEvent(kOnErrorOccurred, request, request->was_cached(),
                    request->status());
}

template<typename Out, typename... Args>
int AtomNetworkDelegate::HandleResponseEvent(
    ResponseEvent type,
    net::URLRequest* request,
    const net::CompletionCallback& callback,
    Out out,
    Args... args) {
  const auto& info = response_listeners_[type];
  if (!MatchesFilterCondition(request, info.url_patterns))
    return net::OK;

  std::unique_ptr<base::DictionaryValue> details(new base::DictionaryValue);
  FillDetailsObject(details.get(), request, args...);

  // The |request| could be destroyed before the |callback| is called.
  callbacks_[request->identifier()] = callback;

  int frame_tree_node_id = -1;
  GetFrameTreeNodeId(request, &frame_tree_node_id);

  int render_frame_id = -1;
  int render_process_id = -1;
  GetRenderFrameIdAndProcessId(request, &render_frame_id, &render_process_id);

  ResponseCallback response =
      base::Bind(&AtomNetworkDelegate::OnListenerResultInUI<Out>,
                 weak_factory_.GetWeakPtr(), request->identifier(), out);
  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(RunResponseListener, info.listener, base::Passed(&details),
                 frame_tree_node_id, render_frame_id, render_process_id,
                 response));
  return net::ERR_IO_PENDING;
}

template<typename...Args>
void AtomNetworkDelegate::HandleSimpleEvent(
    SimpleEvent type, net::URLRequest* request, Args... args) {
  const auto& info = simple_listeners_[type];
  if (!MatchesFilterCondition(request, info.url_patterns))
    return;

  std::unique_ptr<base::DictionaryValue> details(new base::DictionaryValue);
  FillDetailsObject(details.get(), request, args...);

  int frame_tree_node_id = -1;
  GetFrameTreeNodeId(request, &frame_tree_node_id);

  int render_frame_id = -1;
  int render_process_id = -1;
  GetRenderFrameIdAndProcessId(request, &render_frame_id, &render_process_id);

  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      base::Bind(RunSimpleListener, info.listener, base::Passed(&details),
          frame_tree_node_id, render_frame_id, render_process_id));
}

template<typename T>
void AtomNetworkDelegate::OnListenerResultInIO(
    uint64_t id, T out, std::unique_ptr<base::DictionaryValue> response) {
  // The request has been destroyed.
  if (!base::ContainsKey(callbacks_, id))
    return;

  ReadFromResponseObject(*response.get(), out);

  bool cancel = false;
  response->GetBoolean("cancel", &cancel);
  callbacks_[id].Run(cancel ? net::ERR_ABORTED : net::OK);
}

template<typename T>
void AtomNetworkDelegate::OnListenerResultInUI(
    uint64_t id,
    T out, const base::DictionaryValue& response) {
  std::unique_ptr<base::DictionaryValue> copy = response.CreateDeepCopy();
  BrowserThread::PostTask(
      BrowserThread::IO, FROM_HERE,
      base::Bind(&AtomNetworkDelegate::OnListenerResultInIO<T>,
                 weak_factory_.GetWeakPtr(), id, out, base::Passed(&copy)));
}

}  // namespace atom
