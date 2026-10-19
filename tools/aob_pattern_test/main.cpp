// GameCoreResolve 的 AOB 解析/匹配自测
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gc {

inline bool PrologueMatchesMasked(const void* target, const uint8_t* value,
	const uint8_t* mask, size_t len) {
	if (!target || !value || !mask || len == 0) return false;
	const auto* bytes = static_cast<const uint8_t*>(target);
	for (size_t i = 0; i < len; ++i) {
		if ((bytes[i] & mask[i]) != (value[i] & mask[i])) return false;
	}
	return true;
}

template <size_t N>
struct AobPattern {
	uint8_t value[N]{};
	uint8_t mask[N]{};
	size_t count = 0;
	bool ok = false;
};

constexpr int HexNibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

template <size_t N>
constexpr AobPattern<N> ParseAob(const char (&text)[N]) {
	AobPattern<N> out{};
	size_t count = 0;
	int hi = -1;
	bool wildcard = false;
	for (size_t i = 0; i + 1 < N; ++i) {
		const char c = text[i];
		if (c == ' ' || c == '\t') {
			if (hi >= 0 || wildcard) {
				if (count < N) {
					out.value[count] = wildcard ? 0 : static_cast<uint8_t>(hi << 4);
					out.mask[count] = wildcard ? 0 : 0xFF;
					++count;
				}
				hi = -1; wildcard = false;
			}
			continue;
		}
		if (c == '?') { wildcard = true; continue; }
		const int nib = HexNibble(c);
		if (nib < 0) return out;
		if (hi < 0) { hi = nib; }
		else {
			if (count < N) {
				out.value[count] = static_cast<uint8_t>((hi << 4) | nib);
				out.mask[count] = 0xFF;
				++count;
			}
			hi = -1;
		}
	}
	if (hi >= 0 || wildcard) {
		if (count < N) {
			out.value[count] = wildcard ? 0 : static_cast<uint8_t>(hi << 4);
			out.mask[count] = wildcard ? 0 : 0xFF;
			++count;
		}
	}
	out.count = count;
	out.ok = count > 0;
	return out;
}

} // namespace gc

static int g_fail = 0;
static void Check(const char* what, bool cond) {
	if (!cond) { ++g_fail; std::printf("FAIL %s\n", what); }
	else std::printf("PASS %s\n", what);
}

int main() {
	using namespace gc;

	constexpr auto p1 = ParseAob("48 8B 05 ?? ?? ?? ?? C3");
	Check("p1.ok", p1.ok);
	Check("p1.count == 8", p1.count == 8);
	Check("p1[0]=0x48 mask=FF", p1.value[0] == 0x48 && p1.mask[0] == 0xFF);
	Check("p1[3] wildcard", p1.mask[3] == 0x00);
	Check("p1[7]=0xC3", p1.value[7] == 0xC3 && p1.mask[7] == 0xFF);

	// 真实字节：48 8B 05 11 95 23 00 C3
	const uint8_t real[] = { 0x48, 0x8B, 0x05, 0x11, 0x95, 0x23, 0x00, 0xC3 };
	Check("real matches", PrologueMatchesMasked(real, p1.value, p1.mask, p1.count));

	// 位移变化仍匹配
	const uint8_t other[] = { 0x48, 0x8B, 0x05, 0xAA, 0xBB, 0xCC, 0xDD, 0xC3 };
	Check("different disp32 matches", PrologueMatchesMasked(other, p1.value, p1.mask, p1.count));

	// retn 不对就不匹配（这正是加通配的目的）
	const uint8_t noRet[] = { 0x48, 0x8B, 0x05, 0x11, 0x95, 0x23, 0x00, 0x48 };
	Check("wrong retn rejected", !PrologueMatchesMasked(noRet, p1.value, p1.mask, p1.count));

	// 前 3 字节不对
	const uint8_t badOp[] = { 0x48, 0x8B, 0x0D, 0x11, 0x95, 0x23, 0x00, 0xC3 };
	Check("wrong opcode rejected", !PrologueMatchesMasked(badOp, p1.value, p1.mask, p1.count));

	// 单 '?' 也应能通配
	constexpr auto p2 = ParseAob("48 8B 05 ? ? ? ? C3");
	Check("single-? parses to 8 bytes", p2.count == 8 && p2.mask[3] == 0x00);

	// 无通配
	constexpr auto p3 = ParseAob("48 8B 01 C3");
	Check("no wildcard count==4", p3.count == 4 && p3.mask[0] == 0xFF && p3.mask[3] == 0xFF);
	const uint8_t hg[] = { 0x48, 0x8B, 0x01, 0xC3 };
	Check("Handle::get bytes match", PrologueMatchesMasked(hg, p3.value, p3.mask, p3.count));

	// 非法字符
	constexpr auto p4 = ParseAob("48 8B ZZ");
	Check("invalid rejected", !p4.ok);

	// 末尾残留
	constexpr auto p5 = ParseAob("48 8B 0");
	Check("odd trailing hex still counted", p5.count == 3 && p5.value[2] == 0x00);

	std::printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "HAS FAILURES", g_fail);
	return g_fail == 0 ? 0 : 1;
}
