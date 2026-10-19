// ModConfig 端到端测试：用真实文件系统跑一遍 C++ 侧核心逻辑（Lua 部分用假 HookAPI 打桩）。
#include <windows.h>
#include <cstdio>
#include <string>

#define CIV6_PLUGIN_GLOBAL 1
#include "HookAPI.h"
#include "ModConfigCore.h"

static std::string g_lastError;
static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_fail; } } while (0)

// ---- 假 HookAPI：只实现测试用到的那几个 Lua 入口 ----
static std::vector<std::string> g_stackStrings;

static void FakeLog(const char*, ...) {}
static void FakePushFString(void*, const char*, ...) {}
static void FakePop(void*, int) {}
static void FakePushBoolean(void*, bool) {}
static void FakePushInteger(void*, int) {}
static void FakePushNumber(void*, double) {}
static void FakePushNil(void*) {}

int main() {
	// 用临时目录作为 ModConfig 根目录
	wchar_t tmp[MAX_PATH] = {};
	::GetTempPathW(MAX_PATH, tmp);
	std::wstring dir = std::wstring(tmp) + L"civ6_modconfig_test";
	::SetEnvironmentVariableW(L"CIV6_MODCONFIG_DIR", dir.c_str());

	civ6base::modconfig::SetApi(nullptr);   // 无 HookAPI：只测纯文件逻辑

	// 目录解析
	const std::wstring resolved = civ6base::modconfig::ResolveConfigDir();
	std::printf("dir = %ls\n", resolved.c_str());
	CHECK(!resolved.empty());

	// 路径拼接：默认分支（不带尾分隔符）也必须拼出 <dir>\<name>.json
	::SetEnvironmentVariableW(L"CIV6_MODCONFIG_DIR", (std::wstring(tmp) + L"civ6_modconfig_test").c_str());
	{
		const std::wstring p = civ6base::modconfig::ConfigPathFor("abc");
		CHECK(p.size() > 8 && p.compare(p.size() - 8, 8, L"abc.json") == 0);
		CHECK(p.find(L"\\abc.json") != std::wstring::npos);
	}
	::SetEnvironmentVariableW(L"CIV6_MODCONFIG_DIR", dir.c_str());

	// 名字清洗
	CHECK(civ6base::modconfig::SanitizeName("My Mod/v1") == "My Mod_v1");
	CHECK(civ6base::modconfig::SanitizeName("  a.  ") == "a");
	CHECK(civ6base::modconfig::SanitizeName("") == "default");
	CHECK(civ6base::modconfig::SanitizeName("..") == "default");

	// 写入 -> 读回
	auto* entry = civ6base::modconfig::Acquire("unit_test", civ6base::modconfig::ConfigPathFor("unit_test"));
	entry->root.Set("answer", civ6base::json::Value::MakeNumber(42));
	entry->root.Set("name", civ6base::json::Value::MakeString("测试"));
	CHECK(civ6base::modconfig::SaveEntry(*entry, civ6base::modconfig::ConfigPathFor("unit_test")));

	// 清缓存后重新从磁盘读
	civ6base::modconfig::Cache().erase("unit_test");
	auto* again = civ6base::modconfig::Acquire("unit_test", civ6base::modconfig::ConfigPathFor("unit_test"));
	CHECK(again->root.Find("answer") && again->root.Find("answer")->AsInt() == 42);
	CHECK(again->root.Find("name") && again->root.Find("name")->AsString() == "测试");

	// 损坏文件 -> 备份 + 空对象
	const std::wstring badPath = civ6base::modconfig::ConfigPathFor("corrupt_case");
	civ6base::modconfig::EnsureDir(civ6base::modconfig::ResolveConfigDir());
	{
		HANDLE h = ::CreateFileW(badPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		const char junk[] = "{ this is not json ";
		DWORD w = 0;
		::WriteFile(h, junk, sizeof(junk) - 1, &w, nullptr);
		::CloseHandle(h);
	}
	civ6base::modconfig::Cache().erase("corrupt_case");
	auto* bad = civ6base::modconfig::Acquire("corrupt_case", badPath);
	CHECK(bad->root.IsObject() && bad->root.Size() == 0);
	CHECK(::GetFileAttributesW(badPath.c_str()) == INVALID_FILE_ATTRIBUTES);
	CHECK(::GetFileAttributesW((badPath + L".corrupt-1").c_str()) != INVALID_FILE_ATTRIBUTES);

	// 空文件按空配置处理（不备份、不报损坏）
	const std::wstring emptyPath = civ6base::modconfig::ConfigPathFor("empty_case");
	{
		HANDLE h = ::CreateFileW(emptyPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		::CloseHandle(h);
	}
	civ6base::modconfig::Cache().erase("empty_case");
	auto* emptyEntry = civ6base::modconfig::Acquire("empty_case", emptyPath);
	CHECK(emptyEntry->root.IsObject() && emptyEntry->root.Size() == 0);
	CHECK(::GetFileAttributesW(emptyPath.c_str()) != INVALID_FILE_ATTRIBUTES);
	::DeleteFileW(emptyPath.c_str());

	// 原子写的临时文件不残留
	CHECK(::GetFileAttributesW((civ6base::modconfig::ConfigPathFor("unit_test") + L".tmp").c_str())
		== INVALID_FILE_ATTRIBUTES);

	// 清理
	::DeleteFileW(civ6base::modconfig::ConfigPathFor("unit_test").c_str());
	::DeleteFileW((badPath + L".corrupt-1").c_str());
	::RemoveDirectoryW(civ6base::modconfig::ResolveConfigDir().c_str());

	std::printf(g_fail == 0 ? "ALL OK\n" : "%d FAILURES\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
