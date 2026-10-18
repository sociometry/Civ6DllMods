// MiniJson.h —— 极简 JSON 值容器 / 解析器 / 序列化器（header-only）
//
// 为什么自带一套：
//   * 插件必须是单 DLL、无外部依赖（/MT 静态运行库），不能引第三方库；
//   * 只需要"读一个对象、改几个字段、写回去"这一档能力，不需要完整 JSON 规范
//     里的所有边角（但解析侧仍按 RFC 8259 实现：转义、\uXXXX、代理对、数字、嵌套）。
//
// 设计取舍：
//   * 值语义 + 显式类型标签，容器用 std::vector：对象保持插入顺序（写回文件后
//     diff 稳定，人肉对比友好），查找是线性扫描——mod 配置量级（几百个键）完全够用；
//   * 数字统一存 double，写回时若为整数值则输出成整数形态，避免 1 变成 1.0；
//   * 解析有深度上限与字节上限，防止损坏/恶意文件把游戏进程的栈打爆；
//   * 字符串按 UTF-8 原样保存，不做编码转换（mod 名、中文值都直接可读）。
//
// 本文件被 Global 与 GameCore 两个工程各复制一份（仓库约定：每个 recipe 自带依赖），
// 两份内容必须保持一致。

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace civ6base {
namespace json {

enum class Type : uint8_t {
	Null = 0,
	Bool,
	Number,
	String,
	Array,
	Object
};

class Value {
public:
	Value() = default;

	static Value MakeBool(bool v) {
		Value r;
		r.type_ = Type::Bool;
		r.bool_ = v;
		return r;
	}
	static Value MakeNumber(double v) {
		Value r;
		r.type_ = Type::Number;
		r.number_ = v;
		return r;
	}
	static Value MakeString(std::string v) {
		Value r;
		r.type_ = Type::String;
		r.string_ = std::move(v);
		return r;
	}
	static Value MakeString(const char* v) { return MakeString(std::string(v ? v : "")); }
	static Value MakeArray() {
		Value r;
		r.type_ = Type::Array;
		return r;
	}
	static Value MakeObject() {
		Value r;
		r.type_ = Type::Object;
		return r;
	}

	Type GetType() const { return type_; }
	bool IsNull() const { return type_ == Type::Null; }
	bool IsBool() const { return type_ == Type::Bool; }
	bool IsNumber() const { return type_ == Type::Number; }
	bool IsString() const { return type_ == Type::String; }
	bool IsArray() const { return type_ == Type::Array; }
	bool IsObject() const { return type_ == Type::Object; }

	bool AsBool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
	double AsNumber(double fallback = 0.0) const { return type_ == Type::Number ? number_ : fallback; }
	int AsInt(int fallback = 0) const {
		if (type_ != Type::Number) return fallback;
		const double v = number_;
		if (v < -2147483648.0 || v > 2147483647.0) return fallback;
		return static_cast<int>(v);
	}
	const std::string& AsString() const {
		static const std::string kEmpty;
		return type_ == Type::String ? string_ : kEmpty;
	}

	size_t Size() const {
		if (type_ == Type::Array) return array_.size();
		if (type_ == Type::Object) return object_.size();
		return 0;
	}

	std::vector<Value>& Items() { return array_; }
	const std::vector<Value>& Items() const { return array_; }
	std::vector<std::pair<std::string, Value>>& Members() { return object_; }
	const std::vector<std::pair<std::string, Value>>& Members() const { return object_; }

	void Append(Value v) {
		if (type_ != Type::Array) {
			type_ = Type::Array;
			array_.clear();
		}
		array_.push_back(std::move(v));
	}

	// 对象成员查找；不存在返回 nullptr（不插入）。
	const Value* Find(const std::string& key) const {
		if (type_ != Type::Object) return nullptr;
		for (const auto& kv : object_) {
			if (kv.first == key) return &kv.second;
		}
		return nullptr;
	}

	// 对象成员写入；已存在则覆盖，保持原位置。
	Value& Set(const std::string& key, Value v) {
		if (type_ != Type::Object) {
			type_ = Type::Object;
			object_.clear();
		}
		for (auto& kv : object_) {
			if (kv.first == key) {
				kv.second = std::move(v);
				return kv.second;
			}
		}
		object_.emplace_back(key, std::move(v));
		return object_.back().second;
	}

	bool Erase(const std::string& key) {
		if (type_ != Type::Object) return false;
		for (auto it = object_.begin(); it != object_.end(); ++it) {
			if (it->first == key) {
				object_.erase(it);
				return true;
			}
		}
		return false;
	}

	void Clear() {
		*this = Value();
	}

	// 深拷贝（值语义默认就是深拷贝，这里只是给"从 const 源复制"一个显式入口）。
	Value Clone() const { return *this; }

private:
	Type type_ = Type::Null;
	bool bool_ = false;
	double number_ = 0.0;
	std::string string_;
	std::vector<Value> array_;
	std::vector<std::pair<std::string, Value>> object_;
};

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

inline void AppendNumber(std::string& out, double v) {
	if (!std::isfinite(v)) {
		out += "null";
		return;
	}
	// 整数值输出成整数形态（1 而不是 1.0），且限定在 double 能精确表达的范围内。
	if (v == static_cast<double>(static_cast<long long>(v)) && std::fabs(v) < 9007199254740992.0) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
		out += buf;
		return;
	}
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	if (std::strtod(buf, nullptr) == v) {
		// 17 位有效数字能 round-trip，但通常不是最短写法；先试 15 位。
		char shortBuf[40];
		std::snprintf(shortBuf, sizeof(shortBuf), "%.15g", v);
		if (std::strtod(shortBuf, nullptr) == v) {
			out += shortBuf;
			return;
		}
	}
	out += buf;
}

inline void AppendString(std::string& out, const std::string& s) {
	out.push_back('"');
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
				out += buf;
			} else {
				// >= 0x20 的字节原样透传：UTF-8 多字节序列保持可读。
				out.push_back(static_cast<char>(c));
			}
			break;
		}
	}
	out.push_back('"');
}

inline void SerializeInto(std::string& out, const Value& v, int indentWidth, int depth) {
	const bool pretty = indentWidth > 0;
	auto newline = [&](int d) {
		if (!pretty) return;
		out.push_back('\n');
		out.append(static_cast<size_t>(d) * static_cast<size_t>(indentWidth), ' ');
	};

	switch (v.GetType()) {
	case Type::Null: out += "null"; break;
	case Type::Bool: out += v.AsBool() ? "true" : "false"; break;
	case Type::Number: AppendNumber(out, v.AsNumber()); break;
	case Type::String: AppendString(out, v.AsString()); break;
	case Type::Array: {
		if (v.Items().empty()) {
			out += "[]";
			break;
		}
		out.push_back('[');
		bool first = true;
		for (const auto& item : v.Items()) {
			if (!first) out.push_back(',');
			first = false;
			newline(depth + 1);
			SerializeInto(out, item, indentWidth, depth + 1);
		}
		newline(depth);
		out.push_back(']');
		break;
	}
	case Type::Object: {
		if (v.Members().empty()) {
			out += "{}";
			break;
		}
		out.push_back('{');
		bool first = true;
		for (const auto& kv : v.Members()) {
			if (!first) out.push_back(',');
			first = false;
			newline(depth + 1);
			AppendString(out, kv.first);
			out.push_back(':');
			if (pretty) out.push_back(' ');
			SerializeInto(out, kv.second, indentWidth, depth + 1);
		}
		newline(depth);
		out.push_back('}');
		break;
	}
	default: out += "null"; break;
	}
}

// indentWidth = 0 输出紧凑格式；2 = 两空格缩进（配置文件默认用这个，便于人读与 diff）。
inline std::string Serialize(const Value& v, int indentWidth = 2) {
	std::string out;
	out.reserve(256);
	SerializeInto(out, v, indentWidth, 0);
	return out;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

class Parser {
public:
	Parser(const char* begin, const char* end) : p_(begin), end_(end) {}

	bool Parse(Value& out) {
		SkipWs();
		if (!ParseValue(out, 0)) return false;
		SkipWs();
		if (p_ != end_) {
			error_ = "trailing characters after top-level value";
			return false;
		}
		return true;
	}

	const std::string& error() const { return error_; }

private:
	static constexpr int kMaxDepth = 64;

	void SkipWs() {
		while (p_ != end_) {
			const char c = *p_;
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				++p_;
				continue;
			}
			break;
		}
	}

	bool Fail(const char* msg) {
		if (error_.empty()) error_ = msg;
		return false;
	}

	bool ParseValue(Value& out, int depth) {
		if (depth > kMaxDepth) return Fail("nesting too deep");
		if (p_ == end_) return Fail("unexpected end of input");
		switch (*p_) {
		case 'n':
			if (!MatchLiteral("null")) return false;
			out = Value();
			return true;
		case 't':
			if (!MatchLiteral("true")) return false;
			out = Value::MakeBool(true);
			return true;
		case 'f':
			if (!MatchLiteral("false")) return false;
			out = Value::MakeBool(false);
			return true;
		case '"': {
			std::string s;
			if (!ParseString(s)) return false;
			out = Value::MakeString(std::move(s));
			return true;
		}
		case '[': return ParseArray(out, depth);
		case '{': return ParseObject(out, depth);
		default: return ParseNumber(out);
		}
	}

	bool MatchLiteral(const char* lit) {
		const size_t len = std::strlen(lit);
		if (static_cast<size_t>(end_ - p_) < len) return Fail("truncated literal");
		if (std::memcmp(p_, lit, len) != 0) return Fail("invalid literal");
		p_ += len;
		return true;
	}

	bool ParseNumber(Value& out) {
		const char* start = p_;
		if (p_ != end_ && (*p_ == '-' || *p_ == '+')) ++p_;
		bool any = false;
		while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
			++p_;
			any = true;
		}
		if (p_ != end_ && *p_ == '.') {
			++p_;
			while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
				++p_;
				any = true;
			}
		}
		if (!any) return Fail("invalid number");
		if (p_ != end_ && (*p_ == 'e' || *p_ == 'E')) {
			++p_;
			if (p_ != end_ && (*p_ == '-' || *p_ == '+')) ++p_;
			bool expAny = false;
			while (p_ != end_ && *p_ >= '0' && *p_ <= '9') {
				++p_;
				expAny = true;
			}
			if (!expAny) return Fail("invalid exponent");
		}
		std::string text(start, p_);
		out = Value::MakeNumber(std::strtod(text.c_str(), nullptr));
		return true;
	}

	static void AppendUtf8(std::string& out, uint32_t cp) {
		if (cp < 0x80) {
			out.push_back(static_cast<char>(cp));
		} else if (cp < 0x800) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	bool ParseHex4(uint32_t& out) {
		if (end_ - p_ < 4) return Fail("truncated \\u escape");
		uint32_t v = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = p_[i];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
			else return Fail("invalid hex digit in \\u escape");
		}
		p_ += 4;
		out = v;
		return true;
	}

	bool ParseString(std::string& out) {
		if (p_ == end_ || *p_ != '"') return Fail("expected string");
		++p_;
		out.clear();
		while (p_ != end_) {
			const unsigned char c = static_cast<unsigned char>(*p_);
			if (c == '"') {
				++p_;
				return true;
			}
			if (c == '\\') {
				++p_;
				if (p_ == end_) return Fail("truncated escape");
				const char e = *p_++;
				switch (e) {
				case '"': out.push_back('"'); break;
				case '\\': out.push_back('\\'); break;
				case '/': out.push_back('/'); break;
				case 'b': out.push_back('\b'); break;
				case 'f': out.push_back('\f'); break;
				case 'n': out.push_back('\n'); break;
				case 'r': out.push_back('\r'); break;
				case 't': out.push_back('\t'); break;
				case 'u': {
					uint32_t cp = 0;
					if (!ParseHex4(cp)) return false;
					if (cp >= 0xD800 && cp <= 0xDBFF) {
						// 高代理项：后面必须紧跟 \uDC00-\uDFFF
						if (end_ - p_ >= 2 && p_[0] == '\\' && p_[1] == 'u') {
							p_ += 2;
							uint32_t low = 0;
							if (!ParseHex4(low)) return false;
							if (low >= 0xDC00 && low <= 0xDFFF) {
								cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
							} else {
								AppendUtf8(out, 0xFFFD);
								cp = low;
							}
						} else {
							cp = 0xFFFD;
						}
					} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
						cp = 0xFFFD;
					}
					AppendUtf8(out, cp);
					break;
				}
				default: return Fail("invalid escape character");
				}
				continue;
			}
			if (c < 0x20) return Fail("raw control character in string");
			out.push_back(static_cast<char>(c));
			++p_;
		}
		return Fail("unterminated string");
	}

	bool ParseArray(Value& out, int depth) {
		++p_; // '['
		out = Value::MakeArray();
		SkipWs();
		if (p_ != end_ && *p_ == ']') {
			++p_;
			return true;
		}
		for (;;) {
			Value item;
			SkipWs();
			if (!ParseValue(item, depth + 1)) return false;
			out.Append(std::move(item));
			SkipWs();
			if (p_ == end_) return Fail("unterminated array");
			if (*p_ == ',') {
				++p_;
				continue;
			}
			if (*p_ == ']') {
				++p_;
				return true;
			}
			return Fail("expected ',' or ']' in array");
		}
	}

	bool ParseObject(Value& out, int depth) {
		++p_; // '{'
		out = Value::MakeObject();
		SkipWs();
		if (p_ != end_ && *p_ == '}') {
			++p_;
			return true;
		}
		for (;;) {
			SkipWs();
			std::string key;
			if (!ParseString(key)) return false;
			SkipWs();
			if (p_ == end_ || *p_ != ':') return Fail("expected ':' after object key");
			++p_;
			SkipWs();
			Value item;
			if (!ParseValue(item, depth + 1)) return false;
			out.Set(key, std::move(item));
			SkipWs();
			if (p_ == end_) return Fail("unterminated object");
			if (*p_ == ',') {
				++p_;
				continue;
			}
			if (*p_ == '}') {
				++p_;
				return true;
			}
			return Fail("expected ',' or '}' in object");
		}
	}

	const char* p_ = nullptr;
	const char* end_ = nullptr;
	std::string error_;
};

// 解析一段 UTF-8 文本；失败时 out 保持原样并写入 err（若提供）。
inline bool Parse(const char* text, size_t len, Value& out, std::string* err = nullptr) {
	Value parsed;
	Parser parser(text, text + len);
	if (!parser.Parse(parsed)) {
		if (err) *err = parser.error();
		return false;
	}
	out = std::move(parsed);
	return true;
}

inline bool Parse(const std::string& text, Value& out, std::string* err = nullptr) {
	return Parse(text.data(), text.size(), out, err);
}

} // namespace json
} // namespace civ6base
