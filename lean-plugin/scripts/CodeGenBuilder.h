#pragma once

#include <string>
#include <vector>

namespace capnp {

class LeanCodeBuilder {
public:
  LeanCodeBuilder() = default;

  void append(const std::string& text) {
    if (atNewLine) {
      out += std::string(indentLevel * 2, ' ');
      atNewLine = false;
    }
    out += text;
  }

  void appendLine(const std::string& text) {
    append(text);
    newLine();
  }

  void newLine() {
    out += "\n";
    atNewLine = true;
  }

  void indent() {
    indentLevel++;
  }

  void dedent() {
    if (indentLevel > 0) indentLevel--;
  }

  const std::string& str() const {
    return out;
  }

private:
  std::string out;
  int indentLevel = 0;
  bool atNewLine = true;
};

} // namespace capnp
