// ModConfigCore.h —— ModConfig 全局 Lua 接口的核心实现（header-only）
//
// 目的：给 mod 作者一个"开箱即用"的键值持久化接口，不依赖游戏自身的存档/数据库，
// 也不需要 mod 自己写 C++。数据落在
//
//     %USERPROFILE%\Documents\My Games\Sid Meier's Civilization VI\ModConfigs\<名字>.json
//
// （若 Documents 被 OneDrive 等重定向，则跟随系统的 Known Folder 解析结果；也可以用
//  环境变量 CIV6_MODCONFIG_DIR 覆盖，便于作者本地调试。）
//
// 为什么是 JSON：
//   * 纯文本、UTF-8、可人读可 diff，玩家与作者都能直接看/改；
//   * 结构（嵌套表、数组）天然对应 Lua table，往返无信息损失；
//   * 解析器自带（MiniJson.h），不引第三方依赖，单 DLL 即可交付。
//
// 设计要点：
//   * 每个"名字"对应一个文件；名字由 mod 自己起（通常就是 mod 名），做文件名清洗；
//   * 写盘是"临时文件 + MoveFileEx 原子替换"，断电/崩溃不会留下半个文件；
//   * 读盘前比对 mtime + size，未变化直接吃内存缓存——Global 与 GameCore 两个插件
//     实例共享同一份磁盘文件，靠这个做到天然同步；
//   * table -> JSON 的编码交给注入到目标 Lua 环境里的一段纯 Lua 编码器：C++ 侧拿不到
//     lua_next，无法遍历哈希表；Lua 侧天然可以。C 函数只接收"已经编码好的 JSON 文本"，
//     因此这里不需要任何 Lua 伪索引（registry/globals）用法；
//   * JSON -> table 由 C++ 逐节点压栈构造。
//
// 线程模型：所有入口都必须在**游戏线程**调用（Lua 状态非线程安全）。文件 IO 是同步的，
// 但配置文件量级很小（通常 < 10KB），且带 mtime 缓存，不会成为帧时间热点。
//
// 本文件被 Global 与 GameCore 两个工程各复制一份，两份内容必须保持一致。

#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "HookAPI.h"
#include "MiniJson.h"

namespace civ6base {
namespace modconfig {

// 单个配置文件的大小上限（字节）。超过视为异常文件，不加载。
constexpr uint64_t kMaxFileBytes = 4ull * 1024ull * 1024ull;
// 名字（= 文件名主干）的最大字节数。
constexpr size_t kMaxNameBytes = 96;

// Lua 基础类型常量（与标准 Lua 5.1 / HavokScript 一致）。
enum LuaTypeTag {
	kLuaTNil = 0,
	kLuaTBoolean = 1,
	kLuaTLightUserData = 2,
	kLuaTNumber = 3,
	kLuaTString = 4,
	kLuaTTable = 5,
	kLuaTFunction = 6,
	kLuaTUserData = 7,
	kLuaTThread = 8
};

// ---------------------------------------------------------------------------
// 接口表与日志
// ---------------------------------------------------------------------------

inline HookAPI*& ApiRef() {
	static HookAPI* s_api = nullptr;
	return s_api;
}

inline void SetApi(HookAPI* api) { ApiRef() = api; }
inline HookAPI* Api() { return ApiRef(); }

// HookAPI::Log 是 varargs 入口，无法转发 va_list；统一"先格式化再 Log("%s", ...)"。
inline void LogFmt(const char* fmt, ...) {
	HookAPI* api = Api();
	if (!api || !api->Log) return;
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	::vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	api->Log("%s", buf);
}

// ---------------------------------------------------------------------------
// 编码转换
// ---------------------------------------------------------------------------

inline std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty()) return std::wstring();
	const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	if (len <= 0) return std::wstring();
	std::wstring out(static_cast<size_t>(len), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
	return out;
}

inline std::string WideToUtf8(const std::wstring& s) {
	if (s.empty()) return std::string();
	const int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
		nullptr, 0, nullptr, nullptr);
	if (len <= 0) return std::string();
	std::string out(static_cast<size_t>(len), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len, nullptr, nullptr);
	return out;
}

// ---------------------------------------------------------------------------
// 目录与文件名
// ---------------------------------------------------------------------------

// Documents\My Games\Sid Meier's Civilization VI\ModConfigs
// 解析失败返回空串（调用方据此把功能整体降级为"内存态"）。
inline std::wstring ResolveConfigDir() {
	wchar_t overrideBuf[1024] = {};
	const DWORD n = ::GetEnvironmentVariableW(L"CIV6_MODCONFIG_DIR", overrideBuf,
		static_cast<DWORD>(sizeof(overrideBuf) / sizeof(overrideBuf[0])));
	if (n > 0 && n < sizeof(overrideBuf) / sizeof(overrideBuf[0])) {
		std::wstring dir(overrideBuf);
		if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
		return dir;
	}

	PWSTR docs = nullptr;
	if (FAILED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs)) || !docs) {
		if (docs) ::CoTaskMemFree(docs);
		return std::wstring();
	}
	std::wstring dir(docs);
	::CoTaskMemFree(docs);
	if (dir.empty()) return std::wstring();

	dir += L"\\My Games\\Sid Meier's Civilization VI\\ModConfigs";
	return dir;
}

// 把任意名字清洗成一个安全的文件名主干（UTF-8 进 UTF-8 出）。
//   * 路径分隔符、Windows 保留字符、控制字符一律替换为 '_'；
//   * 去掉首尾空格与结尾的点（Windows 会静默截断）；
//   * 限制字节数，截断时回退到 UTF-8 边界，避免出现半个多字节字符。
inline std::string SanitizeName(const std::string& raw) {
	std::string out;
	out.reserve(raw.size());
	for (unsigned char c : raw) {
		if (c < 0x20 || c == 0x7F) {
			out.push_back('_');
			continue;
		}
		switch (c) {
		case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
			out.push_back('_');
			break;
		default:
			out.push_back(static_cast<char>(c));
			break;
		}
	}
	while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) out.erase(out.begin());
	while (!out.empty() && (out.back() == ' ' || out.back() == '.' || out.back() == '\t')) out.pop_back();
	if (out.size() > kMaxNameBytes) {
		size_t cut = kMaxNameBytes;
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
		out.resize(cut);
		while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
	}
	if (out.empty() || out == "." || out == "..") out = "default";
	return out;
}

inline std::wstring ConfigPathFor(const std::string& sanitizedName) {
	std::wstring dir = ResolveConfigDir();
	if (dir.empty()) return std::wstring();
	// ResolveConfigDir 的默认分支不带结尾分隔符（环境变量覆盖分支带），这里统一补齐。
	if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
	return dir + Utf8ToWide(sanitizedName) + L".json";
}

// ---------------------------------------------------------------------------
// 文件 IO
// ---------------------------------------------------------------------------

struct FileStamp {
	bool exists = false;
	uint64_t size = 0;
	uint64_t mtime = 0;

	bool operator==(const FileStamp& o) const {
		return exists == o.exists && size == o.size && mtime == o.mtime;
	}
	bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

inline FileStamp StatFile(const std::wstring& path) {
	FileStamp st;
	if (path.empty()) return st;
	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return st;
	st.exists = true;
	st.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
	st.mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
		fad.ftLastWriteTime.dwLowDateTime;
	return st;
}

inline bool ReadFileBytes(const std::wstring& path, std::string& out) {
	out.clear();
	HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0 ||
		static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) {
		::CloseHandle(h);
		return false;
	}
	out.resize(static_cast<size_t>(size.QuadPart));
	size_t done = 0;
	bool ok = true;
	while (done < out.size()) {
		DWORD got = 0;
		const DWORD want = static_cast<DWORD>(out.size() - done);
		if (!::ReadFile(h, &out[done], want, &got, nullptr) || got == 0) {
			ok = false;
			break;
		}
		done += got;
	}
	::CloseHandle(h);
	if (!ok) out.clear();
	return ok;
}

// 原子写：先写 <path>.tmp，再 MoveFileEx 替换。失败时回退为直接覆盖写。
inline bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
	if (path.empty()) return false;
	const std::wstring tmp = path + L".tmp";

	auto writeTo = [&](const std::wstring& target) -> bool {
		HANDLE h = ::CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		size_t done = 0;
		bool ok = true;
		while (done < data.size()) {
			DWORD wrote = 0;
			const DWORD want = static_cast<DWORD>(data.size() - done);
			if (!::WriteFile(h, data.data() + done, want, &wrote, nullptr) || wrote == 0) {
				ok = false;
				break;
			}
			done += wrote;
		}
		::FlushFileBuffers(h);
		::CloseHandle(h);
		return ok;
	};

	if (writeTo(tmp)) {
		if (::MoveFileExW(tmp.c_str(), path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			return true;
		}
		::DeleteFileW(tmp.c_str());
	}
	// 回退：直接覆盖（例如目标被占用/只读属性导致替换失败时，尽力而为）。
	return writeTo(path);
}

// 逐级创建目录（不用 SHCreateDirectoryEx：那是 shell 函数，某些错误路径会弹 UI）。
inline bool EnsureDir(const std::wstring& dir) {
	if (dir.empty()) return false;
	std::wstring partial;
	partial.reserve(dir.size());
	size_t i = 0;
	// 跳过盘符前缀 "C:\"
	if (dir.size() >= 2 && dir[1] == L':') {
		partial.assign(dir, 0, 2);
		i = 2;
	}
	while (i < dir.size()) {
		if (dir[i] == L'\\' || dir[i] == L'/') {
			if (!partial.empty() && partial.back() != L'\\') partial.push_back(L'\\');
			++i;
			continue;
		}
		const size_t start = i;
		while (i < dir.size() && dir[i] != L'\\' && dir[i] != L'/') ++i;
		partial.append(dir, start, i - start);
		if (::GetFileAttributesW(partial.c_str()) == INVALID_FILE_ATTRIBUTES) {
			if (!::CreateDirectoryW(partial.c_str(), nullptr) &&
				::GetLastError() != ERROR_ALREADY_EXISTS) {
				return false;
			}
		}
		if (i < dir.size()) {
			partial.push_back(L'\\');
			++i;
		}
	}
	return true;
}

// 损坏文件改名保留（.corrupt-N），避免下次启动继续读到坏数据。
inline void BackupCorruptFile(const std::wstring& path) {
	if (path.empty()) return;
	for (int i = 1; i <= 9; ++i) {
		wchar_t suffix[32];
		::swprintf_s(suffix, L".corrupt-%d", i);
		const std::wstring target = path + suffix;
		if (::GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
			::MoveFileExW(path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// 内存缓存（按名字）
// ---------------------------------------------------------------------------

struct ConfigEntry {
	json::Value root = json::Value::MakeObject();
	FileStamp stamp;
	bool valid = false;   // root 是否已从磁盘载入（或已确认磁盘上不存在）
};

inline std::map<std::string, ConfigEntry>& Cache() {
	static std::map<std::string, ConfigEntry> s_cache;
	return s_cache;
}

// 取得（必要时从磁盘重新载入）某个名字的配置。永远返回有效对象（磁盘异常时退化为空表）。
inline ConfigEntry* Acquire(const std::string& sanitizedName, const std::wstring& path) {
	ConfigEntry& entry = Cache()[sanitizedName];
	const FileStamp onDisk = StatFile(path);

	if (entry.valid && entry.stamp == onDisk) {
		return &entry;   // 内存与磁盘一致，直接用
	}

	if (!onDisk.exists) {
		entry.root = json::Value::MakeObject();
		entry.stamp = onDisk;
		entry.valid = true;
		return &entry;
	}

	std::string text;
	if (!ReadFileBytes(path, text)) {
		LogFmt("[ModConfig] read failed: %s", WideToUtf8(path).c_str());
		if (!entry.valid) {
			entry.root = json::Value::MakeObject();
			entry.valid = true;
			entry.stamp = onDisk;
		}
		return &entry;
	}

	// 空文件（或被编辑器清空）按"空配置"处理，不当作损坏备份。
	if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
		entry.root = json::Value::MakeObject();
		entry.stamp = onDisk;
		entry.valid = true;
		return &entry;
	}

	json::Value parsed;
	std::string err;
	if (!json::Parse(text, parsed, &err) || !parsed.IsObject()) {
		LogFmt("[ModConfig] corrupt file backed up (%s): %s",
			WideToUtf8(path).c_str(), err.empty() ? "top-level is not an object" : err.c_str());
		BackupCorruptFile(path);
		entry.root = json::Value::MakeObject();
		entry.stamp = StatFile(path);   // 备份后盘上已无此文件
		entry.valid = true;
		return &entry;
	}

	entry.root = std::move(parsed);
	entry.stamp = onDisk;
	entry.valid = true;
	return &entry;
}

inline bool SaveEntry(ConfigEntry& entry, const std::wstring& path) {
	if (path.empty()) return false;
	const std::wstring dir = ResolveConfigDir();
	if (!EnsureDir(dir)) {
		LogFmt("[ModConfig] cannot create dir: %s", WideToUtf8(dir).c_str());
		return false;
	}
	std::string text = json::Serialize(entry.root, 2);
	text.push_back('\n');
	if (!WriteFileAtomic(path, text)) {
		LogFmt("[ModConfig] write failed: %s", WideToUtf8(path).c_str());
		return false;
	}
	entry.stamp = StatFile(path);
	entry.valid = true;
	return true;
}

// ---------------------------------------------------------------------------
// JSON -> Lua 表
// ---------------------------------------------------------------------------

inline void PushJsonValue(void* L, const json::Value& v) {
	HookAPI* api = Api();
	switch (v.GetType()) {
	case json::Type::Null:
		api->LuaPushNil(L);
		break;
	case json::Type::Bool:
		api->LuaPushBoolean(L, v.AsBool());
		break;
	case json::Type::Number: {
		const double d = v.AsNumber();
		if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 2147483648.0) {
			api->LuaPushInteger(L, static_cast<int>(d));
		} else {
			api->LuaPushNumber(L, d);
		}
		break;
	}
	case json::Type::String:
		api->LuaPushFString(L, "%s", v.AsString().c_str());
		break;
	case json::Type::Array: {
		api->LuaCreateTable(L, static_cast<int>(v.Items().size()), 0);
		int index = 1;
		for (const auto& item : v.Items()) {
			api->LuaPushInteger(L, index++);
			PushJsonValue(L, item);
			api->LuaSetTable(L, -3);
		}
		break;
	}
	case json::Type::Object: {
		api->LuaCreateTable(L, 0, static_cast<int>(v.Members().size()));
		for (const auto& kv : v.Members()) {
			api->LuaPushFString(L, "%s", kv.first.c_str());
			PushJsonValue(L, kv.second);
			api->LuaSetTable(L, -3);
		}
		break;
	}
	default:
		api->LuaPushNil(L);
		break;
	}
}

// ---------------------------------------------------------------------------
// Lua 表 -> json::Value（纯 C++，不注入任何 Lua 代码）
//
// 用 HookAPI 的现成入口：
//   数组部分  LuaObjLen + LuaRawGetI
//   哈希部分  LuaNext（Lua 5.1 lua_next 的直接封装）
//   标量      LuaType / LuaToBoolean / LuaToNumber / LuaCheckLString
//
// 数组与对象的判别照抄 Lua 语义：LuaObjLen > 0 就当数组；否则按对象遍历哈希。
// ---------------------------------------------------------------------------

inline bool ReadLuaValue(void* L, int index, json::Value& out, int depth = 0);

inline bool ReadLuaTable(void* L, int index, json::Value& out, int depth) {
	HookAPI* api = Api();
	if (!api || !L) return false;
	if (depth > 16) return false;

	// 规范化到绝对位置：LuaNext / LuaRawGetI 都要在栈变化后仍然指向同一张表。
	const int abs = (index < 0) ? (api->LuaGetTop(L) + index + 1) : index;
	if (abs <= 0) return false;

	const unsigned long long arrayLen = api->LuaObjLen(L, abs);

	// ---- 数组部分 ----
	if (arrayLen > 0) {
		out = json::Value::MakeArray();
		for (unsigned long long i = 1; i <= arrayLen; ++i) {
			api->LuaRawGetI(L, abs, static_cast<int>(i));
			json::Value item;
			const bool ok = ReadLuaValue(L, -1, item, depth + 1);
			api->LuaPop(L, 1);
			out.Append(ok ? std::move(item) : json::Value());
		}
		return true;
	}

	// ---- 哈希部分 ----
	// 空表 + 哈希表都走这里；纯空表 traverse 一次就结束，结果就是 {}。
	out = json::Value::MakeObject();
	api->LuaPushNil(L);
	// 栈: ... table(即 abs) key
	while (api->LuaNext(L, abs)) {
		// 栈: ... key value
		// key 只接受字符串 / 有限的数字（JSON 的对象键必须是字符串）。
		bool keyOk = false;
		std::string key;
		if (api->LuaType(L, -2) == kLuaTString) {
			unsigned __int64 len = 0;
			const char* s = api->LuaCheckLString(L, -2, &len);
			if (s) {
				key.assign(s, static_cast<size_t>(len));
				keyOk = true;
			}
		} else if (api->LuaType(L, -2) == kLuaTNumber) {
			// 数字键（非数组部分）：按整数字面量当键名
			const double d = api->LuaToNumber(L, -2);
			if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) {
				char buf[32];
				::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
				key.assign(buf);
				keyOk = true;
			}
		}

		json::Value item;
		const bool valOk = ReadLuaValue(L, -1, item, depth + 1);
		if (keyOk && valOk) {
			out.Set(key, std::move(item));
		}
		api->LuaPop(L, 1);   // 弹出 value，保留 key 给下一轮（next 自己不会弹 value）
	}
	// LuaNext 返回 0 时已自行弹出 key，栈恢复平衡。
	return true;
}

inline bool ReadLuaValue(void* L, int index, json::Value& out, int depth) {
	HookAPI* api = Api();
	if (!api || !L) return false;
	if (depth > 16) return false;

	switch (api->LuaType(L, index)) {
	case kLuaTNil:
		out = json::Value();
		return true;
	case kLuaTBoolean:
		out = json::Value::MakeBool(api->LuaToBoolean(L, index) != 0);
		return true;
	case kLuaTNumber:
		out = json::Value::MakeNumber(api->LuaToNumber(L, index));
		return true;
	case kLuaTString: {
		const int abs = (index < 0) ? (api->LuaGetTop(L) + index + 1) : index;
		unsigned __int64 len = 0;
		const char* s = api->LuaCheckLString(L, abs, &len);
		if (!s) return false;
		out = json::Value::MakeString(std::string(s, static_cast<size_t>(len)));
		return true;
	}
	case kLuaTTable:
		return ReadLuaTable(L, index, out, depth);
	default:
		// function / userdata / thread：不可序列化
		return false;
	}
}

// ---------------------------------------------------------------------------
// Lua C 函数（全部薄封装，参数校验失败返回安全值，绝不抛 Lua 错误）
// ---------------------------------------------------------------------------

inline bool CheckStringArg(void* L, int index, std::string& out) {
	HookAPI* api = Api();
	if (!api || !L) return false;
	if (api->LuaType(L, index) != kLuaTString) return false;
	unsigned __int64 len = 0;
	const char* s = api->LuaCheckLString(L, index, &len);
	if (!s) return false;
	out.assign(s, static_cast<size_t>(len));
	return true;
}

// ModConfig.Load(name) -> table
inline int lLoad(void* L) {
	HookAPI* api = Api();
	std::string name;
	if (!CheckStringArg(L, 1, name)) {
		api->LuaPushNil(L);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry* entry = Acquire(key, ConfigPathFor(key));
	PushJsonValue(L, entry->root);
	return 1;
}

// ModConfig.Save(name, tbl) -> bool（整体替换）
inline int lSave(void* L) {
	HookAPI* api = Api();
	std::string name;
	if (!CheckStringArg(L, 1, name)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	if (api->LuaType(L, 2) != kLuaTTable) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	json::Value value;
	if (!ReadLuaValue(L, 2, value) || !value.IsObject()) {
		LogFmt("%s", "[ModConfig] Save: value not serializable (only nil/bool/number/string/array tables)");
		api->LuaPushBoolean(L, false);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry& entry = Cache()[key];
	entry.root = std::move(value);
	entry.valid = true;
	entry.stamp = FileStamp();
	api->LuaPushBoolean(L, SaveEntry(entry, ConfigPathFor(key)));
	return 1;
}

// ModConfig.Get(name, key, default) -> value
inline int lGet(void* L) {
	HookAPI* api = Api();
	std::string name;
	std::string field;
	if (!CheckStringArg(L, 1, name) || !CheckStringArg(L, 2, field)) {
		api->LuaPushNil(L);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry* entry = Acquire(key, ConfigPathFor(key));
	const json::Value* found = entry->root.Find(field);
	if (!found) {
		if (api->LuaGetTop(L) >= 3) {
			api->LuaPushValue(L, 3);
		} else {
			api->LuaPushNil(L);
		}
		return 1;
	}
	PushJsonValue(L, *found);
	return 1;
}

// ModConfig.Set(name, key, value) -> bool
inline int lSet(void* L) {
	HookAPI* api = Api();
	std::string name;
	std::string field;
	if (!CheckStringArg(L, 1, name) || !CheckStringArg(L, 2, field)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	json::Value value;
	if (!ReadLuaValue(L, 3, value)) {
		LogFmt("%s", "[ModConfig] Set: value not serializable (only nil/bool/number/string/array tables)");
		api->LuaPushBoolean(L, false);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry* entry = Acquire(key, ConfigPathFor(key));
	entry->root.Set(field, std::move(value));
	api->LuaPushBoolean(L, SaveEntry(*entry, ConfigPathFor(key)));
	return 1;
}

// ModConfig.Delete(name, key) -> bool
inline int lDelete(void* L) {
	HookAPI* api = Api();
	std::string name;
	std::string field;
	if (!CheckStringArg(L, 1, name) || !CheckStringArg(L, 2, field)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry* entry = Acquire(key, ConfigPathFor(key));
	if (!entry->root.Erase(field)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	api->LuaPushBoolean(L, SaveEntry(*entry, ConfigPathFor(key)));
	return 1;
}

// ModConfig.Has(name, key) -> bool
inline int lHas(void* L) {
	HookAPI* api = Api();
	std::string name;
	std::string field;
	if (!CheckStringArg(L, 1, name) || !CheckStringArg(L, 2, field)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry* entry = Acquire(key, ConfigPathFor(key));
	api->LuaPushBoolean(L, entry->root.Find(field) != nullptr);
	return 1;
}

// ModConfig.Keys(name) -> array of string
inline int lKeys(void* L) {
	HookAPI* api = Api();
	std::string name;
	if (!CheckStringArg(L, 1, name)) {
		api->LuaCreateTable(L, 0, 0);
		return 1;
	}
	const std::string key = SanitizeName(name);
	ConfigEntry* entry = Acquire(key, ConfigPathFor(key));
	const auto& members = entry->root.Members();
	api->LuaCreateTable(L, static_cast<int>(members.size()), 0);
	int index = 1;
	for (const auto& kv : members) {
		api->LuaPushInteger(L, index++);
		api->LuaPushFString(L, "%s", kv.first.c_str());
		api->LuaSetTable(L, -3);
	}
	return 1;
}

// ModConfig.Exists(name) -> bool（磁盘上是否已有该文件）
inline int lExists(void* L) {
	HookAPI* api = Api();
	std::string name;
	if (!CheckStringArg(L, 1, name)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	api->LuaPushBoolean(L, StatFile(ConfigPathFor(SanitizeName(name))).exists);
	return 1;
}

// ModConfig.GetPath(name) -> string（调试用）
inline int lGetPath(void* L) {
	HookAPI* api = Api();
	std::string name;
	if (!CheckStringArg(L, 1, name)) {
		api->LuaPushFString(L, "%s", "");
		return 1;
	}
	const std::wstring path = ConfigPathFor(SanitizeName(name));
	api->LuaPushFString(L, "%s", WideToUtf8(path).c_str());
	return 1;
}

// ModConfig.DeleteFile(name) -> bool（整份删除）
inline int lDeleteFile(void* L) {
	HookAPI* api = Api();
	std::string name;
	if (!CheckStringArg(L, 1, name)) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	const std::string key = SanitizeName(name);
	const std::wstring path = ConfigPathFor(key);
	Cache().erase(key);
	if (path.empty()) {
		api->LuaPushBoolean(L, false);
		return 1;
	}
	api->LuaPushBoolean(L, ::DeleteFileW(path.c_str()) != 0);
	return 1;
}

// ---------------------------------------------------------------------------
// 注册入口（在目标 Lua 环境的初始化回调里调用）
// ---------------------------------------------------------------------------

inline void RegisterLuaTable(void* L) {
	HookAPI* api = Api();
	if (!api || !L) return;

	api->LuaNewGlobalTable(L, "ModConfig", 0, 12);
	api->LuaPushCFunctionField(L, -1, "Load", reinterpret_cast<void*>(&lLoad), "lModConfigLoad");
	api->LuaPushCFunctionField(L, -1, "Save", reinterpret_cast<void*>(&lSave), "lModConfigSave");
	api->LuaPushCFunctionField(L, -1, "Get", reinterpret_cast<void*>(&lGet), "lModConfigGet");
	api->LuaPushCFunctionField(L, -1, "Set", reinterpret_cast<void*>(&lSet), "lModConfigSet");
	api->LuaPushCFunctionField(L, -1, "Delete", reinterpret_cast<void*>(&lDelete), "lModConfigDelete");
	api->LuaPushCFunctionField(L, -1, "Has", reinterpret_cast<void*>(&lHas), "lModConfigHas");
	api->LuaPushCFunctionField(L, -1, "Keys", reinterpret_cast<void*>(&lKeys), "lModConfigKeys");
	api->LuaPushCFunctionField(L, -1, "Exists", reinterpret_cast<void*>(&lExists), "lModConfigExists");
	api->LuaPushCFunctionField(L, -1, "GetPath", reinterpret_cast<void*>(&lGetPath), "lModConfigGetPath");
	api->LuaPushCFunctionField(L, -1, "DeleteFile", reinterpret_cast<void*>(&lDeleteFile), "lModConfigDeleteFile");
	api->LuaPushFString(L, "%s", "1.0");
	api->LuaSetField(L, -2, "Version");
	api->LuaPop(L, 1);

	LogFmt("[ModConfig] registered; dir=%s", WideToUtf8(ResolveConfigDir()).c_str());
}

} // namespace modconfig
} // namespace civ6base