#pragma once
// Sending a simple page of text to one of the system's printers: a heading, a few lines about it, and a table.
#include <string>
#include <vector>

namespace printing {
struct Doc {
  std::string title;
  std::vector<std::string> lines;     // under the title
  std::vector<std::string> columns;   // the table's headings
  std::vector<std::vector<std::string>> rows;
  std::vector<bool> faint;            // rows printed in grey, one per row where given
};

// The printers the system knows, and which of them is its default (empty if none is set).
std::vector<std::string> printers(std::string& defaultPrinter);

// Blocks until the job is with the spooler, so call it off the drawing thread. Text is UTF-8.
bool print(const std::string& printer, const Doc& doc, std::string& why);
}  // namespace printing
