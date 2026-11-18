#include "sim/rules.hpp"

#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace altera::sim {
namespace {

bool keyChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

double parseNumber(const std::string& s, const std::string& text) {
    if (s.empty()) throw std::runtime_error("missing number in '" + text + "'");
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') throw std::runtime_error("bad number '" + s + "' in '" + text + "'");
    return v;
}

// subject:key, returns position after key.
size_t parseHead(const std::string& text, size_t i, std::string& subject, std::string& key) {
    const size_t colon = text.find(':', i);
    if (colon == std::string::npos || colon == i) throw std::runtime_error("expected subject:key in '" + text + "'");
    subject = text.substr(i, colon - i);
    for (char c : subject)
        if (!keyChar(c)) throw std::runtime_error("bad subject in '" + text + "'");
    size_t j = colon + 1;
    const size_t k0 = j;
    while (j < text.size() && keyChar(text[j])) ++j;
    if (j == k0) throw std::runtime_error("empty key in '" + text + "'");
    key = text.substr(k0, j - k0);
    return j;
}

} // namespace

Condition parseCondition(const std::string& text) {
    Condition c;
    c.text = text;
    size_t i = 0;
    if (!text.empty() && text[0] == '!') {
        c.negate = true;
        i = 1;
    }
    i = parseHead(text, i, c.subject, c.key);
    if (i == text.size()) return c;
    const std::string rest = text.substr(i);
    auto take = [&](const char* op, Condition::Op o) {
        const size_t n = std::char_traits<char>::length(op);
        if (rest.compare(0, n, op) != 0) return false;
        c.op = o;
        c.value = parseNumber(rest.substr(n), text);
        return true;
    };
    if (take(">=", Condition::Op::Ge) || take("<=", Condition::Op::Le) || take("==", Condition::Op::Eq) ||
        take("!=", Condition::Op::Ne) || take(">", Condition::Op::Gt) || take("<", Condition::Op::Lt))
        return c;
    throw std::runtime_error("bad operator in condition '" + text + "'");
}

Effect parseEffect(const std::string& text) {
    Effect e;
    e.text = text;
    const size_t i = parseHead(text, 0, e.subject, e.key);
    if (i == text.size()) return e;
    const char op = text[i];
    if (op != '+' && op != '-' && op != '=') throw std::runtime_error("bad operator in effect '" + text + "'");
    e.op = op;
    e.value = parseNumber(text.substr(i + 1), text);
    return e;
}

} // namespace altera::sim
