#include <cstdio>
#include <string>
#include "MiniJson.h"

using namespace civ6base::json;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_fail; } } while (0)

int main() {
	// 1) 往返
	{
		Value v = Value::MakeObject();
		v.Set("name", Value::MakeString("测试 mod \"quoted\"\n"));
		v.Set("n", Value::MakeNumber(42));
		v.Set("f", Value::MakeNumber(1.5));
		v.Set("b", Value::MakeBool(true));
		Value arr = Value::MakeArray();
		arr.Append(Value::MakeNumber(1));
		arr.Append(Value::MakeString("two"));
		v.Set("arr", arr);
		std::string text = Serialize(v, 2);
		std::printf("%s\n", text.c_str());
		Value back;
		std::string err;
		CHECK(Parse(text, back, &err));
		CHECK(back.Find("n") && back.Find("n")->AsInt() == 42);
		CHECK(back.Find("f") && back.Find("f")->AsNumber() == 1.5);
		CHECK(back.Find("b") && back.Find("b")->AsBool());
		CHECK(back.Find("name") && back.Find("name")->AsString() == "测试 mod \"quoted\"\n");
		CHECK(back.Find("arr") && back.Find("arr")->Size() == 2);
	}
	// 2) 解析各种形态
	{
		Value v;
		CHECK(Parse("{\"a\":[1,2,3],\"b\":{\"c\":null},\"d\":\"\\u4e2d\\u6587\",\"e\":1e3}", v));
		CHECK(v.Find("a") && v.Find("a")->Size() == 3);
		CHECK(v.Find("b") && v.Find("b")->Find("c") && v.Find("b")->Find("c")->IsNull());
		CHECK(v.Find("d") && v.Find("d")->AsString() == "中文");
		CHECK(v.Find("e") && v.Find("e")->AsNumber() == 1000.0);
	}
	// 3) 错误输入
	{
		Value v;
		CHECK(!Parse("{\"a\":}", v));
		CHECK(!Parse("[1,2", v));
		CHECK(!Parse("nul", v));
		CHECK(!Parse("{} junk", v));
	}
	// 4) 整数形态
	{
		Value v = Value::MakeNumber(7);
		CHECK(Serialize(v, 0) == "7");
		Value d = Value::MakeNumber(0.25);
		CHECK(Serialize(d, 0) == "0.25");
	}
	// 5) 大文件/深嵌套保护
	{
		std::string deep;
		for (int i = 0; i < 200; ++i) deep += "[";
		for (int i = 0; i < 200; ++i) deep += "]";
		Value v;
		CHECK(!Parse(deep, v));
	}
	// 6) 删除/覆盖
	{
		Value v = Value::MakeObject();
		v.Set("k", Value::MakeNumber(1));
		v.Set("k", Value::MakeNumber(2));
		CHECK(v.Size() == 1 && v.Find("k")->AsInt() == 2);
		CHECK(v.Erase("k"));
		CHECK(!v.Erase("k"));
	}
	std::printf(g_fail == 0 ? "ALL OK\n" : "%d FAILURES\n", g_fail);
	return g_fail == 0 ? 0 : 1;
}
