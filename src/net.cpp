#include "net.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <unistd.h>
#endif

namespace net {

static const char kAgent[] = "IC7300Console (+https://github.com/laindonlad/IC-7300-Scope)";
// What a Session says it is: it stands in for the operator's own browser.
static const char kBrowserAgent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36";

std::string urlEscape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      char b[4];
      snprintf(b, sizeof b, "%%%02X", c);
      out += b;
    }
  }
  return out;
}

#ifdef _WIN32
static std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // ASCII only

static HINTERNET openSession(const char* agent) {
  HINTERNET h = WinHttpOpen(wide(agent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (h) WinHttpSetTimeouts(h, 10000, 10000, 15000, 15000);
  return h;
}

// One request on a WinHTTP session, which keeps its cookies and its connections to each server.
static bool request(HINTERNET s, const char* method, const std::string& url, const std::string& form, const std::string& headers, std::string& body,
                    std::string& error, size_t maxBytes) {
  body.clear();
  const std::wstring wurl = wide(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof parts;
  wchar_t host[256] = L"", path[2048] = L"", extra[2048] = L"";
  parts.lpszHostName = host, parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path, parts.dwUrlPathLength = 2048;
  parts.lpszExtraInfo = extra, parts.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
    error = "Not a web address: " + url;
    return false;
  }
  if (!s) {
    error = "Windows couldn't start a web session";
    return false;
  }
  const std::wstring target = std::wstring(path) + extra;
  HINTERNET connection = WinHttpConnect(s, host, parts.nPort, 0);
  HINTERNET req = connection ? WinHttpOpenRequest(connection, wide(method).c_str(), target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                             : nullptr;
  std::string allHeaders = headers;
  if (!form.empty()) allHeaders += "Content-Type: application/x-www-form-urlencoded\r\n";
  const std::wstring wheaders = wide(allHeaders);
  bool ok = false;
  if (req && WinHttpSendRequest(req, wheaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheaders.c_str(), wheaders.empty() ? 0 : (DWORD)-1L,
                                form.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)form.data(), (DWORD)form.size(), (DWORD)form.size(), 0) &&
      WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      char buf[16384];
      DWORD got = 0;
      ok = true;
      while (WinHttpReadData(req, buf, sizeof buf, &got) && got > 0) {
        body.append(buf, got);
        if (body.size() > maxBytes) {
          ok = false;
          error = "The answer was too long";
          break;
        }
      }
    } else {
      error = "The server answered " + std::to_string(status);
    }
  } else {
    error = "Couldn't reach the server (error " + std::to_string(GetLastError()) + ")";
  }
  if (req) WinHttpCloseHandle(req);
  if (connection) WinHttpCloseHandle(connection);
  if (!ok) body.clear();
  return ok;
}

bool httpGet(const std::string& url, std::string& body, std::string& error, size_t maxBytes) {
  // One session for the whole program, so connections to the same server are kept open and used again. WinHTTP's
  // handles can be shared between threads.
  static HINTERNET shared = openSession(kAgent);
  return request(shared, "GET", url, "", "", body, error, maxBytes);
}

Session::Session() { handle_ = openSession(kBrowserAgent); }
Session::~Session() {
  if (handle_) WinHttpCloseHandle((HINTERNET)handle_);
}
bool Session::send(const char* method, const std::string& url, const std::string& form, const std::string& headers, std::string& body, std::string& error,
                   size_t maxBytes) {
  return request((HINTERNET)handle_, method, url, form, headers, body, error, maxBytes);
}
#else
static std::string shellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

// curl, with what it's given. A form goes through a file, so a password isn't on the command line for others to see.
static bool runCurl(const std::string& args, const std::string& form, std::string& body, std::string& error, size_t maxBytes) {
  body.clear();
  std::string formFile;
  std::string cmd = "curl -fsSL --max-time 20 " + args;
  if (!form.empty()) {
    char name[] = "/tmp/ic7300formXXXXXX";
    int fd = mkstemp(name);
    if (fd < 0) {
      error = "Couldn't write a temporary file";
      return false;
    }
    bool wrote = write(fd, form.data(), form.size()) == (ssize_t)form.size();
    close(fd);
    formFile = name;
    if (!wrote) {
      unlink(name);
      error = "Couldn't write a temporary file";
      return false;
    }
    cmd += " --data-binary @" + shellQuote(formFile);
  }
  cmd += " 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) {
    if (!formFile.empty()) unlink(formFile.c_str());
    error = "Couldn't run curl";
    return false;
  }
  char buf[16384];
  size_t got;
  bool tooLong = false;
  while ((got = fread(buf, 1, sizeof buf, p)) > 0) {
    body.append(buf, got);
    if (body.size() > maxBytes) {
      tooLong = true;
      break;
    }
  }
  int rc = pclose(p);
  if (!formFile.empty()) unlink(formFile.c_str());
  if (tooLong || rc != 0) {
    error = tooLong ? "The answer was too long" : "Couldn't fetch it (is curl installed?)";
    body.clear();
    return false;
  }
  return true;
}

bool httpGet(const std::string& url, std::string& body, std::string& error, size_t maxBytes) {
  return runCurl("-A " + shellQuote(kAgent) + " " + shellQuote(url), "", body, error, maxBytes);
}

Session::Session() {
  char name[] = "/tmp/ic7300cookiesXXXXXX";
  int fd = mkstemp(name);
  if (fd >= 0) {
    close(fd);
    jar_ = name;
  }
}
Session::~Session() {
  if (!jar_.empty()) unlink(jar_.c_str());
}
bool Session::send(const char* method, const std::string& url, const std::string& form, const std::string& headers, std::string& body, std::string& error,
                   size_t maxBytes) {
  (void)method;  // a form makes it a POST, and curl follows a redirect after one with a GET, as a browser would
  std::string args = "-A " + shellQuote(kBrowserAgent);
  if (!jar_.empty()) args += " -b " + shellQuote(jar_) + " -c " + shellQuote(jar_);
  size_t at = 0;
  while (at < headers.size()) {
    size_t end = headers.find("\r\n", at);
    if (end == std::string::npos) end = headers.size();
    if (end > at) args += " -H " + shellQuote(headers.substr(at, end - at));
    at = end + 2;
  }
  args += " " + shellQuote(url);
  return runCurl(args, form, body, error, maxBytes);
}
#endif

bool Session::get(const std::string& url, std::string& body, std::string& error, const std::string& headers, size_t maxBytes) {
  return send("GET", url, "", headers, body, error, maxBytes);
}

bool Session::post(const std::string& url, const std::string& form, std::string& body, std::string& error, const std::string& headers, size_t maxBytes) {
  return send("POST", url, form, headers, body, error, maxBytes);
}

}  // namespace net
