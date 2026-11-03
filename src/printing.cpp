#include "printing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <winspool.h>
#else
#include <unistd.h>
#endif

namespace printing {

#ifdef _WIN32
static std::wstring wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

static std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return std::string();
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::vector<std::string> printers(std::string& defaultPrinter) {
  std::vector<std::string> out;
  defaultPrinter.clear();
  DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, need = 0, count = 0;
  EnumPrintersW(flags, nullptr, 4, nullptr, 0, &need, &count);
  if (need > 0) {
    std::vector<BYTE> buf(need);
    if (EnumPrintersW(flags, nullptr, 4, buf.data(), need, &need, &count)) {
      const PRINTER_INFO_4W* p = (const PRINTER_INFO_4W*)buf.data();
      for (DWORD i = 0; i < count; i++)
        if (p[i].pPrinterName) out.push_back(narrow(p[i].pPrinterName));
    }
  }
  wchar_t name[512];
  DWORD n = sizeof name / sizeof name[0];
  if (GetDefaultPrinterW(name, &n)) defaultPrinter = narrow(name);
  std::sort(out.begin(), out.end());
  return out;
}

bool print(const std::string& printer, const Doc& doc, std::string& why) {
  std::wstring wp = wide(printer);
  HDC dc = CreateDCW(L"WINSPOOL", wp.c_str(), nullptr, nullptr);
  if (!dc) {
    why = "Couldn't open the printer " + printer;
    return false;
  }
  int dpiX = GetDeviceCaps(dc, LOGPIXELSX), dpiY = GetDeviceCaps(dc, LOGPIXELSY);
  int offX = GetDeviceCaps(dc, PHYSICALOFFSETX), offY = GetDeviceCaps(dc, PHYSICALOFFSETY);
  int pageW = GetDeviceCaps(dc, PHYSICALWIDTH), pageH = GetDeviceCaps(dc, PHYSICALHEIGHT);
  // Three-quarter-inch margins, measured from the paper's edge rather than from where the printer can reach.
  int left = std::max(0, dpiX * 3 / 4 - offX), top = std::max(0, dpiY * 3 / 4 - offY);
  int right = std::min(GetDeviceCaps(dc, HORZRES), pageW - dpiX * 3 / 4 - offX);
  int bottom = std::min(GetDeviceCaps(dc, VERTRES), pageH - dpiY * 3 / 4 - offY);
  auto font = [&](int pt, int weight) {
    return CreateFontW(-MulDiv(pt, dpiY, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  };
  HFONT fTitle = font(16, FW_BOLD), fBody = font(10, FW_NORMAL), fBold = font(10, FW_BOLD);
  HGDIOBJ oldFont = SelectObject(dc, fBody);
  SetBkMode(dc, TRANSPARENT);
  auto textW = [&](const std::wstring& s) {
    SIZE z{0, 0};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &z);
    return (int)z.cx;
  };

  // Each column as wide as its widest entry, then all squeezed in proportion if together they're too wide.
  size_t nc = doc.columns.size();
  std::vector<int> colW(nc, 0);
  int gap = dpiX / 6;
  SelectObject(dc, fBold);
  for (size_t c = 0; c < nc; c++) colW[c] = textW(wide(doc.columns[c]));
  SelectObject(dc, fBody);
  for (const auto& r : doc.rows)
    for (size_t c = 0; c < nc && c < r.size(); c++) colW[c] = std::max(colW[c], textW(wide(r[c])));
  int total = 0;
  for (int& w : colW) total += (w += gap);
  int avail = right - left;
  if (total > avail && total > 0)
    for (int& w : colW) w = (int)((long long)w * avail / total);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  int lineH = tm.tmHeight + tm.tmExternalLeading + dpiY / 20;

  std::wstring wt = wide(doc.title.empty() ? "Print" : doc.title);
  DOCINFOW di;
  memset(&di, 0, sizeof di);
  di.cbSize = sizeof di;
  di.lpszDocName = wt.c_str();
  bool ok = StartDocW(dc, &di) > 0;
  if (!ok) why = GetLastError() == ERROR_CANCELLED ? "Printing was cancelled" : "The printer " + printer + " wouldn't take the job";
  HPEN pen = CreatePen(PS_SOLID, std::max(1, dpiY / 150), RGB(0, 0, 0));
  HGDIOBJ oldPen = SelectObject(dc, pen);
  int y = top;
  auto cell = [&](int x, int w, const std::string& s) {
    std::wstring ws = wide(s);
    RECT rc{x, y, x + w - gap / 2, y + lineH};
    DrawTextW(dc, ws.c_str(), (int)ws.size(), &rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  };
  auto header = [&] {
    SelectObject(dc, fBold);
    SetTextColor(dc, RGB(0, 0, 0));
    int x = left;
    for (size_t c = 0; c < nc; c++) cell(x, colW[c], doc.columns[c]), x += colW[c];
    y += lineH;
    MoveToEx(dc, left, y - dpiY / 40, nullptr);
    LineTo(dc, right, y - dpiY / 40);
    SelectObject(dc, fBody);
  };
  if (ok && StartPage(dc) > 0) {
    SelectObject(dc, fTitle);
    TextOutW(dc, left, y, wt.c_str(), (int)wt.size());
    TEXTMETRICW tt;
    GetTextMetricsW(dc, &tt);
    y += tt.tmHeight + dpiY / 12;
    SelectObject(dc, fBody);
    for (const std::string& l : doc.lines) {
      SetTextColor(dc, RGB(60, 60, 60));
      std::wstring wl = wide(l);
      TextOutW(dc, left, y, wl.c_str(), (int)wl.size());
      y += lineH;
    }
    y += lineH / 2;
    if (nc) header();
    for (size_t i = 0; i < doc.rows.size(); i++) {
      if (y + lineH > bottom) {
        EndPage(dc);
        if (StartPage(dc) <= 0) {
          ok = false;
          break;
        }
        y = top;
        header();
      }
      SetTextColor(dc, i < doc.faint.size() && doc.faint[i] ? RGB(128, 128, 128) : RGB(0, 0, 0));
      int x = left;
      for (size_t c = 0; c < nc; c++) {
        cell(x, colW[c], c < doc.rows[i].size() ? doc.rows[i][c] : std::string());
        x += colW[c];
      }
      y += lineH;
    }
    EndPage(dc);
    if (ok) EndDoc(dc);
    else AbortDoc(dc), why = "The printer " + printer + " stopped part way through";
  } else if (ok) {
    AbortDoc(dc);
    ok = false;
    why = "The printer " + printer + " wouldn't start a page";
  }
  SelectObject(dc, oldPen);
  SelectObject(dc, oldFont);
  DeleteObject(pen);
  DeleteObject(fTitle), DeleteObject(fBody), DeleteObject(fBold);
  DeleteDC(dc);
  return ok;
}

#else

// Every line a command prints.
static std::vector<std::string> commandLines(const char* cmd) {
  std::vector<std::string> out;
  FILE* p = popen(cmd, "r");
  if (!p) return out;
  char b[1024];
  while (fgets(b, sizeof b, p)) {
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) out.push_back(s);
  }
  pclose(p);
  return out;
}

static std::string quoted(const std::string& s) {
  std::string q = "'";
  for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
}

// Characters rather than bytes, so the columns line up with accented names.
static size_t chars(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}

static std::string pad(const std::string& s, size_t w) { return s + std::string(w > chars(s) ? w - chars(s) : 0, ' '); }

std::vector<std::string> printers(std::string& defaultPrinter) {
  defaultPrinter.clear();
  std::vector<std::string> out = commandLines("lpstat -e 2>/dev/null");
  if (out.empty())  // older CUPS: "NAME accepting requests since ..."
    for (const std::string& l : commandLines("lpstat -a 2>/dev/null")) out.push_back(l.substr(0, l.find(' ')));
  for (const std::string& l : commandLines("lpstat -d 2>/dev/null")) {
    size_t colon = l.find(": ");
    if (colon != std::string::npos) defaultPrinter = l.substr(colon + 2);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool print(const std::string& printer, const Doc& doc, std::string& why) {
  std::string t = doc.title + "\n" + std::string(chars(doc.title), '=') + "\n";
  for (const std::string& l : doc.lines) t += l + "\n";
  t += "\n";
  size_t nc = doc.columns.size();
  std::vector<size_t> w(nc, 0);
  for (size_t c = 0; c < nc; c++) w[c] = chars(doc.columns[c]);
  for (const auto& r : doc.rows)
    for (size_t c = 0; c < nc && c < r.size(); c++) w[c] = std::max(w[c], chars(r[c]));
  auto row = [&](const std::vector<std::string>& r) {
    std::string s;
    for (size_t c = 0; c < nc; c++) s += pad(c < r.size() ? r[c] : std::string(), w[c] + 2);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s + "\n";
  };
  if (nc) {
    t += row(doc.columns);
    size_t all = 0;
    for (size_t x : w) all += x + 2;
    t += std::string(all - 2, '-') + "\n";
  }
  for (const auto& r : doc.rows) t += row(r);

  char path[] = "/tmp/ic7300-print-XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    why = "Couldn't write the page to print";
    return false;
  }
  bool wrote = write(fd, t.data(), t.size()) == (ssize_t)t.size();
  close(fd);
  std::string cmd = "lp -d " + quoted(printer) + " -t " + quoted(doc.title) + " " + quoted(path) + " >/dev/null 2>&1";
  bool ok = wrote && system(cmd.c_str()) == 0;
  unlink(path);
  if (!ok) why = "The printer " + printer + " wouldn't take the job";
  return ok;
}
#endif

}  // namespace printing
