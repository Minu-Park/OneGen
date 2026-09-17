#include "XmlHttp.h"
#include "OneGenError.h"
#include <chrono>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#elif defined(ONEGEN_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace onegen {
std::vector<char> fetch_xml_http(const std::string& url)
{
  if (url.empty() || url.size() > 65536) throw Error("HTTP XML URL length is invalid");
  std::vector<char> body;
#ifdef _WIN32
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                                         static_cast<int>(url.size()), nullptr, 0);
  if (!length) throw Error("HTTP XML URL is not valid UTF-8");
  std::wstring wide(length, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int>(url.size()), wide.data(), length);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide.c_str(), length, 0, &parts)) throw Error("Invalid HTTP XML URL");
  if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)
    throw Error("Only HTTP and HTTPS XML URLs are supported");
  using Handle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
  Handle session(WinHttpOpen(L"OneGen", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
  if (!session || !WinHttpSetTimeouts(session.get(), 1000, 3000, 5000, 5000))
    throw Error("Could not initialize HTTP XML retrieval");
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0), WinHttpCloseHandle);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  Handle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0), WinHttpCloseHandle);
  DWORD disabled = WINHTTP_DISABLE_REDIRECTS;
  if (!request || !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
    throw Error("Could not create HTTP XML request");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
      || !WinHttpReceiveResponse(request.get(), nullptr)) throw Error("HTTP XML request failed");
  DWORD status = 0, status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)
      || status != 200) throw Error("HTTP XML response status is not 200");
  char block[16384];
  for (;;) {
    if (std::chrono::steady_clock::now() >= deadline) throw Error("HTTP XML retrieval timed out");
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), block, sizeof(block), &read)) throw Error("HTTP XML read failed");
    if (!read) break;
    if (body.size() > maximum_xml_size - read) throw Error("HTTP XML exceeds 32 MiB");
    body.insert(body.end(), block, block + read);
  }
#elif defined(ONEGEN_HAVE_CURL)
  static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (initialized != CURLE_OK) throw Error("Could not initialize HTTP XML retrieval");
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
  if (!request) throw Error("Could not create HTTP XML request");
  curl_easy_setopt(request.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(request.get(), CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
  curl_easy_setopt(request.get(), CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(request.get(), CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(request.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(request.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(request.get(), CURLOPT_WRITEFUNCTION,
      +[](char* bytes, std::size_t size, std::size_t count, void* context) -> std::size_t {
        auto& output = *static_cast<std::vector<char>*>(context);
        if (size && count > maximum_xml_size / size) return 0;
        const auto length = size * count;
        if (length > maximum_xml_size - output.size()) return 0;
        try { output.insert(output.end(), bytes, bytes + length); } catch (...) { return 0; }
        return length;
      });
  if (curl_easy_perform(request.get()) != CURLE_OK) throw Error("HTTP XML request failed or exceeded its limits");
  long status = 0;
  curl_easy_getinfo(request.get(), CURLINFO_RESPONSE_CODE, &status);
  if (status != 200) throw Error("HTTP XML response status is not 200");
#else
  throw Error("HTTP XML retrieval requires an installed libcurl on this platform");
#endif
  if (body.empty()) throw Error("HTTP XML response is empty");
  return body;
}
}
