// Copyright (c) 2015 Klaralvdalens Datakonsult AB (KDAB).
// All rights reserved. Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "handler.h"

#include <sstream>
#include <string>
#include <iostream>
#include <locale>
#include <algorithm>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPageSize>
#include <QRect>
#include <QImage>
#include <QBuffer>
#include <QImageWriter>
#include <QDateTime>
#include <QFileInfo>
#include <QMessageLogger>
#include <QUrl>

#include "include/base/cef_bind.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "print_handler.h"
#include "debug.h"

#include "WindowsKeyboardCodes.h"
#include "keyevents.h"

namespace {

/**
 * Workaround an issue where the frame's IsMain property is not updated to
 * reflect the actual main frame set in the browser.
 */
bool isMain(const CefRefPtr<CefFrame>& frame)
{
  return frame->IsMain() || frame->GetIdentifier() == frame->GetBrowser()->GetMainFrame()->GetIdentifier();
}

template<typename T, typename K>
T takeCallback(QHash<K, T>* callbacks, K key)
{
  auto it = callbacks->find(key);
  if (it != callbacks->end()) {
    auto ret = it.value();
    callbacks->erase(it);
    return ret;
  }
  return {};
}

template<typename T>
T takeCallback(QHash<int32, T>* callbacks, const CefRefPtr<CefBrowser>& browser)
{
  return takeCallback(callbacks, browser->GetIdentifier());
}

void initWindowInfo(CefWindowInfo& window_info, bool isPhantomMain)
{
#if defined(OS_WIN)
  // On Windows we need to specify certain flags that will be passed to
  // CreateWindowEx().
  window_info.SetAsPopup(NULL, "phantomjs");
#endif
  if (isPhantomMain || !qEnvironmentVariableIsSet("PHANTOMJS_CEF_SHOW_WINDOW")) {
    window_info.SetAsWindowless(0, true);
  }
}

cef_state_t toState(const QJsonValue& value)
{
  if (value.isBool()) {
    return value.toBool() ? STATE_ENABLED : STATE_DISABLED;
  } else if (value.isString()) {
    const auto stringValue = value.toString();
    if (!stringValue.compare(QLatin1String("on"), Qt::CaseInsensitive)
        || stringValue.compare(QLatin1String("yes"), Qt::CaseInsensitive))
    {
      return STATE_ENABLED;
    }
  }

  return STATE_DEFAULT;
}

void initBrowserSettings(CefBrowserSettings& browser_settings, bool isPhantomMain,
                         const QJsonObject& config)
{
  // TODO: make this configurable
  if (isPhantomMain) {
    browser_settings.web_security = STATE_DISABLED;
    browser_settings.universal_access_from_file_urls = STATE_ENABLED;
    browser_settings.file_access_from_file_urls = STATE_ENABLED;
  } else {
    browser_settings.web_security = toState(config.value(QStringLiteral("webSecurityEnabled")));
    browser_settings.universal_access_from_file_urls = toState(config.value(QStringLiteral("localToRemoteUrlAccessEnabled")));
    browser_settings.image_loading = toState(config.value(QStringLiteral("loadImages")));
    browser_settings.javascript = toState(config.value(QStringLiteral("javascriptEnabled")));
    browser_settings.javascript_open_windows = toState(config.value(QStringLiteral("javascriptOpenWindows")));
    browser_settings.javascript_close_windows = toState(config.value(QStringLiteral("javascriptCloseWindows")));
    /// TODO: extend
  }
}

#if CHROME_VERSION_BUILD >= 2526
const bool PRINT_SETTINGS = false;

void printValue(const CefString& key, const CefRefPtr<CefValue>& value) {
  switch (value->GetType()) {
    case VTYPE_INVALID:
      qDebug() << key << "invalid";
      break;
    case VTYPE_NULL:
      qDebug() << key << "null";
      break;
    case VTYPE_BOOL:
      qDebug() << key << value->GetBool();
      break;
    case VTYPE_INT:
      qDebug() << key << value->GetInt();
      break;
    case VTYPE_DOUBLE:
      qDebug() << key << value->GetDouble();
      break;
    case VTYPE_STRING:
      qDebug() << key << value->GetString();
      break;
    case VTYPE_BINARY:
      qDebug() << key << "binary";
      break;
    case VTYPE_DICTIONARY: {
      qDebug() << key << "dictionary";
      auto dict = value->GetDictionary();
      CefDictionaryValue::KeyList keys;
      dict->GetKeys(keys);
      for (const auto& subKey : keys) {
        printValue(key.ToString() + "." + subKey.ToString(), dict->GetValue(subKey));
      }
      break;
    }
    case VTYPE_LIST:
      qDebug() << key << "list";
      auto list = value->GetList();
      for (int i = 0; i < static_cast<int>(list->GetSize()); ++i) {
        printValue(key.ToString() + "[" + std::to_string(i) + "]", list->GetValue(i));
      }
      break;
  }
}
#endif

QJsonObject toJson(const CefRefPtr<CefDownloadItem>& item)
{
  QJsonObject object;
  object[QStringLiteral("isInProgress")] = item->IsInProgress();
  object[QStringLiteral("isComplete")] = item->IsComplete();
  object[QStringLiteral("isCanceled")] = item->IsCanceled();
  if (!item->GetSuggestedFileName().empty()) {
    object[QStringLiteral("suggestedFileName")] = QString::fromStdString(item->GetSuggestedFileName());
  }
  object[QStringLiteral("fullPath")] = QString::fromStdString(item->GetFullPath());
  object[QStringLiteral("url")] = QString::fromStdString(item->GetURL());
  object[QStringLiteral("originalUrl")] = QString::fromStdString(item->GetOriginalUrl());
  object[QStringLiteral("mimeType")] = QString::fromStdString(item->GetMimeType());
  object[QStringLiteral("contentDisposition")] = QString::fromStdString(item->GetContentDisposition());
  object[QStringLiteral("totalBytes")] = static_cast<qint64>(item->GetTotalBytes());
  object[QStringLiteral("receivedBytes")] = static_cast<qint64>(item->GetReceivedBytes());
  object[QStringLiteral("currentSpeed")] = static_cast<qint64>(item->GetCurrentSpeed());
  object[QStringLiteral("percentComplete")] = item->GetPercentComplete();
  object[QStringLiteral("startTime")] = QDateTime::fromTime_t(item->GetStartTime().GetTimeT()).toUTC().toString(Qt::ISODate);
  if (item->GetEndTime().GetTimeT()) {
    // this was always invalid in my tests
    object[QStringLiteral("endTime")] = QDateTime::fromTime_t(item->GetEndTime().GetTimeT()).toUTC().toString(Qt::ISODate);
  }
  return object;
}
}

PhantomJSHandler::PhantomJSHandler()
    : m_messageRouter(CefMessageRouterBrowserSide::Create(messageRouterConfig()))
{
  m_messageRouter->AddHandler(this, false);
}

PhantomJSHandler::~PhantomJSHandler()
{
}

CefMessageRouterConfig PhantomJSHandler::messageRouterConfig()
{
  CefMessageRouterConfig config;
  config.js_cancel_function = "cancelPhantomJsQuery";
  config.js_query_function = "startPhantomJsQuery";
  return config;
}

CefRefPtr<CefBrowser> PhantomJSHandler::createBrowser(const CefString& url, bool isPhantomMain,
                                                      const QJsonObject& config)
{
  CefWindowInfo window_info;
  initWindowInfo(window_info, isPhantomMain);

  CefBrowserSettings browser_settings;
  initBrowserSettings(browser_settings, isPhantomMain, config);

  qCDebug(handler) << url << isPhantomMain << config;

  return CefBrowserHost::CreateBrowserSync(window_info, this, url, browser_settings,
                                           NULL);
}

bool PhantomJSHandler::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                                CefProcessId source_process,
                                                CefRefPtr<CefProcessMessage> message)
{
  if (m_messageRouter->OnProcessMessageReceived(browser, source_process, message)) {
    return true;
  }
  if (message->GetName() == "exit") {
    CloseAllBrowsers(true);
    return true;
  }
  return false;
}

bool PhantomJSHandler::OnConsoleMessage(CefRefPtr<CefBrowser> browser, const CefString& message, const CefString& source, int line)
{
  if (!canEmitSignal(browser)) {
    auto shortSource = QFileInfo(QString::fromStdString(source)).fileName().toStdString();
    QMessageLogger(shortSource.c_str(), line, 0).debug() << message;
  } else {
    emitSignal(browser, QStringLiteral("onConsoleMessage"),
        {QString::fromStdString(message), QString::fromStdString(source), line});
  }
  return true;
}

void PhantomJSHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  qCDebug(handler) << browser->GetIdentifier();

  auto& browserInfo = m_browsers[browser->GetIdentifier()];
  browserInfo.browser = browser;
  if (!m_popupToParentMapping.isEmpty()) {
    auto parentBrowser = m_popupToParentMapping.dequeue();
    // we don't open about:blank for popups
    browserInfo.firstLoadFinished = true;
    emitSignal(m_browsers.value(parentBrowser).browser, QStringLiteral("onPopupCreated"),
               {browser->GetIdentifier()}, true);
  }

#if CHROME_VERSION_BUILD >= 2526
  if (PRINT_SETTINGS) {
    auto prefs = browser->GetHost()->GetRequestContext()->GetAllPreferences(true);
    CefDictionaryValue::KeyList keys;
    prefs->GetKeys(keys);
    for (const auto& key : keys) {
      printValue(key, prefs->GetValue(key));
    }
  }
#endif
}

bool PhantomJSHandler::DoClose(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  // Allow the close. For windowed browsers this will result in the OS close
  // event being sent.
  return false;
}

void PhantomJSHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser)
{
  CEF_REQUIRE_UI_THREAD();

  qCDebug(handler) << browser->GetIdentifier();

  m_messageRouter->OnBeforeClose(browser);

  m_browsers.remove(browser->GetIdentifier());

  if (m_browsers.empty()) {
    // All browser windows have closed. Quit the application message loop.
    CefQuitMessageLoop();
  }
}

bool PhantomJSHandler::OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                     const CefString& target_url, const CefString& target_frame_name,
                                     CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture,
                                     const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
                                     CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
                                     bool* no_javascript_access)
{
  qCDebug(handler) << browser->GetIdentifier() << frame->GetURL() << target_url << target_frame_name;
  // TODO: inherit settings? manipulate?
  initWindowInfo(windowInfo, false);
  initBrowserSettings(settings, false, {});
  client = this;
  // TODO: is it enough to assume the next browser that will be created belongs to this popup request?
  m_popupToParentMapping << browser->GetIdentifier();
  return false;
}

void PhantomJSHandler::OnLoadError(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                ErrorCode errorCode,
                                const CefString& errorText,
                                const CefString& failedUrl)
{
  CEF_REQUIRE_UI_THREAD();

  qCDebug(handler) << browser->GetIdentifier() << isMain(frame) << errorCode << errorText << failedUrl;

  if (isMain(frame)) {
    handleLoadEnd(browser, errorCode, failedUrl, false);
  }

  // Don't display an error for downloaded files.
  if (errorCode == ERR_ABORTED)
    return;

  // Display a load error message.
  std::stringstream ss;
  ss << "<html><body bgcolor=\"white\">"
        "<h2>Failed to load URL " << std::string(failedUrl) <<
        " with error " << std::string(errorText) << " (" << errorCode <<
        ").</h2></body></html>";
  frame->LoadString(ss.str(), failedUrl);
}

void PhantomJSHandler::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame)
{
  CEF_REQUIRE_UI_THREAD();

  qCDebug(handler) << browser->GetIdentifier() << frame->GetURL() << isMain(frame);

  // filter out events from sub frames
  if (!isMain(frame) || !canEmitSignal(browser) || !m_browsers.value(browser->GetIdentifier()).firstLoadFinished) {
    return;
  }

  emitSignal(browser, QStringLiteral("onLoadStarted"), {QString::fromStdString(frame->GetURL())});
}

void PhantomJSHandler::emitSignal(const CefRefPtr<CefBrowser>& browser, const QString& signal,
                                  const QJsonArray& arguments, bool internal)
{
  auto callback = m_browsers.value(browser->GetIdentifier()).signalCallback;
  if (!callback) {
    qDebug() << "no signal callback for browser" << browser->GetIdentifier() << signal;
    return;
  }
  QJsonObject data = {
    {QStringLiteral("signal"), signal},
    {QStringLiteral("args"), arguments}
  };
  if (internal) {
    data[QStringLiteral("internal")] = true;
  }
  callback->Success(QJsonDocument(data).toJson().constData());
}

bool PhantomJSHandler::canEmitSignal(const CefRefPtr<CefBrowser>& browser) const
{
  return m_browsers.value(browser->GetIdentifier()).signalCallback;
}

void PhantomJSHandler::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode)
{
  CEF_REQUIRE_UI_THREAD();

  qCDebug(handler) << browser->GetIdentifier() << frame->GetURL() << isMain(frame) << httpStatusCode;

  // filter out events from sub frames
  if (!isMain(frame)) {
    return;
  }

  /// TODO: is this OK?
  const bool success = httpStatusCode < 400;
  handleLoadEnd(browser, httpStatusCode, frame->GetURL(), success);
}

void PhantomJSHandler::OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading, bool canGoBack, bool canGoForward)
{
  qCDebug(handler) << browser->GetIdentifier() << isLoading;
}

void PhantomJSHandler::handleLoadEnd(CefRefPtr<CefBrowser> browser, int statusCode, const CefString& url, bool success)
{
  qCDebug(handler) << browser->GetIdentifier() << statusCode << url << success;

  auto& browserInfo = m_browsers[browser->GetIdentifier()];
  if (!browserInfo.firstLoadFinished) {
    browserInfo.firstLoadFinished = true;
    return;
  }

  if (canEmitSignal(browser)) {
    emitSignal(browser, QStringLiteral("onLoadEnd"), {QString::fromStdString(url), success}, true);
  }

  while (auto callback = takeCallback(&m_waitForLoadedCallbacks, browser)) {
    if (success) {
      callback->Success(std::to_string(statusCode));
    } else {
      callback->Failure(statusCode, "Failed to load URL: " + url.ToString());
    }
  }
}

bool PhantomJSHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
{
  qCDebug(handler) << browser->GetIdentifier() << m_viewRects.value(browser->GetIdentifier());
  const auto size = m_viewRects.value(browser->GetIdentifier(), qMakePair(800, 600));
  rect.Set(0, 0, size.first, size.second);
  return true;
}

void PhantomJSHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height)
{
  qCDebug(handler) << browser->GetIdentifier() << type << width << height;

  if (!canEmitSignal(browser)) {
    return;
  }

  auto info = takeCallback(&m_paintCallbacks, browser);
  if (info.callback) {
    QImage image(reinterpret_cast<const uchar*>(buffer), width, height, QImage::Format_ARGB32);
    if (info.clipRect.isValid()) {
      image = image.copy(info.clipRect);
    }
    if (info.format.isEmpty()) {
      if (image.save(info.path)) {
        info.callback->Success({});
      } else {
        info.callback->Failure(1, QStringLiteral("Failed to render page to \"%1\".").arg(info.path).toStdString());
      }
    } else {
      QByteArray ba;
      QBuffer buffer(&ba);
      buffer.open(QIODevice::WriteOnly);
      if (image.save(&buffer, info.format.toUtf8().constData())) {
        const auto data = ba.toBase64();
        info.callback->Success(std::string(data.constData(), data.size()));
      } else {
        std::string error = "Failed to render page into Base64 encoded buffer of format \"";
        error += qPrintable(info.format);
        error += "\". Available formats are: ";
        bool first = true;
        foreach (const auto& format, QImageWriter::supportedImageFormats()) {
          if (!first) {
            error += ", ";
          }
          error += std::string(format);
          first = false;
        }
        info.callback->Failure(1, error);
      }
    }
  }

  QJsonArray jsonDirtyRects;
  for (const auto& rect : dirtyRects) {
    QJsonObject jsonRect = {
      {QStringLiteral("x"), rect.x},
      {QStringLiteral("y"), rect.y},
      {QStringLiteral("width"), rect.width},
      {QStringLiteral("height"), rect.height}
    };
    jsonDirtyRects.push_back(jsonRect);
  }

  emitSignal(browser, QStringLiteral("onPaint"), {jsonDirtyRects, width, height, type});
}

void PhantomJSHandler::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus status)
{
  m_messageRouter->OnRenderProcessTerminated(browser);
}

bool PhantomJSHandler::OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool is_redirect)
{
  m_messageRouter->OnBeforeBrowse(browser, frame);
  return false;
}

template<typename T>
QJsonObject headerMapToJson(const CefRefPtr<T>& r)
{
  QJsonObject jsonHeaders;
  CefRequest::HeaderMap headers;
  r->GetHeaderMap(headers);
  for (const auto& header : headers) {
    jsonHeaders[QString::fromStdString(header.first)] = QString::fromStdString(header.second);
  }
  return jsonHeaders;
}

CefRequestHandler::ReturnValue PhantomJSHandler::OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                                   CefRefPtr<CefRequest> request, CefRefPtr<CefRequestCallback> callback)
{
  if (!canEmitSignal(browser)) {
    return RV_CONTINUE;
  }

  qCDebug(handler) << browser->GetIdentifier() << frame->GetURL() << request->GetURL();

  QJsonArray jsonPost;
  if (const auto post = request->GetPostData()) {
    CefPostData::ElementVector elements;
    post->GetElements(elements);
    for (const auto& element : elements) {
      QJsonObject elementJson = {{QStringLiteral("type"), element->GetType()}};
      switch (element->GetType()) {
        case PDE_TYPE_BYTES: {
          QByteArray bytes;
          bytes.resize(static_cast<int>(element->GetBytesCount()));
          element->GetBytes(bytes.size(), bytes.data());
          const auto STRING_BYTES = QStringLiteral("bytes");
          elementJson[STRING_BYTES] = QString::fromUtf8(bytes.toBase64());
          break;
        }
        case PDE_TYPE_FILE: {
          const auto STRING_FILE = QStringLiteral("file");
          elementJson[STRING_FILE] = QString::fromStdString(element->GetFile().ToString());
          break;
        }
        case PDE_TYPE_EMPTY:
          break;
      }
      jsonPost.append(elementJson);
    }
  }

  QJsonObject jsonRequest;
  jsonRequest[QStringLiteral("headers")] = headerMapToJson(request);
  jsonRequest[QStringLiteral("post")] = jsonPost;
  jsonRequest[QStringLiteral("url")] = QString::fromStdString(request->GetURL().ToString());
  jsonRequest[QStringLiteral("method")] = QString::fromStdString(request->GetMethod().ToString());
  jsonRequest[QStringLiteral("flags")] = request->GetFlags();
  jsonRequest[QStringLiteral("resourceType")] = static_cast<int>(request->GetResourceType());
  jsonRequest[QStringLiteral("transitionType")] = static_cast<int>(request->GetTransitionType());

  m_requestCallbacks[request->GetIdentifier()] = {request, callback};

  emitSignal(browser, QStringLiteral("onBeforeResourceLoad"),
             {jsonRequest, QString::number(request->GetIdentifier())}, true);

  return RV_CONTINUE_ASYNC;
}

bool PhantomJSHandler::OnResourceResponse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                          CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response)
{
  if (canEmitSignal(browser)) {
    QJsonObject jsonResponse;
    jsonResponse[QStringLiteral("status")] = response->GetStatus();
    jsonResponse[QStringLiteral("statusText")] = QString::fromStdString(response->GetStatusText());
    jsonResponse[QStringLiteral("contentType")] = QString::fromStdString(response->GetMimeType());
    jsonResponse[QStringLiteral("headers")] = headerMapToJson(response);
    jsonResponse[QStringLiteral("url")] = QString::fromStdString(request->GetURL());
    jsonResponse[QStringLiteral("id")] = QString::number(request->GetIdentifier());
    /// TODO: time, stage, bodySize, redirectUrl
    emitSignal(browser, QStringLiteral("onResourceReceived"), {jsonResponse});
  }
  return false;
}

bool PhantomJSHandler::GetAuthCredentials(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                          bool isProxy, const CefString& host, int port, const CefString& realm, const CefString& scheme,
                                          CefRefPtr<CefAuthCallback> callback)
{
  const auto& info = m_browsers.value(browser->GetIdentifier());
  if (info.authName.empty() || info.authPassword.empty()) {
    return false;
  }
  // TODO: this old PhantomJS API is really bad, we should rather delegate that to the script and delay the callback execution
  callback->Continue(info.authName, info.authPassword);
  return true;
}

void PhantomJSHandler::CloseAllBrowsers(bool force_close)
{
  qCDebug(handler) << force_close;

  m_messageRouter->CancelPending(nullptr, nullptr);

  if (!CefCurrentlyOn(TID_UI)) {
    // Execute on the UI thread.
    CefPostTask(TID_UI,
        base::Bind(&PhantomJSHandler::CloseAllBrowsers, this, force_close));
    return;
  }

  // iterate over list of values to ensure we really close all browsers
  foreach (const auto& info, m_browsers.values()) {
    info.browser->GetHost()->CloseBrowser(force_close);
  }
}

bool PhantomJSHandler::OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                               int64 query_id, const CefString& request, bool persistent,
                               CefRefPtr<Callback> callback)
{
  CEF_REQUIRE_UI_THREAD();

  const auto data = QByteArray(request.ToString().data());

  QJsonParseError error;
  const auto json = QJsonDocument::fromJson(data, &error).object();
  qCDebug(handler) << browser->GetIdentifier() << frame->GetURL() << json;
  if (error.error) {
    qCWarning(handler) << error.errorString();
    return false;
  }

  const auto type = json.value(QStringLiteral("type")).toString();

  if (type == QLatin1String("createBrowser")) {
    const auto& settings = json.value(QStringLiteral("settings")).toObject();
    auto subBrowser = createBrowser("about:blank", false, settings);
    auto& info = m_browsers[subBrowser->GetIdentifier()];
    info.authName = settings.value(QStringLiteral("userName")).toString().toStdString();
    info.authPassword = settings.value(QStringLiteral("password")).toString().toStdString();
    callback->Success(std::to_string(subBrowser->GetIdentifier()));
    return true;
  } else if (type == QLatin1String("returnEvaluateJavaScript")) {
    auto otherQueryId = json.value(QStringLiteral("queryId")).toInt(-1);
    auto it = m_pendingQueryCallbacks.find(otherQueryId);
    if (it != m_pendingQueryCallbacks.end()) {
      auto exception = json.value(QStringLiteral("exception"));
      auto otherCallback = it.value();
      if (!exception.isUndefined()) {
        otherCallback->Failure(1, exception.toString().toStdString());
      } else {
        auto retval = json.value(QStringLiteral("retval")).toString();
        otherCallback->Success(retval.toStdString());
      }
      m_pendingQueryCallbacks.erase(it);
      callback->Success({});
      return true;
    }
  } else if (type == QLatin1String("beforeResourceLoadResponse")) {
    const auto requestId = static_cast<uint64>(json.value(QStringLiteral("requestId")).toString().toULongLong());
    auto callback = takeCallback(&m_requestCallbacks, requestId);
    if (!callback.callback || !callback.request) {
      qCWarning(handler) << "Unknown request with id" << requestId << "for query" << json;
      return false;
    }
    const auto allow = json.value(QStringLiteral("allow")).toBool();
    if (!allow) {
      callback.callback->Continue(allow);
      return true;
    }
    auto requestData = json.value(QStringLiteral("request")).toObject();
    callback.request->SetURL(requestData.value(QStringLiteral("url")).toString().toStdString());
    CefRequest::HeaderMap headers;
    const auto& jsonHeaders = requestData.value(QStringLiteral("headers")).toObject();
    for (auto it = jsonHeaders.begin(); it != jsonHeaders.end(); ++it) {
      headers.insert(std::make_pair(it.key().toStdString(), it.value().toString().toStdString()));
    }
    callback.request->SetHeaderMap(headers);
    // TODO: post support
    callback.callback->Continue(true);
    return true;
  } else if (type == QLatin1String("beforeDownloadResponse")) {
    const auto requestId = static_cast<uint64>(json.value(QStringLiteral("requestId")).toString().toULongLong());
    const auto target = json.value(QStringLiteral("target")).toString().toStdString();
    auto callback = m_beforeDownloadCallbacks.take(requestId);
    if (!callback) {
      qCWarning(handler) << "Unknown request with id" << requestId << "for query" << json;
      return false;
    }
    callback->Continue(target, false);
    return true;
  } else if (type == QLatin1String("cancelDownload")) {
    const auto requestId = static_cast<uint64>(json.value(QStringLiteral("requestId")).toString().toULongLong());
    auto callback = m_downloadItemCallbacks.take(requestId);
    if (!callback) {
      qCWarning(handler) << "Unknown request with id" << requestId << "for query" << json;
      return false;
    }
    callback->Cancel();
    return true;
  }

  const auto subBrowserId = json.value(QStringLiteral("browser")).toInt(-1);
  auto& subBrowserInfo = m_browsers[subBrowserId];
  const auto& subBrowser = subBrowserInfo.browser;
  if (!subBrowser) {
    qCWarning(handler) << "Unknown browser with id" << subBrowserId << "for request" << json;
    return false;
  }

  // below, all queries work on a browser
  if (type == QLatin1String("webPageSignals")) {
    subBrowserInfo.signalCallback = callback;
    Q_ASSERT(persistent);
    return true;
  } else if (type == QLatin1String("openWebPage")) {
    const auto url = QUrl::fromUserInput(json.value(QStringLiteral("url")).toString(),
                                         json.value(QStringLiteral("libraryPath")).toString(),
                                         QUrl::AssumeLocalFile);
    subBrowser->GetMainFrame()->LoadURL(url.toString().toStdString());
    m_waitForLoadedCallbacks.insert(subBrowser->GetIdentifier(), callback);
    return true;
  } else if (type == QLatin1String("waitForLoaded")) {
    m_waitForLoadedCallbacks.insert(subBrowser->GetIdentifier(), callback);
    return true;
  } else if (type == QLatin1String("waitForDownload")) {
    m_waitForDownloadCallbacks.insert(subBrowser->GetIdentifier(), callback);
    return true;
  } else if (type == QLatin1String("stopWebPage")) {
    subBrowser->StopLoad();
    callback->Success({});
    return true;
  } else if (type == QLatin1String("closeWebPage")) {
    subBrowser->GetHost()->CloseBrowser(true);
    callback->Success({});
    return true;
  } else if (type == QLatin1String("evaluateJavaScript")) {
    auto code = json.value(QStringLiteral("code")).toString();
    auto url = json.value(QStringLiteral("url")).toString(QStringLiteral("phantomjs://evaluateJavaScript"));
    auto line = json.value(QStringLiteral("line")).toInt(1);
    auto args = json.value(QStringLiteral("args")).toString(QStringLiteral("[]"));
    m_pendingQueryCallbacks[query_id] = callback;
    code = "phantom.internal.handleEvaluateJavaScript(" + code + ", " + args + ", " + QString::number(query_id) + ")";
    subBrowser->GetMainFrame()->ExecuteJavaScript(code.toStdString(), url.toStdString(), line);
    return true;
  } else if (type == QLatin1String("setProperty")) {
    const auto name = json.value(QStringLiteral("name")).toString();
    const auto value = json.value(QStringLiteral("value"));
    if (name == QLatin1String("viewportSize")) {
      const auto width = value.toObject().value(QStringLiteral("width")).toInt(-1);
      const auto height = value.toObject().value(QStringLiteral("height")).toInt(-1);
      if (width < 0 || height < 0) {
        callback->Failure(1, "Invalid viewport size.");
        return true;
      } else {
        const auto newSize = qMakePair(width, height);
        auto& oldSize = m_viewRects[subBrowserId];
        if (newSize != oldSize) {
          m_viewRects[subBrowserId] = newSize;
          subBrowser->GetHost()->WasResized();
        }
      }
    } else if (name == QLatin1String("zoomFactor")) {
      const auto value = json.value(QStringLiteral("value")).toDouble(1.);
      /// TODO: this doesn't seem to work
      subBrowser->GetHost()->SetZoomLevel(value);
    } else {
      callback->Failure(1, "unknown property: " + name.toStdString());
      return true;
    }
    callback->Success({});
    return true;
  } else if (type == QLatin1String("renderImage")) {
    const auto path = json.value(QStringLiteral("path")).toString();
    const auto format = json.value(QStringLiteral("format")).toString();
    const auto clipRectJson = json.value(QStringLiteral("clipRect")).toObject();
    const auto clipRect = QRect(
      clipRectJson.value(QStringLiteral("left")).toDouble(),
      clipRectJson.value(QStringLiteral("top")).toDouble(),
      clipRectJson.value(QStringLiteral("width")).toDouble(),
      clipRectJson.value(QStringLiteral("height")).toDouble()
    );
    m_paintCallbacks[subBrowserId] = {path, format, clipRect, callback};
    subBrowser->GetHost()->Invalidate(PET_VIEW);
    return true;
  } else if (type == QLatin1String("printPdf")) {
    const auto path = json.value(QStringLiteral("path")).toString().toStdString();
    CefPdfPrintSettings settings;
    const auto paperSize = json.value(QStringLiteral("paperSize")).toObject();
    if (!paperSize.value(QStringLiteral("orientation")).toString().compare("landscape", Qt::CaseInsensitive)) {
      settings.landscape = true;
    }
    QPageSize pageSize;
    if (paperSize.contains(QStringLiteral("format"))) {
      pageSize = pageSizeForName(paperSize.value(QStringLiteral("format")).toString());
    } else if (paperSize.contains(QStringLiteral("width")) && paperSize.contains(QStringLiteral("height"))) {
      auto width = stringToPointSize(paperSize.value(QStringLiteral("width")).toString());
      auto height = stringToPointSize(paperSize.value(QStringLiteral("height")).toString());
      pageSize = QPageSize(QSize(width, height), QPageSize::Point);
    }
    auto rect = pageSize.rect(QPageSize::Millimeter);
    settings.page_height = rect.height() * 1000;
    settings.page_width = rect.width() * 1000;

    const auto margin = paperSize.value(QStringLiteral("margin"));
    if (margin.isString()) {
      const auto marginString = margin.toString();
      if (marginString == QLatin1String("default")) {
        settings.margin_type = PDF_PRINT_MARGIN_DEFAULT;
      } else if (marginString == QLatin1String("minimum")) {
        settings.margin_type = PDF_PRINT_MARGIN_MINIMUM;
      } else if (marginString == QLatin1String("none")) {
        settings.margin_type = PDF_PRINT_MARGIN_NONE;
      } else {
        settings.margin_type = PDF_PRINT_MARGIN_CUSTOM;
        int intMargin = stringToMillimeter(marginString);
        settings.margin_left = intMargin;
        settings.margin_top = intMargin;
        settings.margin_right = intMargin;
        settings.margin_bottom = intMargin;
      }
    } else if (margin.isObject()) {
      auto marginObject = margin.toObject();
      settings.margin_type = PDF_PRINT_MARGIN_CUSTOM;
      settings.margin_left = stringToMillimeter(marginObject.value(QStringLiteral("left")).toString());
      settings.margin_top = stringToMillimeter(marginObject.value(QStringLiteral("top")).toString());
      settings.margin_right = stringToMillimeter(marginObject.value(QStringLiteral("right")).toString());
      settings.margin_bottom = stringToMillimeter(marginObject.value(QStringLiteral("bottom")).toString());
    }
    qCDebug(print) << paperSize << pageSize.name() << settings.page_height << settings.page_width << "landscape:" << settings.landscape
                    << "margins:"<< settings.margin_bottom << settings.margin_left << settings.margin_top << settings.margin_right << "margin type:" << settings.margin_type;
    subBrowser->GetHost()->PrintToPDF(path, settings, makePdfPrintCallback([callback] (const CefString& path, bool success) {
      if (success) {
        callback->Success(path);
      } else {
        callback->Failure(1, std::string("failed to print to path ") + path.ToString());
      }
    }));
    return true;
  } else if (type == QLatin1String("sendEvent")) {
    const auto event = json.value(QStringLiteral("event")).toString();
    const auto modifiers = json.value(QStringLiteral("modifiers")).toInt();
    qCDebug(handler) << json << event;
    if (event == QLatin1String("keydown") || event == QLatin1String("keyup") || event == QLatin1String("keypress")) {
      CefKeyEvent keyEvent;
      keyEvent.modifiers = modifiers;
      auto arg1 = json.value(QStringLiteral("arg1"));
      if (arg1.isString()) {
        foreach (auto c, arg1.toString()) {
          keyEvent.character = c.unicode();
          // use uppercase chars where possible, required for ASCII letters
          keyEvent.windows_key_code = c.toUpper().unicode();
          keyEvent.native_key_code = c.toUpper().unicode();
          keyEvent.type = KEYEVENT_KEYDOWN;
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
          keyEvent.type = KEYEVENT_CHAR;
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
          keyEvent.type = KEYEVENT_KEYUP;
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
        }
      } else {
        if (event == QLatin1String("keydown")) {
          keyEvent.type = KEYEVENT_KEYDOWN;
        } else if (event == QLatin1String("keyup")) {
          keyEvent.type = KEYEVENT_KEYUP;
        } else {
          keyEvent.type = KEYEVENT_CHAR;
        }
        keyEvent.windows_key_code = arg1.toInt();
        keyEvent.native_key_code = vkToNative(keyEvent.windows_key_code);
        keyEvent.character = arg1.toInt();
        if (keyEvent.type != KEYEVENT_CHAR) {
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
        } else {
          keyEvent.type = KEYEVENT_KEYDOWN;
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
          keyEvent.type = KEYEVENT_CHAR;
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
          keyEvent.type = KEYEVENT_KEYUP;
          subBrowser->GetHost()->SendKeyEvent(keyEvent);
        }
      }
    } else if (event == QLatin1String("click") || event == QLatin1String("doubleclick")
            || event == QLatin1String("mousedown") || event == QLatin1String("mouseup")
            || event == QLatin1String("mousemove"))
    {
      CefMouseEvent mouseEvent;
      mouseEvent.modifiers = modifiers;
      if (!modifiers) {
        mouseEvent.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;
      }
      mouseEvent.x = json.value(QStringLiteral("arg1")).toDouble();
      mouseEvent.y = json.value(QStringLiteral("arg2")).toDouble();
      cef_mouse_button_type_t type = MBT_LEFT;
      const auto typeString = json.value(QStringLiteral("arg3")).toString();
      if (typeString == QLatin1String("right")) {
        type = MBT_RIGHT;
      } else if (typeString == QLatin1String("middle")) {
        type = MBT_MIDDLE;
      }
      if (event == QLatin1String("doubleclick")) {
        subBrowser->GetHost()->SendMouseClickEvent(mouseEvent, type, false, 2);
        subBrowser->GetHost()->SendMouseClickEvent(mouseEvent, type, true, 2);
      } else if (event == QLatin1String("click")) {
        subBrowser->GetHost()->SendMouseClickEvent(mouseEvent, type, false, 1);
        subBrowser->GetHost()->SendMouseClickEvent(mouseEvent, type, true, 1);
      } else if (event == QLatin1String("mousemove")) {
        subBrowser->GetHost()->SendMouseMoveEvent(mouseEvent, false);
      } else {
        subBrowser->GetHost()->SendMouseClickEvent(mouseEvent, type, event == QLatin1String("mouseup"), 1);
      }
    } else {
      callback->Failure(1, "invalid event type passed to sendEvent: " + event.toStdString());
      return true;
    }
    callback->Success({});
    return true;
  } else if (type == QLatin1String("download")) {
    const auto source = json.value(QStringLiteral("source")).toString();
    const auto target = json.value(QStringLiteral("target")).toString();
    m_downloadTargets[source] = {target, callback};
    subBrowser->GetHost()->StartDownload(source.toStdString());
    return true;
  }
  return false;
}

void PhantomJSHandler::OnQueryCanceled(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                       int64 query_id)
{
  CEF_REQUIRE_UI_THREAD();

  m_waitForLoadedCallbacks.remove(browser->GetIdentifier());
  m_pendingQueryCallbacks.remove(query_id);
  m_paintCallbacks.remove(browser->GetIdentifier());
}

void PhantomJSHandler::OnBeforeDownload(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> download_item, const CefString& suggested_name, CefRefPtr<CefBeforeDownloadCallback> callback)
{
  const auto source = QString::fromStdString(download_item->GetOriginalUrl());
  const auto target = m_downloadTargets.take(source);

  qCDebug(handler) << browser->GetIdentifier() << source << target.target;

  if (!target.callback) {
    // not a manually triggered page.download() call but an indirect one
    // call back to the user script to ask for a download location
    m_beforeDownloadCallbacks[download_item->GetId()] = callback;

    emitSignal(browser, QStringLiteral("onBeforeDownload"), {
                QString::number(download_item->GetId()),
                QString::fromStdString(download_item->GetOriginalUrl())
              }, true);
    return;
  }

  m_downloadCallbacks[download_item->GetId()] = target.callback;
  callback->Continue(target.target.toStdString(), false);
}

void PhantomJSHandler::OnDownloadUpdated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDownloadItem> download_item, CefRefPtr<CefDownloadItemCallback> callback)
{
  qCDebug(handler) << browser->GetIdentifier() << download_item->GetURL() << download_item->GetPercentComplete() << download_item->IsComplete() << download_item->IsCanceled() << download_item->IsInProgress();

  m_downloadItemCallbacks[download_item->GetId()] = callback;
  const auto jsonDownloadItem = toJson(download_item);

  emitSignal(browser, QStringLiteral("onDownloadUpdated"),
             {QString::number(download_item->GetId()), jsonDownloadItem}, true);

  if (!download_item->IsInProgress()) {
    QByteArray data;
    std::string errorMessage;
    if (download_item->IsCanceled()) {
      errorMessage = "Download of " + download_item->GetURL().ToString() + " canceled.";
    } else if (!download_item->IsComplete()) {
      errorMessage = "Download of " + download_item->GetURL().ToString() + " ailed.";
    } else {
       data = QJsonDocument(jsonDownloadItem).toJson();
    }
    auto callback = m_downloadCallbacks.take(download_item->GetId());
    if (callback) {
      if (errorMessage.empty()) {
        callback->Success(data.constData());
      } else {
        callback->Failure(download_item->IsCanceled() ? 0 : 1, errorMessage);
      }
    }
    while (auto callback = takeCallback(&m_waitForDownloadCallbacks, browser)) {
      if (errorMessage.empty()) {
        callback->Success(data.constData());
      } else {
        callback->Failure(download_item->IsCanceled() ? 0 : 1, errorMessage);
      }
    }
  }
}
