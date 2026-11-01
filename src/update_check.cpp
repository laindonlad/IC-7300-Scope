#include "update_check.h"

#include <ctype.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace update {

const char* const kDownloadPage = "https://github.com/laindonlad/IC-7300-Scope#download";

#define UPDATE_REPO_PATH "/laindonlad/IC-7300-Scope/main/bin/"
#if defined(_WIN32)
#define UPDATE_FOLDER "windows-x64"
#elif defined(__APPLE__)
#define UPDATE_FOLDER "macos"
#else
#define UPDATE_FOLDER "linux-x64"
#endif

bool validStamp(const std::string& stamp) {
  static const char kShape[] = "dddd-dd-dd dd:dd";
  if (stamp.size() != sizeof kShape - 1) return false;
  for (size_t i = 0; i < stamp.size(); i++) {
    if (kShape[i] == 'd' ? !isdigit((unsigned char)stamp[i]) : stamp[i] != kShape[i]) return false;
  }
  return true;
}

bool isNewer(const std::string& published, const std::string& mine) {
  // In this form, later dates sort later as text.
  return validStamp(published) && validStamp(mine) && published > mine;
}

#ifdef _WIN32
static std::string download() {
  std::string body;
  HINTERNET session = WinHttpOpen(L"IC7300Console", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return body;
  WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
  HINTERNET connection = WinHttpConnect(session, L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", L"" UPDATE_REPO_PATH UPDATE_FOLDER "/BUILD_DATE", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    char buf[64];
    DWORD got = 0;
    // The file is one short line, so anything long isn't it.
    while (status == 200 && body.size() < 64 && WinHttpReadData(request, buf, sizeof buf, &got) && got > 0) body.append(buf, got);
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return body;
}
#else
static std::string download() {
  // There's no TLS library in this program, and macOS and nearly every Linux desktop have curl. Without it, no message.
  std::string body;
  FILE* p = popen("curl -fsSL --max-time 10 https://raw.githubusercontent.com" UPDATE_REPO_PATH UPDATE_FOLDER "/BUILD_DATE 2>/dev/null", "r");
  if (!p) return body;
  char buf[64];
  size_t got;
  while (body.size() < 64 && (got = fread(buf, 1, sizeof buf, p)) > 0) body.append(buf, got);
  pclose(p);
  return body;
}
#endif

std::string fetchPublished() {
  std::string s = download();
  while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
  return validStamp(s) ? s : "";
}

}  // namespace update
