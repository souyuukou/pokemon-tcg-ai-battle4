#pragma once

#include "ExactSearchHooks.h"
#include "ExactCanonicalState.h"
#include "ExactCpuEvaluator.h"
#include "ExactBigRational.h"
#include "ExactCardPartition.h"
#include "ExactCardLivenessV4.h"
#include "ExactFeatureRecordV4.h"
#include "ExactPassiveExpectationV4.h"
#include "ExactPassivePayloadV4.h"
#include "ExactSkeletonDag.h"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <numeric>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <future>
#include <fstream>
#include <stdexcept>
#include <emmintrin.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

// Process-wide exploration budget.  A root worker or an active chance worker
// must hold one permit; nested chance expansion can only acquire permits that
// remain available, so a two-core process never creates an unbounded async
// tree.  The peak is exported for regression tests and runtime diagnostics.
class ExactThreadBudget {
public:
	class Lease {
	public:
		Lease() = default;
		explicit Lease(ExactThreadBudget* owner) : owner_(owner) {}
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		Lease(Lease&& other) noexcept : owner_(other.owner_) { other.owner_ = nullptr; }
		Lease& operator=(Lease&& other) noexcept {
			if (this != &other) { release(); owner_ = other.owner_; other.owner_ = nullptr; }
			return *this;
		}
		~Lease() { release(); }
		void release() {
			if (owner_ != nullptr) { owner_->release(); owner_ = nullptr; }
		}
		explicit operator bool() const { return owner_ != nullptr; }
	private:
		ExactThreadBudget* owner_ = nullptr;
	};

	explicit ExactThreadBudget(int maximum = 2) : maximum_(std::max(1, maximum)), available_(maximum_) {}

	bool tryAcquire() {
		int current = available_.load(std::memory_order_relaxed);
		while (current > 0) {
			if (available_.compare_exchange_weak(current, current - 1,
				std::memory_order_acquire, std::memory_order_relaxed)) {
				const int active = maximum_ - (current - 1);
				int peak = peak_.load(std::memory_order_relaxed);
				while (active > peak && !peak_.compare_exchange_weak(peak, active,
					std::memory_order_relaxed, std::memory_order_relaxed)) {}
				return true;
			}
		}
		return false;
	}

	Lease tryLease() { return tryAcquire() ? Lease(this) : Lease(); }

	Lease acquire() {
		while (!tryAcquire()) std::this_thread::yield();
		return Lease(this);
	}

	void release() { available_.fetch_add(1, std::memory_order_release); }
	int available() const { return available_.load(std::memory_order_relaxed); }
	int active() const { return maximum_ - available(); }
	int peak() const { return peak_.load(std::memory_order_relaxed); }

private:
	int maximum_;
	std::atomic<int> available_;
	std::atomic<int> peak_{0};
};

inline ExactThreadBudget& ExactGlobalThreadBudget() {
	static ExactThreadBudget budget(2);
	return budget;
}

// Registers ActivateSkillEffect / AfterEffect / … so Passive scans can treat the
// skill-pipeline frames as EffectControl instead of Opaque (P0-2).
inline void ExactEnsureDeferredFunctionRegistry() {
	static std::once_flag registryOnce;
	std::call_once(registryOnce, [] {
		auto reg = [](void* fp, DeferredArgSemantics semantics = {
			DeferredArgSemantic::Unknown, DeferredArgSemantic::Unknown, DeferredArgSemantic::Unknown }) {
			auto found = FunctionIndexTable.find((long long)fp);
			if (found == FunctionIndexTable.end()) {
				throw std::runtime_error("deferred function is missing from FunctionIndexTable");
			}
			ExactCardLivenessV4::RegisterEffectControlFunction(found->second);
			// Register the semantic type explicitly. Effect indices and recursion
			// depths share the int ABI but must not be inferred from their value.
			ExactCardLivenessV4::RegisterDeferredFunctionSemantics(found->second, semantics);
		};
		reg((void*)AfterEffect);
		reg((void*)ActivateSkillEffect);
		reg((void*)ActivateEffectEachSelected,
			{ DeferredArgSemantic::EffectId, DeferredArgSemantic::None, DeferredArgSemantic::None });
		reg((void*)ActivateEffectForEach,
			{ DeferredArgSemantic::EffectId, DeferredArgSemantic::None, DeferredArgSemantic::None });
		reg((void*)ActivateEffectMultiple,
			{ DeferredArgSemantic::EffectId, DeferredArgSemantic::None, DeferredArgSemantic::None });
		reg((void*)SeparatorProc);
		// Attach / trigger pipeline (Enriching Energy attaches via temporaryTriggerStack).
		reg((void*)AfterAbility);
		reg((void*)AfterTriggerAbility,
			{ DeferredArgSemantic::Scalar, DeferredArgSemantic::None, DeferredArgSemantic::None });
		reg((void*)ResolveTriggerStack,
			{ DeferredArgSemantic::Scalar, DeferredArgSemantic::None, DeferredArgSemantic::None });
		reg((void*)AfterPlay);
		reg((void*)AfterRefresh);
		reg((void*)ToMain);
		reg((void*)MainSelect);
		reg((void*)SelectedMain);
	});
}

inline unsigned long long ExactPeakResidentBytes() {
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters)))
		return (unsigned long long)counters.PeakWorkingSetSize;
	return 0;
#else
	struct rusage usage{};
	if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
	return (unsigned long long)usage.ru_maxrss * 1024ULL;
#endif
}

inline unsigned long long ExactResidentBytes() {
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters)))
		return (unsigned long long)counters.WorkingSetSize;
	return 0;
#else
	// ru_maxrss is a process-lifetime peak and must never be used for the
	// live memory guard.  /proc/self/statm reports the current resident page
	// count, so a released TT/session can make the value fall again.
	std::ifstream statm("/proc/self/statm");
	unsigned long long sizePages = 0, residentPages = 0;
	if (statm >> sizePages >> residentPages) {
		const long pageSize = sysconf(_SC_PAGESIZE);
		if (pageSize > 0) return residentPages * (unsigned long long)pageSize;
	}
	return 0;
#endif
}

enum class ExactRuntimeMode : unsigned char { Legacy, Cow, Shadow };

inline ExactRuntimeMode ExactRuntimeModeFromEnvironment() {
	std::string value;
#ifdef _WIN32
	char configured[32]{};
	DWORD length = GetEnvironmentVariableA("PTCG_EXACT_RUNTIME", configured, (DWORD)std::size(configured));
	if (length == 0 || length >= std::size(configured)) return ExactRuntimeMode::Legacy;
	value.assign(configured, configured + length);
#else
	const char* configured = std::getenv("PTCG_EXACT_RUNTIME");
	if (configured == nullptr) return ExactRuntimeMode::Legacy;
	value.assign(configured);
#endif
	if (value == "cow") return ExactRuntimeMode::Cow;
	if (value == "shadow") return ExactRuntimeMode::Shadow;
	return ExactRuntimeMode::Legacy;
}

// Binary observation keys use an unambiguous unsigned LEB128 encoding and
// zig-zag signed integers.  Most rule values are below 64, so fixed-width i32
// keys wasted hundreds of bytes per TT entry.  The encoding remains injective;
// full bytes, rather than the hash, continue to decide equality.
struct ExactPackedKeyWriter {
	std::string bytes;
	void u8(unsigned char value) { bytes.push_back((char)value); }
	void u32(std::uint32_t value) {
		while (value >= 0x80U) { bytes.push_back((char)((value & 0x7fU) | 0x80U)); value >>= 7; }
		bytes.push_back((char)value);
	}
	void i32(int value) {
		const std::uint32_t bits = (std::uint32_t)value;
		u32((bits << 1) ^ (std::uint32_t)-(std::int32_t)(bits >> 31));
	}
	void i32Span(const int* values, size_t count) {
		for (size_t i = 0; i < count; ++i) i32(values[i]);
	}
	void u64(std::uint64_t value) {
		while (value >= 0x80ULL) { bytes.push_back((char)((value & 0x7fULL) | 0x80ULL)); value >>= 7; }
		bytes.push_back((char)value);
	}
	void blob(const std::string& value) {
		u32((std::uint32_t)value.size()); bytes.append(value.data(), value.size());
	}
};

struct ExactPackedDescriptor {
	// One entity can reference at most the 60 physical deck cards: five scalar
	// fields, up to 60 effective energy entries, and up to 60 attached/evolution
	// cards. An Attach/Evolve option adds both position headers. 160 therefore
	// retains a checked safety margin without spacing hot descriptors 1.5 KiB apart.
	static constexpr size_t MaxValues = 160;
	std::array<int, MaxValues> values{};
	std::uint16_t count = 0;
	void clear() { count = 0; }
	void assign(const ExactPackedDescriptor& other) {
		count = other.count;
		std::copy_n(other.values.begin(), count, values.begin());
	}
	void add(int value) {
		if (count >= MaxValues) throw std::length_error("packed exact descriptor overflow");
		values[count++] = value;
	}
	void append(const ExactPackedDescriptor& other) {
		add((int)other.count);
		for (std::uint16_t i = 0; i < other.count; ++i) add(other.values[i]);
	}
	bool operator<(const ExactPackedDescriptor& other) const {
		const auto first = values.begin(), second = other.values.begin();
		return std::lexicographical_compare(first, first + count, second, second + other.count);
	}
	bool operator==(const ExactPackedDescriptor& other) const {
		return count == other.count && std::equal(values.begin(), values.begin() + count, other.values.begin());
	}
	void write(ExactPackedKeyWriter& writer) const {
		writer.u32(count);
		writer.i32Span(values.data(), count);
	}
};

struct ExactFraction {
	long long numerator = 0;
	unsigned long long denominator = 1;
	bool valid = true;
	std::shared_ptr<const ExactBigRational> big;

	static ExactFraction integer(long long value) { return { value, 1, true }; }

	void normalize() {
		if (big) return;
		if (!valid || denominator == 0) { valid = false; return; }
		unsigned long long magnitude = numerator < 0 ? (unsigned long long)(-(numerator + 1)) + 1 : (unsigned long long)numerator;
		auto g = std::gcd(magnitude, denominator);
		if (g > 1) { numerator /= (long long)g; denominator /= g; }
	}

	static bool checkedMul(long long value, unsigned long long factor, long long& out) {
		if (factor == 0 || value == 0) { out = 0; return true; }
		if (factor > (unsigned long long)std::numeric_limits<long long>::max()) return false;
		long long f = (long long)factor;
		if (value > 0 && value > std::numeric_limits<long long>::max() / f) return false;
		if (value < 0 && value < std::numeric_limits<long long>::min() / f) return false;
		out = value * f;
		return true;
	}

	static bool checkedMulU(unsigned long long a, unsigned long long b, unsigned long long& out) {
		if (a != 0 && b > std::numeric_limits<unsigned long long>::max() / a) return false;
		out = a * b; return true;
	}

	ExactFraction scaled(unsigned long long weight, unsigned long long total) const {
		if (!valid || total == 0) return { 0, 1, false };
		unsigned long long originalWeight = weight, originalTotal = total;
		if (big) {
			auto value = std::make_shared<ExactBigRational>(*big); value->scale(weight, total);
			ExactFraction out; out.big = std::move(value); return out;
		}
		ExactFraction result = *this;
		auto g1 = std::gcd(weight, total); weight /= g1; total /= g1;
		unsigned long long magnitude = result.numerator < 0 ? (unsigned long long)(-(result.numerator + 1)) + 1 : (unsigned long long)result.numerator;
		auto g2 = std::gcd(magnitude, total); result.numerator /= (long long)g2; total /= g2;
		auto g3 = std::gcd(result.denominator, weight); result.denominator /= g3; weight /= g3;
		long long n; unsigned long long d;
		if (!checkedMul(result.numerator, weight, n) || !checkedMulU(result.denominator, total, d)) {
			auto value = std::make_shared<ExactBigRational>(numerator, denominator); value->scale(originalWeight, originalTotal);
			ExactFraction out; out.big = std::move(value); return out;
		}
		result.numerator = n; result.denominator = d; result.normalize(); return result;
	}

	ExactFraction scaled(const ExactWeight& weight, const ExactWeight& total) const {
		if (!valid || total.zero()) return { 0, 1, false };
		if (weight.fitsUnsignedLongLong() && total.fitsUnsignedLongLong())
			return scaled(weight.unsignedLongLong(), total.unsignedLongLong());
		ExactBigRational value = big ? *big : ExactBigRational(numerator, denominator);
		value.scale(weight, total);
		ExactFraction out; out.big = std::make_shared<ExactBigRational>(std::move(value)); return out;
	}

	static ExactFraction add(const ExactFraction& a, const ExactFraction& b) {
		if (!a.valid || !b.valid) return { 0, 1, false };
		if (a.big || b.big) {
			ExactBigRational av = a.big ? *a.big : ExactBigRational(a.numerator, a.denominator);
			ExactBigRational bv = b.big ? *b.big : ExactBigRational(b.numerator, b.denominator);
			ExactFraction out; out.big = std::make_shared<ExactBigRational>(ExactBigRational::add(av, bv)); return out;
		}
		auto g = std::gcd(a.denominator, b.denominator);
		unsigned long long am = b.denominator / g, bm = a.denominator / g;
		long long an, bn;
		unsigned long long d;
		if (!checkedMul(a.numerator, am, an) || !checkedMul(b.numerator, bm, bn)) {
			ExactFraction out; out.big = std::make_shared<ExactBigRational>(ExactBigRational::add(
				ExactBigRational(a.numerator, a.denominator), ExactBigRational(b.numerator, b.denominator))); return out;
		}
		if ((bn > 0 && an > std::numeric_limits<long long>::max() - bn)
			|| (bn < 0 && an < std::numeric_limits<long long>::min() - bn)) {
			ExactFraction out; out.big = std::make_shared<ExactBigRational>(ExactBigRational::add(
				ExactBigRational(a.numerator, a.denominator), ExactBigRational(b.numerator, b.denominator))); return out;
		}
		if (!checkedMulU(a.denominator, am, d)) {
			ExactFraction out; out.big = std::make_shared<ExactBigRational>(ExactBigRational::add(
				ExactBigRational(a.numerator, a.denominator), ExactBigRational(b.numerator, b.denominator))); return out;
		}
		ExactFraction result{ an + bn, d, true }; result.normalize(); return result;
	}

	std::string numeratorText() const { return big ? big->numerator.text() : std::to_string(numerator); }
	std::string denominatorText() const { return big ? big->denominatorText() : std::to_string(denominator); }
};

inline int ExactCompare(const ExactFraction& a, const ExactFraction& b) {
	if (a.big || b.big) {
		ExactBigRational av = a.big ? *a.big : ExactBigRational(a.numerator, a.denominator);
		ExactBigRational bv = b.big ? *b.big : ExactBigRational(b.numerator, b.denominator);
		return ExactBigRational::compare(av, bv);
	}
	if (a.numerator < 0 && b.numerator >= 0) return -1;
	if (a.numerator >= 0 && b.numerator < 0) return 1;
	auto magnitude = [](long long n) { return n < 0 ? (unsigned long long)(-(n + 1)) + 1 : (unsigned long long)n; };
	unsigned long long ah, al, bh, bl;
#ifdef _MSC_VER
	al = _umul128(magnitude(a.numerator), b.denominator, &ah);
	bl = _umul128(magnitude(b.numerator), a.denominator, &bh);
#else
	unsigned __int128 av = (unsigned __int128)magnitude(a.numerator) * b.denominator;
	unsigned __int128 bv = (unsigned __int128)magnitude(b.numerator) * a.denominator;
	ah = (unsigned long long)(av >> 64); al = (unsigned long long)av;
	bh = (unsigned long long)(bv >> 64); bl = (unsigned long long)bv;
#endif
	int cmp = ah != bh ? (ah < bh ? -1 : 1) : (al == bl ? 0 : (al < bl ? -1 : 1));
	return a.numerator < 0 ? -cmp : cmp;
}

inline unsigned long long ExactRotl64(unsigned long long value, int bits) {
	return (value << bits) | (value >> (64 - bits));
}

template<int Bits>
inline __m128i ExactRotl64x2(__m128i value) {
	return _mm_or_si128(_mm_slli_epi64(value, Bits), _mm_srli_epi64(value, 64 - Bits));
}

inline unsigned long long ExactSipHash24(const std::string& bytes, unsigned long long k0, unsigned long long k1) {
	unsigned long long v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
	unsigned long long v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
	auto rounds = [&](int count) {
		for (int i = 0; i < count; ++i) {
			v0 += v1; v1 = ExactRotl64(v1, 13); v1 ^= v0; v0 = ExactRotl64(v0, 32);
			v2 += v3; v3 = ExactRotl64(v3, 16); v3 ^= v2;
			v0 += v3; v3 = ExactRotl64(v3, 21); v3 ^= v0;
			v2 += v1; v1 = ExactRotl64(v1, 17); v1 ^= v2; v2 = ExactRotl64(v2, 32);
		}
	};
	size_t offset = 0;
	for (; offset + 8 <= bytes.size(); offset += 8) {
		unsigned long long word = 0;
		for (int i = 0; i < 8; ++i) word |= (unsigned long long)(unsigned char)bytes[offset + i] << (8 * i);
		v3 ^= word; rounds(2); v0 ^= word;
	}
	unsigned long long tail = (unsigned long long)bytes.size() << 56;
	for (size_t i = offset; i < bytes.size(); ++i) tail |= (unsigned long long)(unsigned char)bytes[i] << (8 * (i - offset));
	v3 ^= tail; rounds(2); v0 ^= tail; v2 ^= 0xff; rounds(4);
	return v0 ^ v1 ^ v2 ^ v3;
}

struct ExactStringHasher {
	size_t operator()(const std::string& bytes) const noexcept {
		unsigned long long a0 = 0x736f6d6570736575ULL ^ 0x7766554433221100ULL;
		unsigned long long a1 = 0x646f72616e646f6dULL ^ 0xffeeddccbbaa9988ULL;
		unsigned long long a2 = 0x6c7967656e657261ULL ^ 0x7766554433221100ULL;
		unsigned long long a3 = 0x7465646279746573ULL ^ 0xffeeddccbbaa9988ULL;
		unsigned long long b0 = 0x736f6d6570736575ULL ^ 0x8899aabbccddeeffULL;
		unsigned long long b1 = 0x646f72616e646f6dULL ^ 0x0011223344556677ULL;
		unsigned long long b2 = 0x6c7967656e657261ULL ^ 0x8899aabbccddeeffULL;
		unsigned long long b3 = 0x7465646279746573ULL ^ 0x0011223344556677ULL;
		__m128i v0 = _mm_set_epi64x((long long)b0, (long long)a0);
		__m128i v1 = _mm_set_epi64x((long long)b1, (long long)a1);
		__m128i v2 = _mm_set_epi64x((long long)b2, (long long)a2);
		__m128i v3 = _mm_set_epi64x((long long)b3, (long long)a3);
		auto roundBoth = [&] {
			v0 = _mm_add_epi64(v0, v1); v1 = ExactRotl64x2<13>(v1); v1 = _mm_xor_si128(v1, v0);
			v0 = ExactRotl64x2<32>(v0); v2 = _mm_add_epi64(v2, v3);
			v3 = ExactRotl64x2<16>(v3); v3 = _mm_xor_si128(v3, v2);
			v0 = _mm_add_epi64(v0, v3); v3 = ExactRotl64x2<21>(v3); v3 = _mm_xor_si128(v3, v0);
			v2 = _mm_add_epi64(v2, v1); v1 = ExactRotl64x2<17>(v1); v1 = _mm_xor_si128(v1, v2);
			v2 = ExactRotl64x2<32>(v2);
		};
		size_t offset = 0;
		for (; offset + 8 <= bytes.size(); offset += 8) {
			unsigned long long word = 0;
			std::memcpy(&word, bytes.data() + offset, sizeof(word));
			__m128i both = _mm_set1_epi64x((long long)word);
			v3 = _mm_xor_si128(v3, both); roundBoth(); roundBoth(); v0 = _mm_xor_si128(v0, both);
		}
		unsigned long long tail = (unsigned long long)bytes.size() << 56;
		for (size_t i = offset; i < bytes.size(); ++i)
			tail |= (unsigned long long)(unsigned char)bytes[i] << (8 * (i - offset));
		__m128i bothTail = _mm_set1_epi64x((long long)tail);
		v3 = _mm_xor_si128(v3, bothTail); roundBoth(); roundBoth(); v0 = _mm_xor_si128(v0, bothTail);
		v2 = _mm_xor_si128(v2, _mm_set1_epi64x(0xff));
		for (int i = 0; i < 4; ++i) roundBoth();
		alignas(16) unsigned long long lanes[2];
		_mm_store_si128(reinterpret_cast<__m128i*>(lanes),
			_mm_xor_si128(_mm_xor_si128(v0, v1), _mm_xor_si128(v2, v3)));
		unsigned long long lo = lanes[0], hi = lanes[1];
		return (size_t)(lo ^ ExactRotl64(hi, 1));
	}
};

class ExactSmallAction {
public:
	static constexpr size_t InlineCount = 4;
	ExactSmallAction() = default;
	ExactSmallAction(std::initializer_list<int> values) { assign(values.begin(), values.end()); }
	ExactSmallAction(const std::vector<int>& values) { assign(values.begin(), values.end()); }
	ExactSmallAction(std::vector<int>&& values) {
		if (values.size() <= InlineCount) assign(values.begin(), values.end());
		else { count = values.size(); overflow = std::make_unique<std::vector<int>>(std::move(values)); }
	}
	ExactSmallAction(const ExactSmallAction& other) { copyFrom(other); }
	ExactSmallAction(ExactSmallAction&&) noexcept = default;
	ExactSmallAction& operator=(const ExactSmallAction& other) {
		if (this != &other) copyFrom(other);
		return *this;
	}
	ExactSmallAction& operator=(ExactSmallAction&&) noexcept = default;
	ExactSmallAction& operator=(std::initializer_list<int> values) { assign(values.begin(), values.end()); return *this; }
	ExactSmallAction& operator=(const std::vector<int>& values) { assign(values.begin(), values.end()); return *this; }
	ExactSmallAction& operator=(std::vector<int>&& values) {
		if (values.size() <= InlineCount) assign(values.begin(), values.end());
		else { count = values.size(); overflow = std::make_unique<std::vector<int>>(std::move(values)); }
		return *this;
	}
	template<class Iterator>
	void assignRange(Iterator first, Iterator last) { assign(first, last); }
	void sort() {
		if (overflow) std::sort(overflow->begin(), overflow->end());
		else std::sort(inlineValues.begin(), inlineValues.begin() + count);
	}
	void push_back(int value) {
		if (!overflow && count < InlineCount) inlineValues[count++] = value;
		else {
			if (!overflow) overflow = std::make_unique<std::vector<int>>(inlineValues.begin(), inlineValues.begin() + count);
			overflow->push_back(value); count++;
		}
	}
	void pop_back() {
		if (count == 0) return;
		if (overflow) overflow->pop_back();
		count--;
	}
	bool empty() const { return count == 0; }
	size_t size() const { return count; }
	int front() const { return (*this)[0]; }
	int operator[](size_t index) const { return overflow ? (*overflow)[index] : inlineValues[index]; }
	void clear() { count = 0; overflow.reset(); }
	operator std::vector<int>() const {
		if (overflow) return *overflow;
		return std::vector<int>(inlineValues.begin(), inlineValues.begin() + count);
	}
	bool operator<(const ExactSmallAction& other) const {
		const size_t common = std::min(count, other.count);
		for (size_t i = 0; i < common; ++i) if ((*this)[i] != other[i]) return (*this)[i] < other[i];
		return count < other.count;
	}
private:
	template<class Iterator>
	void assign(Iterator first, Iterator last) {
		const size_t size = (size_t)std::distance(first, last);
		count = size;
		if (size <= InlineCount) {
			overflow.reset();
			std::copy(first, last, inlineValues.begin());
		} else overflow = std::make_unique<std::vector<int>>(first, last);
	}
	void copyFrom(const ExactSmallAction& other) {
		count = other.count;
		if (other.overflow) overflow = std::make_unique<std::vector<int>>(*other.overflow);
		else { overflow.reset(); std::copy_n(other.inlineValues.begin(), count, inlineValues.begin()); }
	}
	std::array<int, InlineCount> inlineValues{};
	std::unique_ptr<std::vector<int>> overflow;
	size_t count = 0;
};

struct ExactScore {
	ExactFraction lower = ExactFraction::integer(-100'000'000);
	ExactFraction upper = ExactFraction::integer(100'000'000);
	ExactSmallAction action;
	bool certified = false;
	// The interval is mathematically sound even when it is still wide.  A
	// provisional opponent policy must never produce a point interval here;
	// it is widened to the global range before it reaches pruning or TT code.
	bool boundsSound = true;
	// The interval may be sound while public certification is forbidden by a
	// provisional policy, experimental evaluator, or another safety gate.
	bool certificationBlocked = false;
};

struct BoundMergeResult {
	ExactScore score;
	bool contradiction = false;
};

// Intersect two resumable results without allowing an old timeout to erase a
// proof that a later slice (or another worker) has already established.  An
// unsound interval is never used to tighten a sound one.  `certified` is kept
// A point interval is certified when the intersected sound slices prove a
// single value, while a later wide slice cannot revoke that proof.
inline BoundMergeResult mergeSoundBounds(const ExactScore& previous, const ExactScore& fresh) {
	// Once an explicit contradiction has been observed, it must not be healed
	// by a later slice.  A merely unsound estimate, on the other hand, is
	// ignored so that it cannot weaken an already sound interval.
	if (!fresh.boundsSound) {
		if (fresh.certificationBlocked) return { fresh, true };
		return { previous, false };
	}
	if (!previous.boundsSound) {
		if (previous.certificationBlocked) return { previous, true };
		return { fresh, false };
	}
	ExactScore result = previous;
	if (ExactCompare(fresh.lower, result.lower) > 0) result.lower = fresh.lower;
	if (ExactCompare(fresh.upper, result.upper) < 0) result.upper = fresh.upper;
	if (ExactCompare(result.lower, result.upper) > 0) {
		ExactScore invalid;
		invalid.boundsSound = false;
		invalid.certified = false;
		invalid.certificationBlocked = true;
		return { invalid, true };
	}
	if (!fresh.action.empty()) result.action = fresh.action;
	result.boundsSound = true;
	result.certificationBlocked = previous.certificationBlocked || fresh.certificationBlocked;
	// Certification here means that the sound interval has become a point.
	// It must not depend on either slice having independently finished: two
	// incomplete but sound slices can intersect at the exact value.
	result.certified = !result.certificationBlocked
		&& ExactCompare(result.lower, result.upper) == 0;
	return { result, false };
}

struct ExactRootActionValue {
	std::vector<int> action;
	ExactFraction lower;
	ExactFraction upper;
	bool certified = false;
};

// Completed exact entries are immutable, so they can safely be shared by the
// two root workers.  The full canonical byte string remains the equality key;
// SipHash only chooses a bucket/shard.
class ExactSharedTransposition {
public:
	struct FlightState {
		std::string key;
		size_t hash = 0;
		std::thread::id owner;
		std::mutex mutex;
		std::condition_variable ready;
		bool done = false, success = false;
		ExactScore value;
	};
	enum class ClaimStatus { Owner, Completed, Released, TimedOut, SelfDuplicate };
	struct ClaimResult {
		ClaimStatus status = ClaimStatus::TimedOut;
		std::shared_ptr<FlightState> flight;
		ExactScore value;
		bool waited = false;
	};

	bool find(const std::string& key, ExactScore& value, size_t* computedHash = nullptr) const {
		size_t hash = ExactStringHasher{}(key);
		if (computedHash != nullptr) *computedHash = hash;
		Shard& shard = shards[hash & (ShardCount - 1)];
		std::shared_lock<std::shared_mutex> lock(shard.mutex);
		auto found = shard.buckets.find(hash);
		if (found == shard.buckets.end()) return false;
		for (const Entry& entry : found->second) if (shard.keyEquals(entry.key, key)) {
			value = entry.value;
			return true;
		}
		return false;
	}

	bool store(std::string key, const ExactScore& value) {
		const size_t hash = ExactStringHasher{}(key);
		return storeHashed(std::move(key), value, hash);
	}

	bool storeHashed(std::string key, const ExactScore& value, size_t hash) {
		const size_t entryBytes = key.size() + sizeof(ExactScore) + 96;
		if (entryCount.load(std::memory_order_relaxed) >= MaxEntries) return false;
		size_t prior = byteCount.fetch_add(entryBytes, std::memory_order_relaxed);
		if (prior + entryBytes > MaxBytes) {
			byteCount.fetch_sub(entryBytes, std::memory_order_relaxed);
			return false;
		}
		Shard& shard = shards[hash & (ShardCount - 1)];
		std::unique_lock<std::shared_mutex> lock(shard.mutex);
		auto& bucket = shard.buckets[hash];
		for (const Entry& entry : bucket) if (shard.keyEquals(entry.key, key)) {
			byteCount.fetch_sub(entryBytes, std::memory_order_relaxed);
			return false;
		}
		KeyRef keyRef = shard.intern(key, arenaByteCount);
		ExactScore stored = value;
		stored.action.clear();
		bucket.push_back({ keyRef, std::move(stored) });
		entryCount.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	ClaimResult claim(const std::string& key, std::chrono::steady_clock::time_point deadline) {
		const size_t hash = ExactStringHasher{}(key);
		Shard& shard = shards[hash & (ShardCount - 1)];
		std::shared_ptr<FlightState> flight;
		{
			std::unique_lock<std::shared_mutex> lock(shard.mutex);
			auto completed = shard.buckets.find(hash);
			if (completed != shard.buckets.end()) for (const Entry& entry : completed->second)
				if (shard.keyEquals(entry.key, key)) return { ClaimStatus::Completed, {}, entry.value };
			auto& active = shard.flights[hash];
			for (const auto& item : active) if (item->key == key) { flight = item; break; }
			if (!flight) {
				flight = std::make_shared<FlightState>(); flight->key = key; flight->hash = hash;
				flight->owner = std::this_thread::get_id(); active.push_back(flight);
				return { ClaimStatus::Owner, flight, {} };
			}
			if (flight->owner == std::this_thread::get_id())
				return { ClaimStatus::SelfDuplicate, flight, {} };
		}
		std::unique_lock<std::mutex> waitLock(flight->mutex);
		if (!flight->ready.wait_until(waitLock, deadline, [&] { return flight->done; }))
			return { ClaimStatus::TimedOut, flight, {}, true };
		if (flight->success) return { ClaimStatus::Completed, flight, flight->value, true };
		return { ClaimStatus::Released, flight, {}, true };
	}

	void finishFlight(const std::shared_ptr<FlightState>& flight, const ExactScore* value) {
		if (!flight) return;
		Shard& shard = shards[flight->hash & (ShardCount - 1)];
		{
			std::unique_lock<std::shared_mutex> lock(shard.mutex);
			auto found = shard.flights.find(flight->hash);
			if (found != shard.flights.end()) {
				auto& active = found->second;
				active.erase(std::remove(active.begin(), active.end(), flight), active.end());
				if (active.empty()) shard.flights.erase(found);
			}
		}
		{
			std::lock_guard<std::mutex> lock(flight->mutex);
			if (value != nullptr) { flight->value = *value; flight->value.action.clear(); flight->success = true; }
			flight->done = true;
		}
		flight->ready.notify_all();
	}

	size_t bytes() const { return byteCount.load(std::memory_order_relaxed); }
	size_t size() const { return entryCount.load(std::memory_order_relaxed); }
	size_t arenaBytes() const { return arenaByteCount.load(std::memory_order_relaxed); }
	std::uint64_t internExactComponent(const std::string& value) {
		std::lock_guard<std::mutex> lock(componentMutex);
		auto found = componentIds.find(value);
		if (found != componentIds.end()) return found->second;
		const std::uint64_t id = nextComponentId++;
		componentIds.emplace(value, id);
		return id;
	}

private:
	static constexpr size_t ShardCount = 64;
	// The 90-second Rich gate reaches the former 600 MiB completed-entry ceiling
	// after roughly 35 seconds.  Completed nodes are exactly recomputable and the
	// process-wide 2.7 GiB RSS guard remains authoritative, so retain them while
	// there is measured headroom instead of forcing exponential re-expansion.
	static constexpr size_t MaxEntries = 4'000'000;
	static constexpr size_t MaxBytes = 2'560ULL * 1024ULL * 1024ULL;
	struct KeyRef { std::uint32_t block = 0, offset = 0, length = 0; };
	struct Entry { KeyRef key; ExactScore value; };
	struct Shard {
		Shard() { buckets.reserve(8'192); }
		struct ArenaBlock {
			std::unique_ptr<unsigned char[]> data;
			size_t capacity = 0, used = 0;
		};
		KeyRef intern(const std::string& key, std::atomic<size_t>& allocatedBytes) {
			static constexpr size_t BlockBytes = 256ULL * 1024ULL;
			if (blocks.empty() || blocks.back().capacity - blocks.back().used < key.size()) {
				const size_t capacity = std::max(BlockBytes, key.size());
				ArenaBlock block; block.data = std::make_unique<unsigned char[]>(capacity); block.capacity = capacity;
				blocks.push_back(std::move(block)); allocatedBytes.fetch_add(capacity, std::memory_order_relaxed);
			}
			ArenaBlock& block = blocks.back();
			if (blocks.size() > std::numeric_limits<std::uint32_t>::max()
				|| block.used > std::numeric_limits<std::uint32_t>::max()
				|| key.size() > std::numeric_limits<std::uint32_t>::max())
				throw std::overflow_error("exact key arena overflow");
			KeyRef ref{ (std::uint32_t)(blocks.size() - 1), (std::uint32_t)block.used, (std::uint32_t)key.size() };
			std::memcpy(block.data.get() + block.used, key.data(), key.size()); block.used += key.size();
			return ref;
		}
		bool keyEquals(const KeyRef& ref, const std::string& key) const {
			return ref.length == key.size() && ref.block < blocks.size()
				&& std::memcmp(blocks[ref.block].data.get() + ref.offset, key.data(), key.size()) == 0;
		}
		mutable std::shared_mutex mutex;
		std::unordered_map<size_t, std::vector<Entry>> buckets;
		std::unordered_map<size_t, std::vector<std::shared_ptr<FlightState>>> flights;
		std::vector<ArenaBlock> blocks;
	};
	mutable std::array<Shard, ShardCount> shards;
	std::atomic<size_t> byteCount{ 0 };
	std::atomic<size_t> entryCount{ 0 };
	std::atomic<size_t> arenaByteCount{ 0 };
	std::mutex componentMutex;
	std::unordered_map<std::string, std::uint64_t, ExactStringHasher> componentIds;
	std::uint64_t nextComponentId = 1;
};

struct ExactMetrics {
	// Programming-performance counters. They are observational only and must
	// never participate in a search key, bound, or evaluator value.
	unsigned long long stateCopies = 0;
	unsigned long long stateCopyBytes = 0;
	unsigned long long stateCopySampleNs = 0;
	unsigned long long canonicalBuilds = 0;
	unsigned long long canonicalBytes = 0;
	unsigned long long canonicalSampleNs = 0;
	unsigned long long ttReadHits = 0;
	unsigned long long ttReadMisses = 0;
	unsigned long long ttReadSampleNs = 0;
	unsigned long long ttInsertions = 0;
	unsigned long long transitionCacheHits = 0;
	unsigned long long arenaBytes = 0;
	unsigned long long heapAllocations = 0;
	unsigned long long statePoolReuses = 0;
	unsigned long long engineStepCalls = 0;
	unsigned long long engineStepSampleNs = 0;
	unsigned long long actionApplyCalls = 0;
	unsigned long long actionApplySampleNs = 0;
	unsigned long long actionKeyCalls = 0;
	unsigned long long actionKeySampleNs = 0;
	unsigned long long partitionKeyCalls = 0;
	unsigned long long partitionKeySampleNs = 0;
	unsigned long long observationKeyCalls = 0;
	unsigned long long observationKeySampleNs = 0;
	unsigned long long evaluatorCacheHits = 0;
	unsigned long long evaluatorCalls = 0;
	unsigned long long evaluatorSampleNs = 0;
	unsigned long long evaluatorExtractSampleNs = 0;
	unsigned long long evaluatorInferenceSampleNs = 0;
	unsigned long long evaluatorPublicSampleNs = 0;
	unsigned long long evaluatorHiddenSampleNs = 0;
	unsigned long long evaluatorEntitySampleNs = 0;
	unsigned long long workerBusyNs = 0;
	unsigned long long workerWaitNs = 0;
	unsigned long long legacyShadowMismatches = 0;
	unsigned long long packedObservationBuilds = 0;
	unsigned long long packedObservationBytes = 0;
	unsigned long long keyArenaBytes = 0;
	unsigned long long cowFullCopies = 0;
	unsigned long long cowPageCopies = 0;
	unsigned long long cowCopyBytes = 0;
	unsigned long long mutationMisses = 0;
	unsigned long long materializedSnapshots = 0;
	unsigned long long inFlightWaits = 0;
	unsigned long long workerDuplicateClaims = 0;
	unsigned long long exactWeightInlineOps = 0;
	unsigned long long exactWeightSpills = 0;
	unsigned long long evaluatorAccumulatorHits = 0;
	int runtimeVersion = 1;
	int canonicalSchemaVersion = 1;
	unsigned long long expanded = 0;
	unsigned long long merged = 0;
	bool timedOut = false;
	bool arithmeticOverflow = false;
	unsigned long long leaves = 0;
	unsigned long long opaque = 0;
	unsigned long long exceptions = 0;
	std::string lastException;
	int lastPendingDetail = 0;
	unsigned long long unknownOpponentList = 0;
	unsigned long long unsupportedConcreteReference = 0;
	unsigned long long interruptedTransition = 0;
	unsigned long long rawOutcomes = 0;
	unsigned long long groupedOutcomes = 0;
	unsigned long long depthLimitNodes = 0;
	int maxDepth = 0;
	int lastDepthSelectType = 0;
	int lastDepthTurnActionCount = 0;
	int rootWorkers = 1;
	unsigned long long rootQueueLeases = 0;
	unsigned long long rootQueueReassignments = 0;
	unsigned long long rootQueueSteals = 0;
	unsigned long long rootQueueEliminations = 0;
	int lastPendingPlayer = -1;
	int lastPendingEffectCardId = 0;
	int lastPendingEffectPlayer = -1;
	int lastPendingNullCount = 0;
	bool lastPendingDeckUnknown = false;
	unsigned long long policyNodes = 0;
	unsigned long long policyHits = 0;
	unsigned long long policyMisses = 0;
	unsigned long long rerootCount = 0;
	unsigned long long resumedNodes = 0;
	unsigned long long avoidedExpandedNodes = 0;
	unsigned long long partialDecisionNodes = 0;
	unsigned long long partialChanceNodes = 0;
	unsigned long long semanticActionRemaps = 0;
	unsigned long long sessionInvalidations = 0;
	unsigned long long sessionBytes = 0;
	long long deadlineOverrunMs = 0;
	unsigned long long canonicalStateMerges = 0;
	unsigned long long successorMerges = 0;
	unsigned long long distributionMerges = 0;
	unsigned long long rootSharedTTHits = 0;
	unsigned long long beliefWorldsBefore = 0;
	unsigned long long beliefWorldsAfter = 0;
	unsigned long long largestEquivalenceClass = 0;
	unsigned long long resumedActionCount = 0;
	unsigned long long resumedChanceMass = 0;
	int currentRootAction = -1;
	unsigned long long currentRssBytes = 0;
	unsigned long long peakRssBytes = 0;
	bool memoryLimitReached = false;
	bool structurallyBlocked = false;
	unsigned long long partialDecisionHits = 0;
	unsigned long long partialChanceHits = 0;
	unsigned long long partialRevealHits = 0;
	unsigned long long enumeratedHiddenWorlds = 0;
	unsigned long long partialTableBytes = 0;
	unsigned long long rootRetryKeyMatches = 0;
	unsigned long long rootRetryKeyMismatches = 0;
	unsigned long long smallWeightOps = 0;
	unsigned long long bigWeightPromotions = 0;
	unsigned maxWeightBits = 0;
	unsigned long long chanceMassMismatches = 0;
	unsigned long long beliefNodes = 0;
	unsigned long long informationSets = 0;
	unsigned long long strategyFusionPrevented = 0;
	unsigned long long illegalInformationSetSplits = 0;
	unsigned long long attackPreviewExactCount = 0;
	unsigned long long attackPreviewUnavailableCount = 0;
	unsigned long long entityFeatureCount = 0;
	unsigned long long comboFeatureCount = 0;
	unsigned long long provisionalOpponentPolicyNodes = 0;
	unsigned long long boundContradictions = 0;
	unsigned long long dynamicPartitionBuilds = 0;
	unsigned long long dynamicPartitionFallbacks = 0;
	unsigned long long dynamicPartitionCacheHits = 0;
	unsigned long long dynamicPartitionMaxClasses = 0;
	unsigned long long dynamicPartitionMaxVisibleIdentities = 0;
	unsigned long long continuationDraws = 0;
	unsigned long long continuationDrawClasses = 0;
	unsigned long long continuationClassOutcomes = 0;
	unsigned long long continuationConditionalSplits = 0;
	unsigned long long continuationPreparedOutcomes = 0;
	unsigned long long continuationDrawOutcomes = 0;
	unsigned long long continuationCompletedOutcomeNodes = 0;
	unsigned long long continuationMaxOutcomeNodes = 0;
	unsigned long long streamingCursorHits = 0;
	unsigned long long streamingCursorResumes = 0;
	unsigned long long streamingCursorGenerated = 0;
	unsigned long long streamingCursorPeakBytes = 0;
	unsigned long long continuationAtomsMerged = 0;
	unsigned long long v4SemanticEvaluations = 0;
	unsigned long long v4PassiveEvaluations = 0;
	unsigned long long passiveCardsIntegrated = 0;
	unsigned long long passivePairTermsEvaluated = 0;
	unsigned long long passiveExpectationCalls = 0;
	unsigned long long activeCardCount = 0;
	unsigned long long passiveCardCount = 0;
	unsigned long long unknownLivenessCount = 0;
	unsigned long long livenessFallbackCount = 0;
	unsigned long long richActiveOutcomeCount = 0;
	unsigned long long passiveResidualCalls = 0;
	unsigned long long passiveResidualElapsedNs = 0;
	ExactWeight richPassiveIntegratedWeight;
	ExactWeight richTotalChanceWeight;
	unsigned long long livenessAnalysisNs = 0;
	unsigned long long semanticForwardNs = 0;
	unsigned long long passiveExpectationNs = 0;
	bool v4PassiveDrawExperimental = false;
	unsigned long long nestedChancePassiveFallbacks = 0;
	unsigned long long representativeInvariantFallbacks = 0;
	unsigned long long fallbackIncompletePending = 0;
	unsigned long long fallbackIncompleteGlobal = 0;
	unsigned long long fallbackIncompleteCosts = 0;
	unsigned long long fallbackIncompleteSelection = 0;
	unsigned long long fallbackIncompleteConditions = 0;
	unsigned long long fallbackSemanticInvariant = 0;
	unsigned long long fallbackFurtherChance = 0;
	unsigned long long fallbackAnalyticBound = 0;
	unsigned long long fallbackUnknownToken = 0;
	unsigned long long activeDrawEnumerationNs = 0;
	unsigned long long activeOutcomeSolveNs = 0;
	unsigned long long activeOutcomeSolveCount = 0;
	unsigned long long avgSolveOwnedNsPerActiveOutcome = 0;
	unsigned long long skeletonClasses = 0;
	unsigned long long skeletonClassCount = 0;
	unsigned long long skeletonClassMembers = 0;
	unsigned long long skeletonExpansions = 0;
	unsigned long long skeletonSweeps = 0;
	unsigned long long skeletonGuardFallbacks = 0;
	unsigned long long skeletonGuardNodeCap = 0;
	unsigned long long skeletonGuardInertLegal = 0;
	unsigned long long skeletonGuardInteriorChance = 0;
	unsigned long long skeletonGuardIncomplete = 0;
	unsigned long long skeletonGuardOther = 0;
	unsigned long long skeletonNodes = 0;
	unsigned long long skeletonInteriorChances = 0;
	unsigned long long turnInertIdentities = 0;
	unsigned long long macroCollapsedTransitions = 0;
	unsigned long long sleepSetPrunes = 0;
	unsigned long long argmaxDominatedCuts = 0;
	int dynamicPartitionFallbackCardId = 0;
	int dynamicPartitionFallbackEffectType = 0;
	int dynamicPartitionFallbackTargetType = 0;
	bool hiddenInformationLeakDetected = false;
	bool probabilityExact = true;
	bool informationSetSafe = true;
	ExactSkeleton::CertScope certificationScope = ExactSkeleton::CertScope::ExactEvaluatorExpectation;
};

inline bool ExactMetricsCanCertify(const ExactMetrics& metrics) {
	return metrics.informationSetSafe
		&& metrics.probabilityExact
		&& !metrics.arithmeticOverflow
		&& !metrics.hiddenInformationLeakDetected
		&& metrics.provisionalOpponentPolicyNodes == 0
		&& metrics.boundContradictions == 0
		&& !metrics.v4PassiveDrawExperimental;
}

struct ExactDecision {
	ExactScore score;
	ExactMetrics metrics;
	std::vector<ExactRootActionValue> rootActions;
	bool actionValueCertified = false;
	bool bestActionCertified = false;
	bool exactValueCertified = false;
};

// Search states are short-lived and are created at almost every explored edge.
// Keep their vector capacities alive in a worker-local pool so exact search does
// not pay the allocator cost for every mathematically distinct successor.
class ExactStatePool {
public:
	struct CopyStats {
		unsigned long long pageCopies = 0;
		unsigned long long copiedBytes = 0;
		bool fullCopy = false;
		bool mismatch = false;
	};
	template<class T>
	static void copyVector(std::vector<T>& destination, const std::vector<T>& source) {
		if (destination.capacity() < source.size()) destination.reserve(source.size());
		destination.assign(source.begin(), source.end());
	}

	// State contains user-defined FixedList members and padding-sensitive unions;
	// it is not a byte-serializable POD. Use the compiler-generated memberwise
	// assignment so every non-owning pointer, value, and owning vector follows
	// normal C++ object semantics.
	static void copyExactState(State& destination, const State& source) {
		destination = source;
	}

	template<class T>
	static void restoreVector(std::vector<T>& destination, const std::vector<T>& source, CopyStats& stats) {
		bool equal = destination.size() == source.size();
		if (equal && !source.empty()) {
			if constexpr (std::is_trivially_copyable_v<T>)
				equal = std::memcmp(destination.data(), source.data(), source.size() * sizeof(T)) == 0;
			else equal = false;
		}
		if (equal) return;
		copyVector(destination, source);
		stats.pageCopies++;
		stats.copiedBytes += (unsigned long long)source.size() * sizeof(T);
	}

	// The old page-COW path used memcmp/memcpy over State's object representation.
	// That is invalid for FixedList, unions, and padding, so the safe fallback is
	// a normal assignment. The pool still reuses the State allocation itself.
	static void copyCowState(State& destination, const State& source, CopyStats& stats) {
		destination = source;
		stats.fullCopy = true;
		stats.copiedBytes = sizeof(State);
	}

	static bool equalAfterCopy(const State& destination, const State& source) {
		(void)destination;
		(void)source;
		// copyExactState/copyCowState use memberwise assignment, so no object
		// representation comparison is needed or valid here.
		return true;
	}

	static State* acquire(const State& source, bool* allocated = nullptr,
		bool cow = false, bool verify = false, CopyStats* copyStats = nullptr) {
		State* result = nullptr;
		CopyStats localStats;
		if (!freeStates.empty()) {
			result = freeStates.back(); freeStates.pop_back();
			if (cow) copyCowState(*result, source, localStats);
			else copyExactState(*result, source);
			if (allocated != nullptr) *allocated = false;
		} else {
			result = new State(source);
			localStats.fullCopy = true;
			if (allocated != nullptr) *allocated = true;
		}
		// A freshly copy-constructed State is semantically exact, but C++ does not
		// require memberwise copies to preserve padding bytes. Dirty-page auditing
		// applies only to reused slots restored by raw page/vector copies.
		if (verify && !localStats.fullCopy && !equalAfterCopy(*result, source)) localStats.mismatch = true;
		if (copyStats != nullptr) *copyStats = localStats;
		return result;
	}
	static void release(State* state) noexcept {
		if (state == nullptr) return;
		try { freeStates.push_back(state); }
		catch (...) { delete state; }
	}
	static unsigned long long retainedBytes() {
		return (unsigned long long)freeStates.capacity() * sizeof(State);
	}
private:
	static inline thread_local std::vector<State*> freeStates;
};

struct ExactStatePoolDeleter {
	void operator()(State* state) const noexcept { ExactStatePool::release(state); }
};
using ExactStatePtr = std::unique_ptr<State, ExactStatePoolDeleter>;

// The lossless search/TT identity.  EvaluatorRecordV3 is deliberately not
// used here because its Q8 belief projection may merge distinct beliefs.
struct PlayerInformationStateV3 {
	static constexpr int SchemaVersion = 3;
	int observer = 0;
	std::uint64_t environmentPriorId = 0;
	int evaluatorSchema = 0;
	std::uint64_t evaluatorModel = 0;
	std::vector<std::string> normalizedWorlds;
	std::string canonicalKey() const {
		std::string key = "BELIEF-V3|INFORMATION-V3|CANONICAL-V1|RULES-V1|";
		auto append = [&](long long value) { key += std::to_string(value); key.push_back(';'); };
		append(observer); append((long long)environmentPriorId); append(evaluatorSchema); append((long long)evaluatorModel);
		for (const std::string& world : normalizedWorlds) { append((long long)world.size()); key += world; }
		return key;
	}
};

class ExactPlanner {
public:
	ExactPlanner(const int* deck, const int* handValues, int deckCount, int budgetMilliseconds,
		const int* opponentDeck = nullptr, int opponentDeckCount = 0,
		std::shared_ptr<ExactSharedTransposition> sharedTable = nullptr,
		std::shared_ptr<const ExactCpuEvaluator> cpuEvaluator = nullptr)
		: deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, budgetMilliseconds))),
		transposition(sharedTable ? sharedTable : std::make_shared<ExactSharedTransposition>()),
		usingSharedTable(sharedTable != nullptr), evaluator(std::move(cpuEvaluator)) {
		metrics.currentRssBytes = ExactResidentBytes();
		metrics.peakRssBytes = ExactPeakResidentBytes();
		actorDeckSnapshot.assign(deck, deck + deckCount);
		if (handValues != nullptr)
			actorHandValuesSnapshot.assign(handValues, handValues + deckCount);
		else
			actorHandValuesSnapshot.assign((size_t)deckCount, 100);
		if (opponentDeck != nullptr && opponentDeckCount > 0)
			opponentDeckSnapshot.assign(opponentDeck, opponentDeck + opponentDeckCount);
		for (int i = 0; i < deckCount; ++i) {
			actorProfileCount[deck[i]]++;
			handValue[deck[i]] = handValues == nullptr ? 100 : handValues[i];
		}
		std::vector<int> actorIds;
		actorIds.reserve(actorProfileCount.size());
		for (const auto& item : actorProfileCount) actorIds.push_back(item.first);
		std::sort(actorIds.begin(), actorIds.end());
		actorProfileSorted.reserve(actorIds.size());
		for (int id : actorIds) actorProfileSorted.push_back({ id, actorProfileCount[id] });
		if (!actorIds.empty() && actorIds.back() >= 0) actorCardIndexById.assign((size_t)actorIds.back() + 1, -1);
		for (size_t i = 0; i < actorIds.size(); ++i) if (actorIds[i] >= 0)
			actorCardIndexById[(size_t)actorIds[i]] = (signed char)i;
		for (int i = 0; opponentDeck != nullptr && i < opponentDeckCount; ++i) opponentProfileCount[opponentDeck[i]]++;
		std::vector<int> priorCards;
		for (int i = 0; opponentDeck != nullptr && i < opponentDeckCount; ++i) priorCards.push_back(opponentDeck[i]);
		std::sort(priorCards.begin(), priorCards.end());
		environmentPriorId = 1469598103934665603ULL;
		for (int id : priorCards) for (int shift = 0; shift < 32; shift += 8) {
			environmentPriorId ^= (unsigned char)((unsigned)id >> shift); environmentPriorId *= 1099511628211ULL;
		}
		runtimeMode = ExactRuntimeModeFromEnvironment();
		metrics.certificationScope = ExactSkeleton::CertScopeFromEnvironment();
		// Skeleton sharing default-on after Passive-residual + macro-collapse walk fix.
		// Set PTCG_EXACT_SKELETON=0 to force legacy per-outcome.
		skeletonSharingEnabled = true;
#ifdef _WIN32
		{
			char skeletonFlag[8]{};
			DWORD length = GetEnvironmentVariableA("PTCG_EXACT_SKELETON", skeletonFlag, (DWORD)std::size(skeletonFlag));
			if (length > 0 && length < std::size(skeletonFlag)
				&& (skeletonFlag[0] == '0' || skeletonFlag[0] == 'n' || skeletonFlag[0] == 'N'))
				skeletonSharingEnabled = false;
		}
#else
		if (const char* skeletonFlag = std::getenv("PTCG_EXACT_SKELETON");
			skeletonFlag != nullptr && (skeletonFlag[0] == '0' || skeletonFlag[0] == 'n' || skeletonFlag[0] == 'N'))
			skeletonSharingEnabled = false;
#endif
		// Stage 1 safety: V4 Passive draw is opt-in only. Never auto-enable from evaluator version.
		v4PassiveDrawEnabled = false;
		// Identity-oracle gates (tests/test_v4_identity_oracle.py) cover Moment Passive
		// Passive integral mass on the artificial no-Ultra-Ball deck; keep experimental
		// until that suite stays green in CI, then certify.
		v4PassiveDrawCertified = false;
#ifdef _WIN32
		{
			char passiveDraw[8]{};
			DWORD length = GetEnvironmentVariableA("PTCG_EXACT_V4_PASSIVE_DRAW", passiveDraw, (DWORD)std::size(passiveDraw));
			if (length > 0 && length < std::size(passiveDraw)
				&& (passiveDraw[0] == '1' || passiveDraw[0] == 'y' || passiveDraw[0] == 'Y'))
				v4PassiveDrawEnabled = true;
		}
#else
		if (const char* passiveDraw = std::getenv("PTCG_EXACT_V4_PASSIVE_DRAW");
			passiveDraw != nullptr && (passiveDraw[0] == '1' || passiveDraw[0] == 'y' || passiveDraw[0] == 'Y'))
			v4PassiveDrawEnabled = true;
#endif
		v4StripPassiveOnly = false;
		if ((v4PassiveDrawEnabled || (evaluator && evaluator->usesV4Search()))
			&& !v4PassiveDrawCertified)
			metrics.v4PassiveDrawExperimental = true;
		ExactEnsureDeferredFunctionRegistry();
		if (runtimeMode != ExactRuntimeMode::Legacy) {
			metrics.runtimeVersion = 3;
			metrics.canonicalSchemaVersion = 3;
		}
	}

	void setBudgetMilliseconds(int budgetMilliseconds) {
		deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1, budgetMilliseconds));
		metrics.timedOut = false;
	}

	void setAbsoluteDeadline(std::chrono::steady_clock::time_point absoluteDeadline) {
		deadline = absoluteDeadline;
		metrics.timedOut = false;
	}

	// Root-parallel sessions reserve both process-wide permits for their root
	// tasks.  In that mode Chance expansion must remain serial inside each task;
	// one-shot/single-root callers may opt into bounded Chance parallelism.
	void setChanceParallelEnabled(bool enabled) { chanceParallelEnabled = enabled; }
	// Session workers already hold a process-wide permit. Direct one-shot
	// planner callers set nothing and acquire one for the duration of a solve,
	// leaving at most one permit for nested Chance workers.
	void setThreadPermitHeld(bool held) { threadPermitHeld = held; }

	// A second root worker may traverse the same expensive action from the
	// opposite side. Completed descendants are shared through the immutable TT,
	// so the two cores do not spend the deadline on the same prefix.
	void setReverseActionOrder(bool reverse) { reverseActionOrder = reverse; }
	void setConcreteWorldCaching(bool enabled) { concreteWorldCaching = enabled; }
	void setInFlightClaims(bool enabled) { inFlightClaimsEnabled = enabled; }

	// Exact search never exposes engine logs to the caller and rule evaluation
	// does not read them.  Run against a private Game scratch object with log
	// recording disabled so AddLog() is a no-op and the live battle Game is not
	// mutated by manual-coin or temporary engine buffers.
	static void prepareExactRoot(State& root, Game& scratch) {
		if (root.game == nullptr) return;
		scratch = *root.game;
		scratch.config.recordLog = false;
		scratch.config.manualCoin = true;
		root.game = &scratch;
		root.logs.clear();
		root.logIndex = {};
	}

	ExactDecision decide(State root) {
		auto threadLease = threadPermitHeld ? ExactThreadBudget::Lease()
			: ExactGlobalThreadBudget().acquire();
		auto workerStarted = std::chrono::steady_clock::now();
		Game exactGame;
		prepareExactRoot(root, exactGame);
		rootActionValues.clear();
		canonicalMainEnabled = root.selectType == SelectType::Main && root.options.size() > 2;
		nodeQuantumDeadline = std::numeric_limits<unsigned long long>::max();
		actor = root.selectPlayer;
		initializeHidden(root);
		root.game->config.manualCoin = true;
		root.exact.enabled = true;
		root.exact.actor = (signed char)actor;
		ExactDecision result;
		result.score = solveOwned(cloneState(root));
		// A deadline may expire while the root information key is still being
		// initialized, before decision() has visited even one successor.  The
		// exact interval remains the safe global range, but the public API must
		// still return a legal action instead of forcing its caller into a
		// separate emergency policy.  Prefer End at Main; otherwise use the
		// first required physical options.  This action is deliberately left
		// uncertified and carries no invented value information.
		if (result.score.action.empty() && root.selectMin > 0
			&& root.options.size() >= (size_t)root.selectMin) {
			ExactSmallAction provisional;
			if (root.selectType == SelectType::Main && root.selectMin == 1) {
				for (int i = 0; i < (int)root.options.size(); ++i)
					if (root.options[i].type == SelectOptionType::End) {
						provisional.push_back(i);
						break;
					}
			}
			for (int i = 0; provisional.size() < (size_t)root.selectMin; ++i)
				provisional.push_back(i);
			provisional.sort();
			result.score = unknown();
			result.score.action = provisional;
			rootActionValues.push_back({ provisional, result.score.lower,
				result.score.upper, false });
		}
		result.rootActions = rootActionValues;
		const bool certifiable = ExactMetricsCanCertify(metrics);
		result.bestActionCertified = certifiable && result.score.boundsSound
			&& (!result.score.action.empty()
			&& (root.options.size() <= 1 || result.score.certified));
		result.actionValueCertified = certifiable && result.score.boundsSound && result.score.certified;
		result.exactValueCertified = result.bestActionCertified && result.actionValueCertified;
		applyEvaluatorSafety(result);
		metrics.workerBusyNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - workerStarted).count();
		result.metrics = metrics;
		return result;
	}

	ExactDecision evaluateRootAction(State root, int optionIndex) {
		auto threadLease = threadPermitHeld ? ExactThreadBudget::Lease()
			: ExactGlobalThreadBudget().acquire();
		auto workerStarted = std::chrono::steady_clock::now();
		Game exactGame;
		prepareExactRoot(root, exactGame);
		rootActionValues.clear();
		canonicalMainEnabled = root.selectType == SelectType::Main && root.options.size() > 2;
		nodeQuantumDeadline = canonicalMainEnabled ? metrics.expanded + 20'000ULL
			: std::numeric_limits<unsigned long long>::max();
		if (v4PassiveDrawEnabled)
			nodeQuantumDeadline = std::numeric_limits<unsigned long long>::max();
		metrics.currentRootAction = optionIndex;
		actor = root.selectPlayer;
		initializeHidden(root);
		root.game->config.manualCoin = true;
		root.exact.enabled = true;
		root.exact.actor = (signed char)actor;
		ExactDecision result;
		State child = root;
		if (optionIndex < 0 || optionIndex >= (int)root.options.size() || !advance(child, { optionIndex })) {
			result.score = unknown();
		} else {
			std::string retryKey = keyFor(child);
			auto prior = rootSuccessorKeys.find(optionIndex);
			if (prior == rootSuccessorKeys.end()) {
				rootSuccessorKeys.emplace(optionIndex, std::move(retryKey));
			}
			else if (prior->second == retryKey) metrics.rootRetryKeyMatches++;
			else metrics.rootRetryKeyMismatches++;
			result.score = child.exact.pending == ExactPendingType::RevealDeck
				? revealAndReplay(root, child.exact, { optionIndex }) : solveOwned(cloneState(child));
			result.score.action = { optionIndex };
		}
		result.actionValueCertified = ExactMetricsCanCertify(metrics)
			&& result.score.boundsSound && result.score.certified;
		result.bestActionCertified = false;
		result.exactValueCertified = false;
		applyEvaluatorSafety(result);
		metrics.workerBusyNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - workerStarted).count();
		result.metrics = metrics;
		result.rootActions.push_back({ { optionIndex }, result.score.lower, result.score.upper, result.score.certified });
		return result;
	}

	std::string canonicalRootSuccessor(State root, int optionIndex,
		unsigned long long* workEstimate = nullptr) {
		Game exactGame;
		prepareExactRoot(root, exactGame);
		actor = root.selectPlayer;
		initializeHidden(root);
		root.game->config.manualCoin = true;
		root.exact.enabled = true;
		root.exact.actor = (signed char)actor;
		if (optionIndex < 0 || optionIndex >= (int)root.options.size() || !advance(root, { optionIndex })) return {};
		if (workEstimate != nullptr) {
			auto saturatedChoose = [](int n, int k) {
				if (k < 0 || k > n) return 0ULL;
				k = std::min(k, n - k);
				unsigned long long value = 1;
				for (int i = 1; i <= k; ++i) {
					const unsigned long long numerator = (unsigned long long)(n - k + i);
					const unsigned long long divisor = (unsigned long long)i;
					const unsigned long long common0 = std::gcd(numerator, divisor);
					const unsigned long long reducedNumerator = numerator / common0;
					const unsigned long long reducedDivisor = divisor / common0;
					const unsigned long long common1 = std::gcd(value, reducedDivisor);
					value /= common1;
					const unsigned long long tailDivisor = reducedDivisor / common1;
					if (value > std::numeric_limits<unsigned long long>::max() / reducedNumerator)
						return std::numeric_limits<unsigned long long>::max();
					value *= reducedNumerator;
					value /= tailDivisor;
				}
				return value;
			};
			unsigned long long estimate = 1;
			if (root.exact.pending == ExactPendingType::Draw && root.exact.pendingCount > 1) {
				const int player = root.exact.pendingPlayer;
				const int available = player >= 0 && player < 2 ? (int)root.players[player].deck.size() : 0;
				estimate = saturatedChoose(available, root.exact.pendingCount);
				// Keep an immediate multi-draw strictly ahead of ordinary Main
				// successors even for a nearly empty deck.
				if (estimate < 1'000'000ULL) estimate += 1'000'000ULL;
			} else if (root.exact.pending != ExactPendingType::None) {
				estimate = 100'000ULL + (unsigned long long)root.exact.pendingCount;
			} else if (root.selectType == SelectType::Main) {
				estimate = 1ULL + (unsigned long long)root.options.size();
			}
			*workEstimate = estimate;
		}
		return keyFor(root);
	}

	// Re-root a completed turn policy at the currently observed decision.  The
	// key deliberately contains only information exposed by ToJsonApi; hidden
	// materializations therefore cannot leak into a later action.
	bool lookupPolicy(const State& observed, ExactDecision& result) {
		std::string key = observationKeyFor(observed);
		auto it = policy.find(key);
		if (it == policy.end() || it->second.empty()) { metrics.policyMisses++; return false; }
		const ExactPolicyEntry* selected = &it->second.front();
		for (const ExactPolicyEntry& candidate : it->second) {
			if (candidate.actionTokens != selected->actionTokens
				|| ExactCompare(candidate.score.lower, selected->score.lower) != 0
				|| ExactCompare(candidate.score.upper, selected->score.upper) != 0
				|| candidate.actionValueCertified != selected->actionValueCertified
				|| candidate.bestActionCertified != selected->bestActionCertified
				|| candidate.exactValueCertified != selected->exactValueCertified) {
				// The same observation was reached with a different belief.  Until
				// those beliefs are conditioned by a unique history, fail closed.
				metrics.policyMisses++; return false;
			}
		}
		std::vector<int> remapped;
		if (!remapAction(observed, selected->actionTokens, remapped)) { metrics.policyMisses++; return false; }
		if (!ExactMetricsCanCertify(metrics) || !selected->score.boundsSound) {
			// A policy may have been computed before a later worker discovered a
			// certification blocker.  Re-rooting must not resurrect that entry.
			metrics.policyMisses++;
			return false;
		}
		result.score = selected->score;
		result.score.action = std::move(remapped);
		result.actionValueCertified = selected->actionValueCertified;
		result.bestActionCertified = selected->bestActionCertified;
		result.exactValueCertified = selected->exactValueCertified;
		metrics.policyHits++;
		metrics.rerootCount++;
		metrics.semanticActionRemaps++;
		metrics.avoidedExpandedNodes += selected->subtreeExpanded;
		result.metrics = metrics;
		return selected->exactValueCertified && selected->score.certified;
	}

	ExactDecision resume(State root, int budgetMilliseconds) {
		setBudgetMilliseconds(budgetMilliseconds);
		unsigned long long before = metrics.expanded;
		ExactDecision result = decide(std::move(root));
		metrics.resumedNodes += metrics.expanded - before;
		metrics.resumedActionCount++;
		result.metrics = metrics;
		return result;
	}

	const ExactMetrics& currentMetrics() const { return metrics; }
	bool resourceStopped() const { return metrics.memoryLimitReached; }
	void noteBoundContradiction() { ++metrics.boundContradictions; }
	std::string publicObservationKey(const State& observed) {
		return observationKeyFor(observed);
	}

private:
	struct ExactKnowledgeState {
		// deckKnown[target] means this observer has seen the complete current
		// multiset of target's deck.  It is deliberately separate from the deck
		// profile supplied to closed-world validation.
		std::array<bool, 2> deckKnown{};
		std::string publicFacts;
		std::array<std::vector<int>, 2> knownTop;
		std::array<std::vector<int>, 2> knownBottom;
		std::array<std::map<int, int>, 2> knownDeckCounts;
		std::array<std::map<int, int>, 2> knownPrizeCounts;
		std::array<std::map<int, std::pair<int, int>>, 2> countBounds;
		std::uint64_t observationSequence = 0;
	};
	struct BeliefWorld {
		ExactStatePtr state;
		ExactWeight weight;
		std::array<ExactKnowledgeState, 2> knowledge;
	};
	static void appendKnowledgeFact(ExactKnowledgeState& knowledge, char type, int cardId) {
		knowledge.publicFacts.push_back(type); appendSemantic(knowledge.publicFacts, cardId);
		knowledge.observationSequence++;
	}
	static void appendKnowledgeKey(std::string& key, const ExactKnowledgeState& knowledge) {
		for (bool known : knowledge.deckKnown) key.push_back(known ? '1' : '0');
		appendSemantic(key, knowledge.observationSequence);
		appendSemantic(key, (long long)knowledge.publicFacts.size()); key += knowledge.publicFacts;
		auto appendSequence = [&](const auto& lists) {
			for (const auto& list : lists) { appendSemantic(key, (long long)list.size()); for (int id : list) appendSemantic(key, id); }
		};
		auto appendMapArray = [&](const auto& maps) {
			for (const auto& values : maps) { appendSemantic(key, (long long)values.size());
				for (const auto& item : values) { appendSemantic(key, item.first);
					if constexpr (std::is_same_v<std::decay_t<decltype(item.second)>, std::pair<int, int>>) {
						appendSemantic(key, item.second.first); appendSemantic(key, item.second.second);
					} else appendSemantic(key, item.second);
				}
			}
		};
		appendSequence(knowledge.knownTop); appendSequence(knowledge.knownBottom);
		appendMapArray(knowledge.knownDeckCounts); appendMapArray(knowledge.knownPrizeCounts);
		appendMapArray(knowledge.countBounds);
	}
	std::array<ExactKnowledgeState, 2> initialKnowledge() const {
		std::array<ExactKnowledgeState, 2> result{};
		for (const auto& item : actorProfileCount)
			result[actor].countBounds[actor][item.first] = { item.second, item.second };
		int opponent = 1 - actor;
		for (const auto& item : opponentProfileCount)
			result[opponent].countBounds[opponent][item.first] = { item.second, item.second };
		return result;
	}
	struct ExactPolicyEntry {
		ExactScore score;
		std::vector<std::string> actionTokens;
		bool actionValueCertified = false;
		bool bestActionCertified = false;
		bool exactValueCertified = false;
		unsigned long long subtreeExpanded = 0;
	};
	struct PartialDecisionEntry {
		std::unordered_map<std::string, ExactScore, ExactStringHasher> actionBounds;
		size_t resumeOrdinal = 0;
		size_t accountedBytes = 0;
	};
	struct PartialChanceEntry {
		std::unordered_map<int, ExactScore> completedOutcomes;
		size_t accountedBytes = 0;
	};
	struct BoundedCompositionCursor {
		std::vector<int> bounds, values, nextTake, left;
		int depth = 0;
		bool initialized = false, complete = false;

		void reset(const std::vector<int>& newBounds, int total) {
			bounds = newBounds; values.assign(bounds.size(), 0);
			nextTake.assign(bounds.size(), 0); left.assign(bounds.size() + 1, 0);
			left[0] = total; depth = 0; initialized = true; complete = false;
		}

		bool next(std::vector<int>& output) {
			if (!initialized || complete) return false;
			const int count = (int)bounds.size();
			while (true) {
				if (depth == count) {
					bool valid = left[depth] == 0;
					if (valid) output = values;
					if (depth == 0) complete = true; else --depth;
					if (valid) return true;
					continue;
				}
				int maximum = std::min(bounds[depth], left[depth]);
				if (nextTake[depth] <= maximum) {
					int take = nextTake[depth]++;
					values[depth] = take; left[depth + 1] = left[depth] - take;
					++depth;
					if (depth < count) nextTake[depth] = 0;
					continue;
				}
				if (depth == 0) { complete = true; return false; }
				--depth;
			}
		}
	};
	struct PartialRevealEntry {
		BoundedCompositionCursor prizeCursor, handCursor;
		std::vector<int> prizeCounts, handCounts;
		ExactFraction completedLower = ExactFraction::integer(0);
		ExactFraction completedUpper = ExactFraction::integer(0);
		ExactWeight totalWeight, processedWeight, pendingWeight;
		bool initialized = false, handActive = false, pendingWorld = false;
		size_t accountedBytes = 0;
	};
	struct PartialBeliefRevealEntry {
		BoundedCompositionCursor prizeCursor, handCursor;
		std::vector<int> bounds, handBounds, prizeCounts, handCounts;
		std::vector<BeliefWorld> worlds;
		ExactWeight expected, generated;
		bool initialized = false, handActive = false, completed = false;
		size_t accountedBytes = 0;
	};
	struct PartitionRevealAllocation {
		std::vector<int> prizeCounts;
		ExactWeight weight;
	};
	struct PartialPartitionRevealEntry {
		std::vector<PartitionRevealAllocation> allocations;
		size_t index = 0;
		ExactFraction completedLower = ExactFraction::integer(0);
		ExactFraction completedUpper = ExactFraction::integer(0);
		ExactWeight totalWeight, processedWeight;
		size_t accountedBytes = 0;
	};
	struct MultiDrawOutcome {
		std::vector<int> atomCounts;
		ExactWeight weight;
		std::string continuationKey;
		bool hasPassiveExpectation = false;
		ExactBigRational expectedPassiveResidual{};
	};
	// Large multi-draws are generated in count-vector order.  The cursor owns
	// only the bounded composition stack and one output, so interruption keeps
	// its exact position without retaining every possible outcome.
	struct MultiDrawCursor {
		std::vector<std::pair<int, ExactWeight>> types;
		BoundedCompositionCursor composition;
		std::vector<int> counts;
		int drawCount = 0;
		bool initialized = false;

		void reset(const std::vector<std::pair<int, ExactWeight>>& source, int take) {
			types = source; drawCount = take; counts.clear();
			std::vector<int> bounds; bounds.reserve(types.size());
			for (const auto& item : types) {
				if (!item.second.fitsUnsignedLongLong()
					|| item.second.unsignedLongLong() > (unsigned long long)DECK_SIZE) {
					initialized = false; return;
				}
				bounds.push_back((int)item.second.unsignedLongLong());
			}
			composition.reset(bounds, take); initialized = true;
		}

		bool next(MultiDrawOutcome& output) {
			if (!initialized || !composition.next(counts)) return false;
			output = {};
			output.atomCounts = counts;
			output.weight = ExactWeight(1);
			output.continuationKey = "STREAM|";
			for (size_t i = 0; i < counts.size(); ++i) {
				output.weight = ExactWeight::multiply(output.weight,
					chooseCount((int)types[i].second.unsignedLongLong(), counts[i]));
				output.continuationKey += std::to_string(counts[i]);
				output.continuationKey.push_back(',');
			}
			return true;
		}

		size_t bytes() const {
			return sizeof(*this) + types.size() * sizeof(types[0])
			+ composition.bounds.size() * sizeof(int) * 4 + counts.size() * sizeof(int);
		}
	};
	struct PartialMultiDrawEntry {
		std::vector<int> bounds;
		std::string continuationSchema;
		std::vector<MultiDrawOutcome> outcomes;
		std::vector<unsigned char> outcomeDone;
		MultiDrawCursor cursor;
		MultiDrawOutcome pendingOutcome;
		size_t outcomeIndex = 0;
		ExactFraction completedLower = ExactFraction::integer(0);
		ExactFraction completedUpper = ExactFraction::integer(0);
		ExactWeight totalWeight, processedWeight, pendingWeight;
		bool initialized = false, pending = false, streaming = false;
		bool skeletonAttempted = false;
		unsigned long long pendingExpandedNodes = 0;
		size_t accountedBytes = 0;
	};
	std::unordered_map<int, int> actorProfileCount;
	std::vector<std::pair<int, int>> actorProfileSorted;
	std::vector<signed char> actorCardIndexById;
	std::unordered_map<int, int> opponentProfileCount;
	std::unordered_map<int, int> handValue;
	std::vector<int> actorDeckSnapshot;
	std::vector<int> actorHandValuesSnapshot;
	std::vector<int> opponentDeckSnapshot;
	std::uint64_t environmentPriorId = 0;
	std::shared_ptr<const ExactCpuEvaluator> evaluator;
	std::chrono::steady_clock::time_point deadline;
	mutable ExactMetrics metrics;
	int actor = 0;
	// Two fixed SipHash-2-4 digests index the table; std::string equality still
	// compares every canonical byte, so a digest collision cannot merge states.
	std::shared_ptr<ExactSharedTransposition> transposition;
	std::unordered_map<std::string, ExactScore, ExactStringHasher> localTransposition;
	bool usingSharedTable = false;
	std::vector<ExactRootActionValue> rootActionValues;
	std::unordered_map<int, std::string> rootSuccessorKeys;
	std::unordered_map<std::string, std::vector<ExactPolicyEntry>, ExactStringHasher> policy;
	std::unordered_map<std::string, PartialDecisionEntry, ExactStringHasher> partialDecisions;
	std::unordered_map<std::string, PartialChanceEntry, ExactStringHasher> partialChances;
	std::unordered_map<std::string, PartialRevealEntry, ExactStringHasher> partialReveals;
	std::unordered_map<std::string, PartialBeliefRevealEntry, ExactStringHasher> partialBeliefReveals;
	// std::map keeps the outer reveal entry stable when a searched card starts a
	// nested partitioned reveal and inserts another resumable entry.
	std::map<std::string, PartialPartitionRevealEntry> partialPartitionReveals;
	std::map<std::string, PartialMultiDrawEntry> partialMultiDraws;
	std::unordered_map<std::string, ExactScore, ExactStringHasher> beliefTransposition;
	// A completed (state, semantic-action) transition is reusable even when the
	// parent decision node itself was only reached through a different path.
	// Only certified values enter this cache; partial bounds remain in the
	// resumable per-node table and can never be mistaken for a complete result.
	std::unordered_map<std::string, ExactScore, ExactStringHasher> transitionScoreCache;
	mutable std::unordered_map<std::string, long long, ExactStringHasher> evaluationCache;
	// Dynamic turn search quotient.  The key retains the complete public
	// state, semantic search options, and every deck count that can affect a later
	// query before the turn leaf, while omitting identities proven irrelevant.
	std::unordered_map<std::string, ExactScore, ExactStringHasher> partitionTurnRevealScores;
	std::unordered_map<std::string, ExactScore, ExactStringHasher> partitionTurnMainScores;
	struct PartitionDependencyKey {
		std::array<int, DECK_SIZE> populationId{};
		std::array<int, DECK_SIZE> populationCount{};
		std::array<int, DECK_SIZE> reachable{};
		unsigned char populationSize = 0;
		unsigned char reachableSize = 0;
		bool drawContinuationOnly = false;
		bool earlyTurn = false;
		bool hasPending = false;
		bool v4PassivePartition = false;
		std::uint64_t prePartitionProofHash = 0;
		int pendingPlayer = 0, pendingSkillId = 0, pendingEffectIndex = 0, pendingDetail = 0;
		bool operator==(const PartitionDependencyKey& other) const {
			return populationSize == other.populationSize && reachableSize == other.reachableSize
				&& drawContinuationOnly == other.drawContinuationOnly && earlyTurn == other.earlyTurn
				&& hasPending == other.hasPending && v4PassivePartition == other.v4PassivePartition
				&& prePartitionProofHash == other.prePartitionProofHash
				&& pendingPlayer == other.pendingPlayer
				&& pendingSkillId == other.pendingSkillId && pendingEffectIndex == other.pendingEffectIndex
				&& pendingDetail == other.pendingDetail
				&& std::equal(populationId.begin(), populationId.begin() + populationSize, other.populationId.begin())
				&& std::equal(populationCount.begin(), populationCount.begin() + populationSize, other.populationCount.begin())
				&& std::equal(reachable.begin(), reachable.begin() + reachableSize, other.reachable.begin());
		}
	};
	struct PartitionDependencyHasher {
		size_t operator()(const PartitionDependencyKey& key) const noexcept {
			std::uint64_t hash = 1469598103934665603ULL;
			auto add = [&](std::uint64_t value) { hash ^= value; hash *= 1099511628211ULL; };
			add(key.populationSize); add(key.reachableSize); add(key.drawContinuationOnly);
			add(key.earlyTurn); add(key.hasPending); add(key.v4PassivePartition);
			add(key.prePartitionProofHash);
			for (size_t i = 0; i < key.populationSize; ++i) { add((std::uint32_t)key.populationId[i]); add((std::uint32_t)key.populationCount[i]); }
			for (size_t i = 0; i < key.reachableSize; ++i) add((std::uint32_t)key.reachable[i]);
			if (key.hasPending) { add((std::uint32_t)key.pendingPlayer); add((std::uint32_t)key.pendingSkillId);
				add((std::uint32_t)key.pendingEffectIndex); add((std::uint32_t)key.pendingDetail); }
			return (size_t)hash;
		}
	};
	struct PartitionAnalysis {
		ExactCardPartition partition;
		std::string schema;
		std::vector<int> visibleIds;
		std::array<signed char, DECK_SIZE> visiblePositionByProfile{};
		bool compressed = false;
		PartitionAnalysis() { visiblePositionByProfile.fill(-1); }
	};
	std::unordered_map<PartitionDependencyKey, PartitionAnalysis, PartitionDependencyHasher> partitionAnalysisCache;
	PartitionAnalysis uncachedPartitionAnalysis;
	std::unordered_map<int, std::vector<long long>> continuationIdentityCache;
	ExactRuntimeMode runtimeMode = ExactRuntimeMode::Legacy;
	static constexpr size_t PackedScratchCapacity = 512;
	mutable std::array<ExactPackedDescriptor, PackedScratchCapacity> packedDescriptors;
	mutable std::array<std::uint16_t, PackedScratchCapacity> packedOrder{};
	mutable std::array<ExactPackedDescriptor, 256> packedCardCache;
	mutable std::array<std::uint64_t, 256> packedCardCacheGeneration{};
	mutable std::uint64_t packedObservationGeneration = 1;
	// Index 0 is unused by solveOwned's depth guard. Each active frame owns its
	// key until it returns; later siblings reuse the retained string capacity.
	mutable std::array<std::string, 386> partitionKeyScratch;
	mutable std::unordered_map<std::string, std::string, ExactStringHasher> shadowLegacyToPacked;
	mutable std::unordered_map<std::string, std::string, ExactStringHasher> shadowPackedToLegacy;
	size_t beliefTranspositionBytes = 0;
	static constexpr size_t MaxPolicyEntries = 100'000;
	static constexpr size_t MaxTransitionCacheEntries = 200'000;
	static constexpr size_t MaxPolicyBytes = 64ULL * 1024ULL * 1024ULL;
	static constexpr size_t MaxLocalTranspositionEntries = 250'000;
	static constexpr size_t MaxLocalTranspositionBytes = 200ULL * 1024ULL * 1024ULL;
	static constexpr size_t MaxPartialEntries = 50'000;
	static constexpr size_t MaxPartialBytes = 64ULL * 1024ULL * 1024ULL;
	size_t localTranspositionBytes = 0;
	size_t partialBytes = 0;
	size_t policyBytes = 0;
	int recursionDepth = 0;
	bool canonicalMainEnabled = false;
	bool reverseActionOrder = false;
	bool skeletonSharingEnabled = false;
	bool v4PassiveDrawEnabled = false;
	bool v4PassiveDrawCertified = false; // Moment Passive + identity oracle gates
	bool v4StripPassiveOnly = false;
	ExactPassivePayloadV4 chanceNodeBasePassive;
	bool concreteWorldCaching = false;
	bool chanceParallelEnabled = true;
	bool threadPermitHeld = false;
	// Enabled only by a future shared-frontier scheduler. Root-parity profiling
	// showed no simultaneous claims, so allocating a flight record per node is
	// deliberately not part of the standard cow path yet.
	bool inFlightClaimsEnabled = false;
	// Full own-deck reveals produce singleton information sets. Turn sessions may
	// opt into concreteWorldCaching for large later-turn DAGs; one-shot calls keep
	// the lower-overhead streaming path.
	bool singletonRevealStreaming = false;
	unsigned long long resourceCheckCounter = 0;
	unsigned long long nodeQuantumDeadline = std::numeric_limits<unsigned long long>::max();

	void updateAccountedBytes(size_t& entryBytes, size_t newBytes) {
		if (newBytes >= entryBytes) {
			partialBytes += newBytes - entryBytes;
		} else {
			partialBytes -= std::min(partialBytes, entryBytes - newBytes);
		}
		entryBytes = newBytes;
	}

	bool expired() {
		if (metrics.memoryLimitReached) return true;
		if (metrics.expanded >= nodeQuantumDeadline) return true;
		if ((++resourceCheckCounter & 4095ULL) == 0) {
			unsigned long long rss = ExactResidentBytes();
			metrics.currentRssBytes = rss;
			metrics.peakRssBytes = std::max(metrics.peakRssBytes, ExactPeakResidentBytes());
			if (rss >= 2'700ULL * 1024ULL * 1024ULL) {
				metrics.memoryLimitReached = true;
				return true;
			}
		}
		if (std::chrono::steady_clock::now() < deadline) return false;
		metrics.timedOut = true;
		metrics.deadlineOverrunMs = std::max<long long>(metrics.deadlineOverrunMs,
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - deadline).count());
		return true;
	}

	static unsigned long long stateDynamicBytes(const State& state) {
		unsigned long long bytes = sizeof(State);
		auto add = [&](const auto& list) {
			bytes += (unsigned long long)list.size()
				* sizeof(typename std::decay_t<decltype(list)>::value_type);
		};
		add(state.options); add(state.selected); add(state.preTargetList); add(state.targetList);
		add(state.koList); add(state.delayTriggerStack); add(state.temporaryTriggerStack);
		add(state.triggerStack); add(state.turnUsedSkill); add(state.turnPlay); add(state.turnHeal);
		add(state.turnEvolve); add(state.functionStack); add(state.logs);
		return bytes;
	}

	void stepExact(State& state) {
		const bool sample = (metrics.engineStepCalls & 1023ULL) == 0;
		auto started = sample ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		state.step();
		metrics.engineStepCalls++;
		if (sample) metrics.engineStepSampleNs += (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count();
	}

	struct HotTimer {
		unsigned long long& calls;
		unsigned long long& sampleNs;
		bool sample;
		std::chrono::steady_clock::time_point started;
		HotTimer(unsigned long long& callCounter, unsigned long long& samples)
			: calls(callCounter), sampleNs(samples), sample((callCounter & 1023ULL) == 0),
			started(sample ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
		~HotTimer() {
			calls++;
			if (sample) sampleNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count();
		}
	};

	ExactStatePtr cloneState(const State& source) {
		const bool sample = (metrics.stateCopies & 1023ULL) == 0;
		auto started = sample ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		bool allocated = false;
		ExactStatePool::CopyStats copyStats;
		// Packed production search uses the pooled bulk restore. On the measured
		// 24 KiB State prefix it is faster than issuing roughly ninety 256-byte
		// memcmp calls per edge, despite copying more bytes. Keep conservative
		// page-COW in shadow mode as an independent restoration audit.
		const bool packedBulkRestore = runtimeMode == ExactRuntimeMode::Cow;
		const bool useCow = runtimeMode == ExactRuntimeMode::Shadow;
		ExactStatePtr result(ExactStatePool::acquire(source, &allocated, useCow,
			runtimeMode == ExactRuntimeMode::Shadow, &copyStats));
		metrics.stateCopies++;
		if (useCow) {
			metrics.cowPageCopies += copyStats.pageCopies;
			metrics.cowCopyBytes += copyStats.copiedBytes;
			if (copyStats.fullCopy) {
				metrics.cowFullCopies++; metrics.materializedSnapshots++;
				metrics.cowCopyBytes += stateDynamicBytes(source);
			}
			if (copyStats.mismatch) {
				metrics.mutationMisses++; metrics.informationSetSafe = false;
			}
		}
		if (packedBulkRestore) metrics.cowFullCopies++;
		// This is an observational counter.  Sample the dynamic-size walk with
		// the same cadence as the copy timer; search semantics never depend on it.
		if (sample) {
			const unsigned long long bytes = stateDynamicBytes(source);
			if (bytes <= std::numeric_limits<unsigned long long>::max() / 1024ULL
				&& metrics.stateCopyBytes <= std::numeric_limits<unsigned long long>::max() - bytes * 1024ULL)
				metrics.stateCopyBytes += bytes * 1024ULL;
			else metrics.stateCopyBytes = std::numeric_limits<unsigned long long>::max();
			if (packedBulkRestore) {
				if (bytes <= std::numeric_limits<unsigned long long>::max() / 1024ULL
					&& metrics.cowCopyBytes <= std::numeric_limits<unsigned long long>::max() - bytes * 1024ULL)
					metrics.cowCopyBytes += bytes * 1024ULL;
				else metrics.cowCopyBytes = std::numeric_limits<unsigned long long>::max();
			}
		}
		if (allocated) metrics.heapAllocations++; else metrics.statePoolReuses++;
		if (sample) metrics.stateCopySampleNs += (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count();
		return result;
	}

	bool sharedFind(const std::string& key, ExactScore& value, size_t* computedHash = nullptr) {
		const bool sample = ((metrics.ttReadHits + metrics.ttReadMisses) & 1023ULL) == 0;
		auto started = sample ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		bool found = transposition != nullptr && transposition->find(key, value, computedHash);
		if (sample) metrics.ttReadSampleNs += (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count();
		if (transposition != nullptr) metrics.arenaBytes = transposition->arenaBytes();
		if (found) metrics.ttReadHits++; else metrics.ttReadMisses++;
		return found;
	}

	bool sharedStore(std::string key, const ExactScore& value, const size_t* computedHash = nullptr) {
		if (transposition == nullptr) return false;
		bool inserted = computedHash == nullptr
			? transposition->store(std::move(key), value)
			: transposition->storeHashed(std::move(key), value, *computedHash);
		metrics.arenaBytes = transposition->arenaBytes();
		if (inserted) metrics.ttInsertions++;
		return inserted;
	}

	std::string keyFor(const State& input) const {
		const bool sample = (metrics.canonicalBuilds & 1023ULL) == 0;
		auto started = sample ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		std::string result = ExactCanonicalState::Build(input);
		metrics.canonicalBuilds++;
		metrics.canonicalBytes += result.size();
		if (sample) metrics.canonicalSampleNs += (unsigned long long)
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started).count();
		return result;
	}

	static bool targetIncludesDeck(const Target& target) {
		for (AreaType area : target.areas) if (area == AreaType::Deck) return true;
		return false;
	}

	static bool exposesArbitraryDeckIdentity(EffectType type) {
		switch (type) {
		case EffectType::Draw:
		case EffectType::DrawTargetCount:
		case EffectType::DrawPrizeCount:
		case EffectType::DrawUntil:
		case EffectType::DrawUntilPsychic:
		case EffectType::DrawMirror:
		case EffectType::LookDeck:
		case EffectType::LookDeckReverse:
		case EffectType::LookDeckBottom:
		case EffectType::DeckToTrash:
		case EffectType::DeckToTrashCoinUntilTail:
		case EffectType::DeckBottomToTrash:
		case EffectType::SwitchDeck:
			return true;
		default:
			return false;
		}
	}

	static bool isDrawEffect(EffectType type) {
		switch (type) {
		case EffectType::Draw: case EffectType::DrawTargetCount:
		case EffectType::DrawPrizeCount: case EffectType::DrawUntil:
		case EffectType::DrawUntilPsychic: case EffectType::DrawMirror: return true;
		default: return false;
		}
	}

	std::vector<long long> continuationIdentityKey(int cardId) {
		auto cached = continuationIdentityCache.find(cardId);
		if (cached != continuationIdentityCache.end()) return cached->second;
		std::vector<long long> key;
		if (evaluator && evaluator->isLoaded()) {
			auto model = evaluator->cardContinuationSignature(cardId);
			key.reserve(model.size() + 96);
			for (std::int16_t value : model) key.push_back(value);
		} else key.push_back(cardId);
		const CardMaster* found = FindCardMaster(cardId);
		if (found == nullptr) { key.push_back(cardId); continuationIdentityCache[cardId] = key; return key; }
		const CardMaster& card = *found;
		key.push_back((int)card.cardType); key.push_back((int)card.pokemonType);
		key.push_back((int)card.evolutionType); key.push_back((int)card.energyType);
		key.push_back(card.energyCount); key.push_back(card.hp);
		// Pokemon and Energy can carry identity-specific evolution, attack, Ability,
		// or attachment semantics which are not completely represented by the V3
		// leaf accumulator. Keep them singleton unless a future compiler proves a
		// complete continuation signature for those rule objects.
		if (card.cardType == CardType::Pokemon || IsEnergy(card.cardType))
			key.push_back(cardId);
		auto appendText = [&](const std::u8string& text) {
			key.push_back(-1); key.push_back((long long)text.size());
			for (char8_t value : text) key.push_back((unsigned char)value);
		};
		// V3 combo features derive evolutionary relations from these strings.
		if (card.cardType == CardType::Pokemon) {
			appendText(card.name); appendText(card.nameEn);
			appendText(card.evolvesFrom); appendText(card.evolvesFrom2);
		}
		continuationIdentityCache[cardId] = key; return key;
	}

	// Transition-only key: rules / types / play structure. No V3 weight signature and
	// no cardId singleton — used only for Passive-proven identities.
	std::vector<long long> transitionContinuationKey(int cardId) {
		std::vector<long long> key;
		const CardMaster* found = FindCardMaster(cardId);
		if (found == nullptr) { key.push_back(cardId); return key; }
		const CardMaster& card = *found;
		key.push_back((int)card.cardType);
		key.push_back((int)card.pokemonType);
		key.push_back((int)card.evolutionType);
		key.push_back((int)card.energyType);
		key.push_back(card.energyCount);
		key.push_back(card.hp);
		key.push_back(card.canPlayFirstTurn ? 1 : 0);
		auto appendSkill = [&](const Skill* skill) {
			if (skill == nullptr) { key.push_back(-2); return; }
			key.push_back((long long)skill->effects.size());
			for (const Effect& effect : skill->effects) {
				key.push_back(effect.isCondition ? 1 : 0);
				key.push_back((int)effect.effectType);
				key.push_back((int)effect.conditionType);
				key.push_back((int)effect.comparatorType);
				key.push_back((int)effect.target.targetPlayer);
				key.push_back((long long)effect.target.areas.size());
				for (AreaType area : effect.target.areas) key.push_back((int)area);
				key.push_back((long long)effect.target.conditions.size());
				for (const auto& cond : effect.target.conditions) {
					key.push_back((int)cond.targetType);
					key.push_back((int)cond.comparatorType);
					key.push_back(cond.val);
					key.push_back(cond.val2);
				}
				for (int v : effect.values) key.push_back(v);
			}
		};
		appendSkill(card.play);
		for (const Skill* skill : card.getSkills()) appendSkill(skill);
		return key;
	}

	const PartitionAnalysis& turnDependencyPartition(const State& state,
		const ExactHiddenState* pending = nullptr, bool drawContinuationOnly = false) {
		PartitionDependencyKey dependencyKey;
		dependencyKey.drawContinuationOnly = drawContinuationOnly;
		dependencyKey.earlyTurn = state.turn <= 2;
		dependencyKey.v4PassivePartition = v4PassiveDrawEnabled && drawContinuationOnly;
		for (int i = 0; i < state.exact.typeCount[actor]; ++i) {
			if (state.exact.cardCount[actor][i] <= 0) continue;
			if (dependencyKey.populationSize >= DECK_SIZE) throw std::length_error("partition population overflow");
			const size_t index = dependencyKey.populationSize++;
			dependencyKey.populationId[index] = state.exact.cardId[actor][i];
			dependencyKey.populationCount[index] = state.exact.cardCount[actor][i];
		}
		// The dependency set is tiny (bounded by the fixed deck).  A sorted flat
		// array avoids allocations while preserving the exact lexical order.
		auto addReachable = [&](int id) {
			auto begin = dependencyKey.reachable.begin();
			auto end = begin + dependencyKey.reachableSize;
			auto pos = std::lower_bound(begin, end, id);
			if (pos != end && *pos == id) return;
			if (dependencyKey.reachableSize >= DECK_SIZE) throw std::length_error("partition reachable overflow");
			std::move_backward(pos, end, end + 1); *pos = id; dependencyKey.reachableSize++;
		};
		auto hasReachable = [&](int id) {
			return std::binary_search(dependencyKey.reachable.begin(),
				dependencyKey.reachable.begin() + dependencyKey.reachableSize, id);
		};
		auto addRef = [&](AreaType area, int index) {
			try {
				CardRef ref = state.getCardRef(area, index, actor);
				if (!ref.isNull()) addReachable(state.getCard(ref).cardId);
			} catch (...) {}
		};
		// Seed from every owned card that can already act or can become playable
		// later in this turn.  Current legality alone is insufficient: benching a
		// Pokemon or changing a restriction may enable a card after this node.
		for (const SelectOption& option : state.options) {
			switch (option.type) {
			case SelectOptionType::Play: addRef(AreaType::Hand, option.param0); break;
			case SelectOptionType::Attach:
			case SelectOptionType::Evolve: addRef((AreaType)option.param0, option.param1); break;
			case SelectOptionType::Ability: addRef((AreaType)option.param0, option.param1); break;
			case SelectOptionType::Skill: {
				auto skill = SkillTable.find(option.param0);
				if (skill != SkillTable.end()) addReachable(skill->second.cardId);
				break;
			}
			default: break;
			}
		}
		for (CardRef ref : state.players[actor].hand) if (!ref.isNull()) {
			int cardId = state.getCard(ref).cardId;
			const CardMaster* master = FindCardMaster(cardId);
			if (master == nullptr) { addReachable(cardId); continue; }
			// Once the once-per-turn attachment has been consumed, an Energy
			// subsequently exposed by a search/draw cannot become an operator this
			// turn. Treating Enriching Energy as reachable here forced every deck
			// identity to split solely because its attach effect draws four cards.
			if (state.energyPlayed && IsEnergy(master->cardType)) continue;
			if (state.stadiumPlayed && master->cardType == CardType::Stadium) continue;
			// A Supporter absent from the legal option list cannot become legal
			// later in the same turn (first-turn prohibition or already used).
			// Evolution cards are likewise unreachable during either player's
			// first turn unless the engine already exposed a legal effect option.
			bool firstTurnEvolution = state.turn <= 2
				&& (master->evolutionType == EvolutionType::Stage1
					|| master->evolutionType == EvolutionType::Stage2);
			// Other cards may become legal after a bench/target/stadium change.
			if ((!firstTurnEvolution && master->cardType != CardType::Supporter)
				|| hasReachable(cardId))
				addReachable(cardId);
		}
		// A draw reveals the concrete card to its owner. Any card which could become
		// a legal operator later in this turn must therefore be an identity-visible
		// continuation class before the draw is aggregated. This closes the strategy
		// fusion hole where two currently-in-deck Items shared model weights but
		// offered different actions after being drawn.
		if (drawContinuationOnly) {
			const PlayerState& player = state.players[actor];
			for (size_t atomIndex = 0; atomIndex < dependencyKey.populationSize; ++atomIndex) {
				const int atomCardId = dependencyKey.populationId[atomIndex];
				const CardMaster* found = FindCardMaster(atomCardId);
				if (found == nullptr) { addReachable(atomCardId); continue; }
				const CardMaster& card = *found;
				bool exhausted = IsEnergy(card.cardType) && state.energyPlayed;
				exhausted = exhausted || (card.cardType == CardType::Supporter
					&& (state.supporterPlayed || player.thisTurn.cannotPlaySupporter
						|| (state.turn <= 1 && !card.canPlayFirstTurn)));
				exhausted = exhausted || (card.cardType == CardType::Stadium
					&& (state.stadiumPlayed || player.cannotPlayStadium
						|| player.thisTurn.cannotPlayStadium));
				// Neither player may evolve on turn 1–2; Stage cards drawn this turn
				// cannot become operators and are Passive-eligible under V4.
				exhausted = exhausted || (state.turn <= 2
					&& (card.evolutionType == EvolutionType::Stage1
						|| card.evolutionType == EvolutionType::Stage2));
				// Items whose play skill requires a later turn (e.g. Rare Candy) are
				// not operators for the remainder of an early turn.
				if (!exhausted && card.play != nullptr) {
					for (const Effect& effect : card.play->effects) {
						if (!effect.isCondition) break;
						if (effect.conditionType != ConditionType::Turn) continue;
						const int need = effect.values[0];
						if (effect.comparatorType == ComparatorType::GreaterEqual && state.turn < need) exhausted = true;
						if (effect.comparatorType == ComparatorType::Greater && state.turn <= need) exhausted = true;
					}
				}
				if (!exhausted) addReachable(atomCardId);
			}
		}
		dependencyKey.hasPending = pending != nullptr;
		if (pending != nullptr) {
			dependencyKey.pendingPlayer = pending->pendingPlayer;
			dependencyKey.pendingSkillId = pending->pendingSkillId;
			dependencyKey.pendingEffectIndex = pending->pendingEffectIndex;
			dependencyKey.pendingDetail = pending->pendingDetail;
		}
		PartitionDependencyKey cacheKey = dependencyKey;
		auto cachedPartition = partitionAnalysisCache.find(cacheKey);
		if (cachedPartition != partitionAnalysisCache.end()) {
			metrics.dynamicPartitionCacheHits++;
			return cachedPartition->second;
		}
		std::vector<ExactCardAtom> population;
		population.reserve(dependencyKey.populationSize);
		for (size_t i = 0; i < dependencyKey.populationSize; ++i)
			population.push_back({ dependencyKey.populationId[i], dependencyKey.populationCount[i] });
		ExactCardPartition partition(population);

		bool exposeAll = false;
		int dependencyCardId = 0;
		int dependencyEffectType = 0;
		auto applyTarget = [&](const Target& target) {
			if (!targetIncludesDeck(target)) return;
			std::vector<int> matching;
			auto addMatching = [&](int id) {
				auto pos = std::lower_bound(matching.begin(), matching.end(), id);
				if (pos == matching.end() || *pos != id) matching.insert(pos, id);
			};
			for (const ExactCardAtom& atom : population) {
				const CardMaster* master = FindCardMaster(atom.cardId);
				if (master == nullptr) { exposeAll = true; return; }
				ExactStaticTargetResult result = ExactStaticTargetMatches(*master, target);
				if (!result.supported) {
					exposeAll = true;
					metrics.dynamicPartitionFallbackCardId = dependencyCardId;
					metrics.dynamicPartitionFallbackEffectType = dependencyEffectType;
					metrics.dynamicPartitionFallbackTargetType = (int)target.conditions.front().targetType;
					return;
				}
				if (result.matches) addMatching(atom.cardId);
			}
			partition.refineVisible([&](int cardId) { return std::binary_search(matching.begin(), matching.end(), cardId); });
			for (int cardId : matching) {
				const CardMaster* master = FindCardMaster(cardId);
				if (master == nullptr) { exposeAll = true; return; }
				// Neither player can evolve during their first turn.  A searched
				// evolution card is observable (and remains a singleton class), but
				// its evolve-time Skill is not a reachable operator this turn.
				if (state.turn <= 2 && (master->evolutionType == EvolutionType::Stage1
					|| master->evolutionType == EvolutionType::Stage2)) continue;
				addReachable(cardId);
			}
		};

		if (pending != nullptr && pending->pendingSkillId > 0 && pending->pendingEffectIndex >= 0) {
			auto skill = SkillTable.find(pending->pendingSkillId);
			if (skill == SkillTable.end() || pending->pendingEffectIndex >= (int)skill->second.effects.size()) exposeAll = true;
			else {
				dependencyCardId = skill->second.cardId;
				dependencyEffectType = (int)skill->second.effects[pending->pendingEffectIndex].effectType;
				applyTarget(skill->second.effects[pending->pendingEffectIndex].target);
			}
		}

		std::vector<int> scanned;
		while (!exposeAll) {
			auto reachableBegin = dependencyKey.reachable.begin();
			auto reachableEnd = reachableBegin + dependencyKey.reachableSize;
			auto next = std::find_if(reachableBegin, reachableEnd, [&](int id) {
				return !std::binary_search(scanned.begin(), scanned.end(), id);
			});
			if (next == reachableEnd) break;
			int id = *next;
			if (!std::binary_search(scanned.begin(), scanned.end(), id)) {
				scanned.insert(std::lower_bound(scanned.begin(), scanned.end(), id), id);
			}
			dependencyCardId = id;
			const CardMaster* master = FindCardMaster(id);
			if (master == nullptr) { exposeAll = true; break; }
			for (const Skill* skill : master->getSkills()) if (skill != nullptr) {
				for (const Effect& effect : skill->effects) {
					dependencyEffectType = (int)effect.effectType;
					bool arbitraryIdentity = exposesArbitraryDeckIdentity(effect.effectType);
					if (arbitraryIdentity && isDrawEffect(effect.effectType) && drawContinuationOnly
						&& evaluator && evaluator->isLoaded()) arbitraryIdentity = false;
					if (arbitraryIdentity) {
						exposeAll = true;
						metrics.dynamicPartitionFallbackCardId = id;
						metrics.dynamicPartitionFallbackEffectType = (int)effect.effectType;
						break;
					}
					// An unfiltered deck-size condition is identity-free.  A filtered
					// condition observes its predicate just as a selection does, even
					// when no card is moved.
					if (targetIncludesDeck(effect.target)
						&& (!effect.isCondition || !effect.target.conditions.empty()))
						applyTarget(effect.target);
					if (exposeAll) break;
				}
				if (exposeAll) break;
			}
		}
		if (exposeAll) partition.exposeAllIdentities();
		else if (drawContinuationOnly) {
			partition.refineVisible([&](int cardId) { return hasReachable(cardId); });
			if (v4PassiveDrawEnabled) {
				// Classify before evaluator signature split so Passive cards share a
				// transition-only key (V3 weight differences go to Residual).
				std::unordered_set<int> reachableSet;
				for (size_t i = 0; i < dependencyKey.reachableSize; ++i)
					reachableSet.insert(dependencyKey.reachable[i]);
				std::uint64_t preHash = 1469598103934665603ULL;
				preHash = ExactCardLivenessV4::MixHash(preHash, ExactCardLivenessV4::LivenessSchemaVersion);
				preHash = ExactCardLivenessV4::MixHash(preHash, ExactFeatureV4::SchemaVersion);
				preHash = ExactCardLivenessV4::MixHash(preHash, dependencyKey.drawContinuationOnly ? 1ull : 0ull);
				preHash = ExactCardLivenessV4::MixHash(preHash, dependencyKey.earlyTurn ? 1ull : 0ull);
				preHash = ExactCardLivenessV4::MixHash(preHash, dependencyKey.reachableSize);
				for (size_t i = 0; i < dependencyKey.reachableSize; ++i)
					preHash = ExactCardLivenessV4::MixHash(preHash, (std::uint64_t)dependencyKey.reachable[i]);
				for (size_t i = 0; i < dependencyKey.populationSize; ++i) {
					preHash = ExactCardLivenessV4::MixHash(preHash, (std::uint64_t)dependencyKey.populationId[i]);
					preHash = ExactCardLivenessV4::MixHash(preHash, (std::uint64_t)dependencyKey.populationCount[i]);
				}
				if (evaluator && evaluator->isLoaded())
					preHash = ExactCardLivenessV4::MixHash(preHash, evaluator->modelHash());
				dependencyKey.prePartitionProofHash = preHash;
				cacheKey.prePartitionProofHash = preHash;
				{
					auto lateHit = partitionAnalysisCache.find(cacheKey);
					if (lateHit != partitionAnalysisCache.end()) {
						metrics.dynamicPartitionCacheHits++;
						return lateHit->second;
					}
				}
				ExactEnsureDeferredFunctionRegistry();
			auto closure = ExactCardLivenessV4::BuildOperatorClosure(reachableSet, preHash);
				ExactCardLivenessV4::ApplyStateCoverageScanners(closure, state);
				(void)ExactCardLivenessV4::FurtherChanceUntilTurnEnd(closure, state);
				const ExactCardLivenessV4::PassiveMomentStateV4 partitionMoment =
					ExactCardLivenessV4::BuildPassiveMomentState(state, actor);
				// Per-card proveCandidate inside ClassifyCardId — no SealCoverage.
				partition.refineEquivalent([&](int cardId) {
					auto live = ExactCardLivenessV4::ClassifyCardId(state, actor, cardId, closure, partitionMoment);
					if (live.liveness == ExactCardLivenessV4::CardLiveness::Passive
						&& live.proof.allProven())
						return transitionContinuationKey(cardId);
					return continuationIdentityKey(cardId);
				});
			} else {
				partition.refineEquivalent([&](int cardId) {
					return continuationIdentityKey(cardId);
				});
			}
		}
		metrics.dynamicPartitionBuilds++;
		metrics.dynamicPartitionMaxClasses = std::max<unsigned long long>(
			metrics.dynamicPartitionMaxClasses, partition.classes().size());
		metrics.dynamicPartitionMaxVisibleIdentities = std::max<unsigned long long>(
			metrics.dynamicPartitionMaxVisibleIdentities, partition.visibleCardIds().size());
		PartitionAnalysis analysis;
		analysis.compressed = partition.hasCompressedClass();
		if (!analysis.compressed) metrics.dynamicPartitionFallbacks++;
		analysis.schema = partition.schemaKey();
		analysis.visibleIds = partition.visibleCardIds();
		for (size_t visibleIndex = 0; visibleIndex < analysis.visibleIds.size(); ++visibleIndex) {
			const int id = analysis.visibleIds[visibleIndex];
			if (id < 0 || (size_t)id >= actorCardIndexById.size()) continue;
			const int profileIndex = actorCardIndexById[(size_t)id];
			if (profileIndex >= 0) analysis.visiblePositionByProfile[(size_t)profileIndex] = (signed char)visibleIndex;
		}
		analysis.partition = std::move(partition);
		if (partitionAnalysisCache.size() < 16'384) {
			auto inserted = partitionAnalysisCache.emplace(std::move(cacheKey), std::move(analysis));
			return inserted.first->second;
		}
		uncachedPartitionAnalysis = std::move(analysis);
		return uncachedPartitionAnalysis;
	}

	// The dynamic-turn key is built at virtually every Rich continuation node.
	// Keep one owning buffer per active recursion depth: a parent key must remain
	// stable while its children run, but siblings at the same depth can reuse the
	// same allocation. This removes a heap allocation/copy on every TT probe
	// without changing a single key byte or the collision-safe equality check.
	const std::string* partitionTurnMainKey(const State& state) {
		HotTimer timer(metrics.partitionKeyCalls, metrics.partitionKeySampleNs);
		if (!singletonRevealStreaming || state.selectPlayer != actor
			|| state.selectType != SelectType::Main || state.exact.deckUnknown[actor]) return nullptr;
		const PartitionAnalysis& analysis = turnDependencyPartition(state);
		if (!analysis.compressed) return nullptr;
		// Count the concrete deck once.  The old implementation rescanned all
		// deck cards for every visible class, which was measurable on the Rich
		// Energy continuation (hundreds of thousands of calls).  The fixed array
		// is stack-only and the sorted visible IDs preserve the exact key order.
		const auto& visible = analysis.visibleIds;
		std::array<int, DECK_SIZE> visibleCounts{};
		for (CardRef ref : state.players[actor].deck) {
			if (ref.isNull()) continue;
			int id = state.getCard(ref).cardId;
			if (id < 0 || (size_t)id >= actorCardIndexById.size()) continue;
			const int profileIndex = actorCardIndexById[(size_t)id];
			if (profileIndex < 0) continue;
			const int visibleIndex = analysis.visiblePositionByProfile[(size_t)profileIndex];
			if (visibleIndex >= 0) visibleCounts[(size_t)visibleIndex]++;
		}
		if ((size_t)recursionDepth >= partitionKeyScratch.size())
			throw std::length_error("partition key recursion depth overflow");
		std::string& key = partitionKeyScratch[(size_t)recursionDepth];
		if (runtimeMode == ExactRuntimeMode::Cow) {
			ExactPackedKeyWriter writer;
			writer.bytes.swap(key);
			writer.bytes.clear();
			writer.bytes.reserve(64 + analysis.schema.size() + 512 + visible.size() * 8);
			writer.bytes.append("PTM3", 4);
			writer.u64(transposition->internExactComponent(analysis.schema));
			{
				HotTimer observationTimer(metrics.observationKeyCalls, metrics.observationKeySampleNs);
				appendPackedObservationKey(writer, state, actor, nullptr, false);
			}
			writer.u32((std::uint32_t)visible.size());
			for (size_t i = 0; i < visible.size(); ++i) {
				writer.i32(visible[i]); writer.i32(visibleCounts[i]);
			}
			key.swap(writer.bytes);
			return &key;
		}
		key.clear();
		key.reserve(64 + analysis.schema.size() + 512 + visible.size() * 16);
		key = "DYNAMIC-TURN-MAIN\x1f"; key += analysis.schema;
		key += observationKeyFor(state, actor, nullptr, false);
		for (size_t i = 0; i < visible.size(); ++i) {
			appendSemantic(key, visible[i]); appendSemantic(key, visibleCounts[i]);
		}
		return &key;
	}

	PartialDecisionEntry* partialDecisionFor(const std::string& key) {
		auto found = partialDecisions.find(key);
		if (found != partialDecisions.end()) { metrics.partialDecisionHits++; return &found->second; }
		if (partialDecisions.size() + partialChances.size() + partialReveals.size() >= MaxPartialEntries
			|| partialBytes + key.size() + 128 > MaxPartialBytes) return nullptr;
		size_t bytes = key.size() + 128;
		partialBytes += bytes;
		auto [inserted, _] = partialDecisions.emplace(key, PartialDecisionEntry{});
		inserted->second.accountedBytes = bytes;
		return &inserted->second;
	}

	PartialChanceEntry* partialChanceFor(const std::string& key) {
		auto found = partialChances.find(key);
		if (found != partialChances.end()) { metrics.partialChanceHits++; return &found->second; }
		if (partialDecisions.size() + partialChances.size() + partialReveals.size() >= MaxPartialEntries
			|| partialBytes + key.size() + 96 > MaxPartialBytes) return nullptr;
		size_t bytes = key.size() + 96;
		partialBytes += bytes;
		auto [inserted, _] = partialChances.emplace(key, PartialChanceEntry{});
		inserted->second.accountedBytes = bytes;
		return &inserted->second;
	}

	PartialRevealEntry* partialRevealFor(const std::string& key, int typeCount) {
		auto found = partialReveals.find(key);
		if (found != partialReveals.end()) { metrics.partialRevealHits++; return &found->second; }
		size_t bytes = key.size() + 256 + (size_t)typeCount * sizeof(int) * 10;
		if (partialDecisions.size() + partialChances.size() + partialReveals.size() >= MaxPartialEntries
			|| partialBytes + bytes > MaxPartialBytes) return nullptr;
		partialBytes += bytes;
		auto [inserted, _] = partialReveals.emplace(key, PartialRevealEntry{});
		inserted->second.accountedBytes = bytes;
		return &inserted->second;
	}

	static void appendSemantic(std::string& out, long long value) {
		out += std::to_string(value); out.push_back(';');
	}

	std::string cardToken(const State& state, CardRef ref, bool pokemon) const {
		if (ref.isNull()) return "?";
		const Card& card = state.getCard(ref);
		std::string token;
		appendSemantic(token, card.cardId);
		appendSemantic(token, card.playerIndex);
		if (!pokemon) return token;
		appendSemantic(token, state.getHp(card));
		appendSemantic(token, state.getMaxHp(card));
		appendSemantic(token, card.appear ? 1 : 0);
		auto& energyTypes = state.game->energyList;
		state.getEnergies(card.playerIndex, ref, energyTypes);
		std::vector<int> types;
		for (EnergyType type : energyTypes) types.push_back(EnergyTypeIndex(type));
		std::sort(types.begin(), types.end());
		for (int type : types) appendSemantic(token, type);
		token.push_back('|');
		auto& cards = state.game->cardList;
		state.getEnergyCards(ref, cards);
		std::vector<int> ids;
		for (CardRef child : cards) ids.push_back(state.getCard(child).cardId);
		auto tools = state.getAttachedToolRef(card);
		for (CardRef child : tools) ids.push_back(state.getCard(child).cardId + 100000);
		auto evolutions = state.getPreEvolutions(card);
		for (CardRef child : evolutions) ids.push_back(state.getCard(child).cardId + 200000);
		std::sort(ids.begin(), ids.end());
		for (int id : ids) appendSemantic(token, id);
		return token;
	}

	template<class List>
	void appendCardList(std::string& out, const State& state, const List& list,
		bool pokemon, bool unordered, bool hidden) const {
		appendSemantic(out, list.size());
		if (hidden) return;
		std::vector<std::string> tokens;
		tokens.reserve(list.size());
		for (CardRef ref : list) tokens.push_back(cardToken(state, ref, pokemon));
		if (unordered) std::sort(tokens.begin(), tokens.end());
		for (const std::string& token : tokens) {
			appendSemantic(out, (long long)token.size()); out += token;
		}
	}

	std::string optionSemanticToken(const State& state, const SelectOption& option) const {
		std::string token;
		appendSemantic(token, (int)option.type);
		auto appendPosition = [&](AreaType area, int index, int player) {
			appendSemantic(token, (int)area); appendSemantic(token, player);
			try {
				CardRef ref = state.getCardRef(area, index, player);
				std::string card = cardToken(state, ref, area == AreaType::Active || area == AreaType::Bench);
				appendSemantic(token, (long long)card.size()); token += card;
			} catch (...) { appendSemantic(token, index); }
		};
		switch (option.type) {
		case SelectOptionType::Card:
		case SelectOptionType::ToolCard:
		case SelectOptionType::EnergyCard:
		case SelectOptionType::Energy:
			appendPosition((AreaType)option.param0, option.param1, option.param2);
			appendSemantic(token, option.param3); appendSemantic(token, option.param4); break;
		case SelectOptionType::Play:
			appendPosition(AreaType::Hand, option.param0, state.selectPlayer); break;
		case SelectOptionType::Attach:
		case SelectOptionType::Evolve:
			appendPosition((AreaType)option.param0, option.param1, state.selectPlayer);
			appendPosition((AreaType)option.param2, option.param3, state.selectPlayer); break;
		case SelectOptionType::Ability:
		case SelectOptionType::Discard:
			appendPosition((AreaType)option.param0, option.param1, state.selectPlayer); break;
		case SelectOptionType::Skill:
			appendSemantic(token, option.param0); break; // serial is deliberately omitted
		default:
			appendSemantic(token, option.param0); appendSemantic(token, option.param1);
			appendSemantic(token, option.param2); appendSemantic(token, option.param3);
			appendSemantic(token, option.param4); break;
		}
		return token;
	}

	std::string legacyObservationKeyFor(const State& state, int requestedObserver = -1,
		const ExactKnowledgeState* knowledge = nullptr, bool includeKnownDeck = true) const {
		const int observer = requestedObserver >= 0 ? requestedObserver : state.selectPlayer;
		std::string key;
		appendSemantic(key, state.turn); appendSemantic(key, state.turnActionCount);
		appendSemantic(key, (int)state.phase); appendSemantic(key, (int)state.gameResult);
		appendSemantic(key, state.firstPlayer); appendSemantic(key, state.turnState);
		appendSemantic(key, (int)state.selectType); appendSemantic(key, (int)state.selectContext);
		appendSemantic(key, state.selectPlayer); appendSemantic(key, state.selectMin);
		appendSemantic(key, state.selectMax); appendSemantic(key, state.remainDamageCounter);
		appendSemantic(key, state.remainEnergyCost);
		appendCardList(key, state, state.stadium, false, true, false);
		for (int player = 0; player < 2; ++player) {
			const PlayerState& ps = state.players[player];
			appendCardList(key, state, ps.active, true, false, false);
			appendCardList(key, state, ps.bench, true, true, false);
			appendSemantic(key, state.benchCapacity(player));
			appendCardList(key, state, ps.trash, false, true, false);
			appendCardList(key, state, ps.prize, false, true, true);
			appendCardList(key, state, ps.hand, false, true, player != observer);
			appendSemantic(key, ps.deck.size());
			if (includeKnownDeck && ((state.selectDeck && state.selectPlayer == observer && player == observer)
				|| (knowledge != nullptr && knowledge->deckKnown[player])))
				appendCardList(key, state, ps.deck, false, true, false);
			appendSemantic(key, ps.poisonDamageCounter); appendSemantic(key, (int)ps.badStatus);
			appendSemantic(key, ps.burned ? 1 : 0);
		}
		std::vector<std::string> options;
		options.reserve(state.options.size());
		for (const SelectOption& option : state.options) options.push_back(optionSemanticToken(state, option));
		std::sort(options.begin(), options.end());
		for (const std::string& option : options) { appendSemantic(key, (long long)option.size()); key += option; }
		if (!state.contextCard.isNull()) { key += "C"; key += cardToken(state, state.contextCard, false); }
		if (state.onEffect()) { key += "E"; key += cardToken(state, state.getEffectCard().card, false); }
		if (knowledge != nullptr) { key += "K"; appendKnowledgeKey(key, *knowledge); }
		return key;
	}

	static void appendPackedKnowledge(ExactPackedKeyWriter& writer, const ExactKnowledgeState& knowledge) {
		for (bool known : knowledge.deckKnown) writer.u8(known ? 1 : 0);
		writer.u64(knowledge.observationSequence);
		writer.blob(knowledge.publicFacts);
		auto appendSequence = [&](const auto& lists) {
			for (const auto& list : lists) {
				writer.u32((std::uint32_t)list.size());
				for (int id : list) writer.i32(id);
			}
		};
		auto appendMapArray = [&](const auto& maps) {
			for (const auto& values : maps) {
				writer.u32((std::uint32_t)values.size());
				for (const auto& item : values) {
					writer.i32(item.first);
					if constexpr (std::is_same_v<std::decay_t<decltype(item.second)>, std::pair<int, int>>) {
						writer.i32(item.second.first); writer.i32(item.second.second);
					} else writer.i32(item.second);
				}
			}
		};
		appendSequence(knowledge.knownTop); appendSequence(knowledge.knownBottom);
		appendMapArray(knowledge.knownDeckCounts); appendMapArray(knowledge.knownPrizeCounts);
		appendMapArray(knowledge.countBounds);
	}

	void packedCardDescriptor(const State& state, CardRef ref, bool pokemon,
		ExactPackedDescriptor& descriptor) const {
		descriptor.clear();
		if (ref.isNull()) { descriptor.add(-1); return; }
		const size_t cacheIndex = (size_t)ref.cardIndex + (pokemon ? 128ULL : 0ULL);
		if (packedCardCacheGeneration[cacheIndex] == packedObservationGeneration) {
			descriptor.assign(packedCardCache[cacheIndex]); return;
		}
		ExactPackedDescriptor& cached = packedCardCache[cacheIndex];
		cached.clear();
		const Card& card = state.getCard(ref);
		cached.add(card.cardId); cached.add(card.playerIndex);
		if (!pokemon) {
			packedCardCacheGeneration[cacheIndex] = packedObservationGeneration;
			descriptor.assign(cached); return;
		}
		cached.add(state.getHp(card)); cached.add(state.getMaxHp(card));
		cached.add(card.appear ? 1 : 0);

		auto& energyTypes = state.game->energyList;
		state.getEnergies(card.playerIndex, ref, energyTypes);
		std::array<int, DECK_SIZE * 2> types{};
		if (energyTypes.size() > types.size()) throw std::length_error("packed energy type overflow");
		size_t typeCount = 0;
		for (EnergyType type : energyTypes) types[typeCount++] = EnergyTypeIndex(type);
		std::sort(types.begin(), types.begin() + typeCount);
		cached.add((int)typeCount);
		for (size_t i = 0; i < typeCount; ++i) cached.add(types[i]);

		std::array<int, DECK_SIZE * 3> attachments{};
		size_t attachmentCount = 0;
		auto appendAttachment = [&](int id) {
			if (attachmentCount >= attachments.size()) throw std::length_error("packed attachment overflow");
			attachments[attachmentCount++] = id;
		};
		auto& cards = state.game->cardList;
		state.getEnergyCards(ref, cards);
		for (CardRef child : cards) appendAttachment(state.getCard(child).cardId);
		for (CardRef child : state.getAttachedToolRef(card)) appendAttachment(state.getCard(child).cardId + 100000);
		for (CardRef child : state.getPreEvolutions(card)) appendAttachment(state.getCard(child).cardId + 200000);
		std::sort(attachments.begin(), attachments.begin() + attachmentCount);
		cached.add((int)attachmentCount);
		for (size_t i = 0; i < attachmentCount; ++i) cached.add(attachments[i]);
		packedCardCacheGeneration[cacheIndex] = packedObservationGeneration;
		descriptor.assign(cached);
	}

	template<class List>
	void appendPackedCardList(ExactPackedKeyWriter& writer, const State& state, const List& list,
		bool pokemon, bool unordered, bool hidden) const {
		writer.u32((std::uint32_t)list.size());
		if (hidden) return;
		if (list.size() > PackedScratchCapacity) throw std::length_error("packed card list overflow");
		if (!pokemon) {
			struct CompactCardToken {
				int marker, cardId, player;
				bool operator<(const CompactCardToken& other) const {
					if (marker != other.marker) return marker < other.marker;
					if (cardId != other.cardId) return cardId < other.cardId;
					return player < other.player;
				}
			};
			std::array<CompactCardToken, PackedScratchCapacity> tokens;
			size_t count = 0;
			for (CardRef ref : list) {
				CompactCardToken& token = tokens[count++];
				if (ref.isNull()) { token.marker = token.cardId = token.player = 0; continue; }
				const Card& card = state.getCard(ref);
				token.marker = 1; token.cardId = card.cardId; token.player = card.playerIndex;
			}
			if (unordered) std::sort(tokens.begin(), tokens.begin() + count);
			for (size_t i = 0; i < count; ++i) {
				writer.u8((unsigned char)tokens[i].marker);
				if (tokens[i].marker) { writer.i32(tokens[i].cardId); writer.i32(tokens[i].player); }
			}
			return;
		}
		size_t count = 0;
		for (CardRef ref : list) {
			packedCardDescriptor(state, ref, pokemon, packedDescriptors[count]);
			packedOrder[count] = (std::uint16_t)count; count++;
		}
		if (unordered) std::sort(packedOrder.begin(), packedOrder.begin() + count,
			[&](std::uint16_t lhs, std::uint16_t rhs) { return packedDescriptors[lhs] < packedDescriptors[rhs]; });
		for (size_t i = 0; i < count; ++i) packedDescriptors[packedOrder[i]].write(writer);
	}

	void packedOptionDescriptor(const State& state, const SelectOption& option,
		ExactPackedDescriptor& descriptor) const {
		descriptor.clear(); descriptor.add((int)option.type);
		auto appendPosition = [&](AreaType area, int index, int player) {
			descriptor.add((int)area); descriptor.add(player);
			try {
				ExactPackedDescriptor card;
				packedCardDescriptor(state, state.getCardRef(area, index, player),
					area == AreaType::Active || area == AreaType::Bench, card);
				descriptor.append(card);
			} catch (...) { descriptor.add(index); }
		};
		switch (option.type) {
		case SelectOptionType::Card:
		case SelectOptionType::ToolCard:
		case SelectOptionType::EnergyCard:
		case SelectOptionType::Energy:
			appendPosition((AreaType)option.param0, option.param1, option.param2);
			descriptor.add(option.param3); descriptor.add(option.param4); break;
		case SelectOptionType::Play:
			appendPosition(AreaType::Hand, option.param0, state.selectPlayer); break;
		case SelectOptionType::Attach:
		case SelectOptionType::Evolve:
			appendPosition((AreaType)option.param0, option.param1, state.selectPlayer);
			appendPosition((AreaType)option.param2, option.param3, state.selectPlayer); break;
		case SelectOptionType::Ability:
		case SelectOptionType::Discard:
			appendPosition((AreaType)option.param0, option.param1, state.selectPlayer); break;
		case SelectOptionType::Skill:
			descriptor.add(option.param0); break;
		default:
			descriptor.add(option.param0); descriptor.add(option.param1); descriptor.add(option.param2);
			descriptor.add(option.param3); descriptor.add(option.param4); break;
		}
	}

	void appendPackedObservationKey(ExactPackedKeyWriter& writer, const State& state, int requestedObserver,
		const ExactKnowledgeState* knowledge, bool includeKnownDeck) const {
		const int observer = requestedObserver >= 0 ? requestedObserver : state.selectPlayer;
		if (++packedObservationGeneration == 0) {
			packedCardCacheGeneration.fill(0); packedObservationGeneration = 1;
		}
		const size_t observationBegin = writer.bytes.size();
		writer.bytes.append("OBS3", 4);
		writer.i32(state.turn); writer.i32(state.turnActionCount);
		writer.i32((int)state.phase); writer.i32((int)state.gameResult);
		writer.i32(state.firstPlayer); writer.i32(state.turnState);
		writer.i32((int)state.selectType); writer.i32((int)state.selectContext);
		writer.i32(state.selectPlayer); writer.i32(state.selectMin); writer.i32(state.selectMax);
		writer.i32(state.remainDamageCounter); writer.i32(state.remainEnergyCost);
		appendPackedCardList(writer, state, state.stadium, false, true, false);
		for (int player = 0; player < 2; ++player) {
			const PlayerState& ps = state.players[player];
			appendPackedCardList(writer, state, ps.active, true, false, false);
			appendPackedCardList(writer, state, ps.bench, true, true, false);
			writer.i32(state.benchCapacity(player));
			appendPackedCardList(writer, state, ps.trash, false, true, false);
			appendPackedCardList(writer, state, ps.prize, false, true, true);
			appendPackedCardList(writer, state, ps.hand, false, true, player != observer);
			writer.u32((std::uint32_t)ps.deck.size());
			if (includeKnownDeck && ((state.selectDeck && state.selectPlayer == observer && player == observer)
				|| (knowledge != nullptr && knowledge->deckKnown[player])))
				appendPackedCardList(writer, state, ps.deck, false, true, false);
			writer.i32(ps.poisonDamageCounter); writer.i32((int)ps.badStatus); writer.u8(ps.burned ? 1 : 0);
		}
		if (state.options.size() > PackedScratchCapacity) throw std::length_error("packed option list overflow");
		writer.u32((std::uint32_t)state.options.size());
		for (size_t i = 0; i < state.options.size(); ++i) {
			packedOptionDescriptor(state, state.options[i], packedDescriptors[i]);
			packedOrder[i] = (std::uint16_t)i;
		}
		std::sort(packedOrder.begin(), packedOrder.begin() + state.options.size(),
			[&](std::uint16_t lhs, std::uint16_t rhs) { return packedDescriptors[lhs] < packedDescriptors[rhs]; });
		for (size_t i = 0; i < state.options.size(); ++i) packedDescriptors[packedOrder[i]].write(writer);
		writer.u8(state.contextCard.isNull() ? 0 : 1);
		if (!state.contextCard.isNull()) { ExactPackedDescriptor card; packedCardDescriptor(state, state.contextCard, false, card); card.write(writer); }
		writer.u8(state.onEffect() ? 1 : 0);
		if (state.onEffect()) { ExactPackedDescriptor card; packedCardDescriptor(state, state.getEffectCard().card, false, card); card.write(writer); }
		writer.u8(knowledge == nullptr ? 0 : 1);
		if (knowledge != nullptr) appendPackedKnowledge(writer, *knowledge);
		metrics.packedObservationBuilds++;
		metrics.packedObservationBytes += writer.bytes.size() - observationBegin;
	}

	std::string packedObservationKeyFor(const State& state, int requestedObserver,
		const ExactKnowledgeState* knowledge, bool includeKnownDeck) const {
		ExactPackedKeyWriter writer;
		appendPackedObservationKey(writer, state, requestedObserver, knowledge, includeKnownDeck);
		return std::move(writer.bytes);
	}

	void auditObservationKeyPair(const std::string& legacy, const std::string& packed) const {
		static constexpr size_t ShadowPairLimit = 100'000;
		auto audit = [&](auto& table, const std::string& first, const std::string& second) {
			auto found = table.find(first);
			if (found != table.end()) {
				if (found->second != second) {
					metrics.legacyShadowMismatches++; metrics.informationSetSafe = false;
				}
			} else if (table.size() < ShadowPairLimit) table.emplace(first, second);
		};
		audit(shadowLegacyToPacked, legacy, packed);
		audit(shadowPackedToLegacy, packed, legacy);
	}

	std::string observationKeyFor(const State& state, int requestedObserver = -1,
		const ExactKnowledgeState* knowledge = nullptr, bool includeKnownDeck = true) const {
		HotTimer timer(metrics.observationKeyCalls, metrics.observationKeySampleNs);
		if (runtimeMode == ExactRuntimeMode::Legacy)
			return legacyObservationKeyFor(state, requestedObserver, knowledge, includeKnownDeck);
		std::string packed = packedObservationKeyFor(state, requestedObserver, knowledge, includeKnownDeck);
		if (runtimeMode == ExactRuntimeMode::Cow) return packed;
		std::string legacy = legacyObservationKeyFor(state, requestedObserver, knowledge, includeKnownDeck);
		auditObservationKeyPair(legacy, packed);
		return legacy;
	}

	template<class Action>
	std::vector<std::string> semanticAction(const State& state, const Action& action) const {
		std::vector<std::string> tokens;
		for (size_t i = 0; i < action.size(); ++i)
			tokens.push_back(optionSemanticToken(state, state.options.at(action[i])));
		std::sort(tokens.begin(), tokens.end());
		return tokens;
	}

	bool remapAction(const State& state, const std::vector<std::string>& wanted, std::vector<int>& result) const {
		std::vector<bool> used(state.options.size(), false);
		for (const std::string& token : wanted) {
			int found = -1;
			for (int i = 0; i < (int)state.options.size(); ++i) if (!used[i]
				&& optionSemanticToken(state, state.options[i]) == token) { found = i; break; }
			if (found < 0) return false;
			used[found] = true; result.push_back(found);
		}
		std::sort(result.begin(), result.end());
		return (int)result.size() >= state.selectMin && (int)result.size() <= state.selectMax;
	}

	void rememberPolicy(const State& state, const ExactScore& score,
		bool actionValueCertified, bool bestActionCertified,
		bool exactValueCertified, unsigned long long subtreeExpanded) {
		if (score.action.empty() && state.selectMin != 0) return;
		if (policy.size() >= MaxPolicyEntries || policyBytes >= MaxPolicyBytes) return;
		// Policy entries are a re-rooting fast path.  Never persist a point value
		// or certification bit that was produced before the global safety gates
		// (including provisional opponent policy and experimental V4) ran.
		const bool certifiable = ExactMetricsCanCertify(metrics) && score.boundsSound;
		ExactScore storedScore = score;
		storedScore.certificationBlocked = storedScore.certificationBlocked || !certifiable;
		storedScore.certified = storedScore.certified && certifiable;
		actionValueCertified = actionValueCertified && certifiable;
		bestActionCertified = bestActionCertified && certifiable;
		exactValueCertified = exactValueCertified && certifiable;
		std::string key = observationKeyFor(state);
		ExactPolicyEntry entry{ storedScore, semanticAction(state, score.action),
			actionValueCertified, bestActionCertified, exactValueCertified,
			subtreeExpanded };
		size_t bytes = key.size() + sizeof(ExactPolicyEntry) + 64;
		for (const std::string& token : entry.actionTokens) bytes += token.size();
		if (policyBytes + bytes > MaxPolicyBytes) return;
		auto& bucket = policy[key];
		for (const ExactPolicyEntry& existing : bucket) {
			if (existing.actionTokens == entry.actionTokens
				&& ExactCompare(existing.score.lower, entry.score.lower) == 0
				&& ExactCompare(existing.score.upper, entry.score.upper) == 0
				&& existing.actionValueCertified == entry.actionValueCertified
				&& existing.bestActionCertified == entry.bestActionCertified
				&& existing.exactValueCertified == entry.exactValueCertified) return;
		}
		policyBytes += bytes; bucket.push_back(std::move(entry));
		metrics.policyNodes++; metrics.sessionBytes = transposition->bytes() + localTranspositionBytes
			+ policyBytes + partialBytes + beliefTranspositionBytes + evaluationCache.size() * 128ULL
			+ transitionScoreCache.size() * 128ULL;
	}

	void initializeHidden(State& state) {
		state.exact = {};
		state.exact.enabled = true;
		state.exact.actor = (signed char)actor;
		for (int player = 0; player < 2; ++player) {
			const auto& profile = player == actor ? actorProfileCount : opponentProfileCount;
			if (profile.empty()) continue;
			state.exact.profileKnown[player] = true;
			auto remaining = profile;
			for (const Card& card : state.allCard) {
				auto it = remaining.find(card.cardId);
				if (card.cardId != 0 && card.playerIndex == player && it != remaining.end() && it->second > 0) it->second--;
			}
			for (const auto& [id, count] : remaining) {
				if (count <= 0) continue;
				int index = state.exact.typeCount[player]++;
				state.exact.cardId[player][index] = id;
				state.exact.cardCount[player][index] = (unsigned char)count;
			}
		}
		auto hasNull = [](const auto& list) { for (CardRef ref : list) if (ref.isNull()) return true; return false; };
		for (int p = 0; p < 2; ++p) {
			state.exact.deckUnknown[p] = hasNull(state.players[p].deck);
			state.exact.prizeExchangeable[p] = hasNull(state.players[p].prize);
		}
	}

	long long evaluate(const State& state) {
		HotTimer timer(metrics.evaluatorCalls, metrics.evaluatorSampleNs);
		if (state.isFinish()) {
			int winner = state.winPlayer();
			return winner == actor ? 100'000'000 : (winner == 2 ? 0 : -100'000'000);
		}
		if (evaluator && evaluator->isLoaded()) {
			auto extractStarted = timer.sample ? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			ExactSparseEvaluatorV3::ExtractTiming extractTiming;
			static thread_local ExactSparseEvaluatorV3::FeatureRecord features;
			ExactSparseEvaluatorV3::extractFeaturesInto(features, state, actor, &actorProfileCount,
				nullptr, &metrics.evaluatorCacheHits, timer.sample ? &extractTiming : nullptr,
				&actorProfileSorted, false);
			if (timer.sample) {
				metrics.evaluatorPublicSampleNs += extractTiming.publicNs;
				metrics.evaluatorHiddenSampleNs += extractTiming.hiddenNs;
				metrics.evaluatorEntitySampleNs += extractTiming.entityNs;
			}
			if (timer.sample) metrics.evaluatorExtractSampleNs += (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - extractStarted).count();
			long long result = 0;
			auto inferenceStarted = timer.sample ? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			const bool useV4 = evaluator->usesV4Search() || evaluator->usesV4PassiveStrip();
			if (useV4 && evaluator->v4().isLoaded()) {
				ExactPassivePayloadV4 passive;
				{
					std::unordered_set<int> reachable;
					const PartitionAnalysis& liveAnalysis = turnDependencyPartition(state, nullptr, false);
					for (int id : liveAnalysis.visibleIds) reachable.insert(id);
					auto closure = ExactCardLivenessV4::BuildOperatorClosure(reachable,
						ExactCardLivenessV4::StableHashString(liveAnalysis.schema));
					ExactEnsureDeferredFunctionRegistry();
					ExactCardLivenessV4::ApplyStateCoverageScanners(closure, state);
					ExactCpuEvaluator::splitOwnHandFeatures(features, state, actor, passive, closure);
				}
				metrics.activeCardCount += 0; // observational; counts updated in draw path
				if (v4StripPassiveOnly) {
					metrics.v4SemanticEvaluations++;
					if (!evaluator->evaluateV4FeaturesUnclamped(features, ExactPassivePayloadV4{}, result,
						&metrics.evaluatorAccumulatorHits)) {
						if (evaluator->allowV3Fallback()) {
							ExactSparseEvaluatorV3::extractFeaturesInto(features, state, actor, &actorProfileCount,
								nullptr, nullptr, nullptr, &actorProfileSorted, false);
							if (!evaluator->evaluateV3Features(features, result, &metrics.evaluatorAccumulatorHits)) {
								metrics.informationSetSafe = false; return 0;
							}
						} else { metrics.informationSetSafe = false; return 0; }
					}
				} else if (evaluator->usesV4Search()) {
					metrics.v4SemanticEvaluations++;
					if (!passive.empty()) metrics.v4PassiveEvaluations++;
					if (!evaluator->evaluateV4FeaturesUnclamped(features, passive, result,
						&metrics.evaluatorAccumulatorHits)) {
						if (evaluator->allowV3Fallback()) {
							ExactSparseEvaluatorV3::extractFeaturesInto(features, state, actor, &actorProfileCount,
								nullptr, nullptr, nullptr, &actorProfileSorted, false);
							if (!evaluator->evaluateV3Features(features, result, &metrics.evaluatorAccumulatorHits)) {
								metrics.informationSetSafe = false; return 0;
							}
						} else { metrics.informationSetSafe = false; return 0; }
					}
				} else {
					// Dual: search still uses V3 absolute values.
					ExactSparseEvaluatorV3::extractFeaturesInto(features, state, actor, &actorProfileCount,
						nullptr, nullptr, nullptr, &actorProfileSorted, false);
					if (!evaluator->evaluateV3Features(features, result, &metrics.evaluatorAccumulatorHits)) {
						metrics.informationSetSafe = false; return 0;
					}
				}
			} else if (!evaluator->evaluateV3Features(features, result, &metrics.evaluatorAccumulatorHits)) {
				metrics.informationSetSafe = false; return 0;
			}
			if (runtimeMode == ExactRuntimeMode::Shadow) {
				static thread_local ExactSparseEvaluatorV3::FeatureRecord canonicalFeatures;
				ExactSparseEvaluatorV3::extractFeaturesInto(canonicalFeatures, state, actor,
					&actorProfileCount, nullptr, nullptr, nullptr, &actorProfileSorted, true);
				long long canonicalResult = 0;
				if (!evaluator->evaluateV3Features(canonicalFeatures, canonicalResult)
					|| canonicalResult != result) {
					metrics.legacyShadowMismatches++; metrics.informationSetSafe = false;
				}
			}
			if (timer.sample) metrics.evaluatorInferenceSampleNs += (unsigned long long)
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - inferenceStarted).count();
			return result;
		}
		return 0;
	}

	static int beliefQ8(const ExactWeight& numerator, const ExactWeight& denominator) {
		ExactWeight scaled = ExactWeight::multiply(numerator, ExactWeight(ExactSparseEvaluatorV3::BeliefScale));
		auto division = ExactWeight::divideRemainder(scaled, denominator);
		ExactWeight twiceRemainder = ExactWeight::multiply(division.second, ExactWeight(2));
		if (twiceRemainder >= denominator) division.first += ExactWeight(1);
		if (!division.first.fitsUnsignedLongLong()) throw std::overflow_error("belief Q8 overflow");
		return (int)division.first.unsignedLongLong();
	}

	long long evaluateBeliefInformationState(const std::vector<BeliefWorld>& worlds,
		const ExactWeight& total) {
		if (worlds.empty() || total.zero()) throw std::runtime_error("empty leaf belief");
		const State& representative = *worlds.front().state;
		if (representative.isFinish()) return evaluate(representative);
		std::unordered_map<int, ExactWeight> deckMass, prizeMass, deckExistsMass, prizeExistsMass, comboMass;
		for (const BeliefWorld& world : worlds) {
			std::unordered_map<int, int> deckCount, prizeCount;
			std::unordered_set<int> worldCombos;
			for (CardRef ref : world.state->players[actor].deck) if (!ref.isNull())
				deckCount[world.state->getCard(ref).cardId]++;
			for (CardRef ref : world.state->players[actor].prize) if (!ref.isNull())
				prizeCount[world.state->getCard(ref).cardId]++;
			for (const auto& item : deckCount) deckMass[item.first] +=
				ExactWeight::multiply(world.weight, ExactWeight(item.second));
			for (const auto& item : prizeCount) prizeMass[item.first] +=
				ExactWeight::multiply(world.weight, ExactWeight(item.second));
			for (const auto& item : deckCount) if (item.second > 0) deckExistsMass[item.first] += world.weight;
			for (const auto& item : prizeCount) if (item.second > 0) prizeExistsMass[item.first] += world.weight;
			// Evolution correlations are exact events over this concrete world.
			for (const auto& profile : actorProfileCount) {
				const CardMaster* foundMaster = FindCardMaster(profile.first);
				if (foundMaster == nullptr || foundMaster->evolutionType == EvolutionType::Basic
					|| foundMaster->evolutionType == EvolutionType::NoEvolutionType
					|| deckCount[profile.first] <= 0) continue;
				bool hasPre = false;
				for (const auto& candidate : actorProfileCount) {
					const CardMaster* preMaster = FindCardMaster(candidate.first);
					if (preMaster != nullptr && deckCount[candidate.first] > 0
						&& (preMaster->name == foundMaster->evolvesFrom
							|| preMaster->nameEn == foundMaster->evolvesFrom)) { hasPre = true; break; }
				}
				if (hasPre) worldCombos.insert(ExactSparseEvaluatorV3::ComboTokenBase + profile.first);
				if (hasPre && foundMaster->evolutionType == EvolutionType::Stage2) {
					bool hasBasic = false;
					for (const auto& stageCandidate : actorProfileCount) { const CardMaster* stage = FindCardMaster(stageCandidate.first);
						if (stage == nullptr || deckCount[stageCandidate.first] <= 0
							|| !(stage->name == foundMaster->evolvesFrom || stage->nameEn == foundMaster->evolvesFrom)) continue;
						for (const auto& basicCandidate : actorProfileCount) { const CardMaster* basic = FindCardMaster(basicCandidate.first);
							if (basic != nullptr && deckCount[basicCandidate.first] > 0
								&& (basic->name == stage->evolvesFrom || basic->nameEn == stage->evolvesFrom)) {
								hasBasic = true; break;
							}
						}
						if (hasBasic) break;
					}
					if (hasBasic) worldCombos.insert(ExactSparseEvaluatorV3::ComboTokenBase + 250'000 + profile.first);
				}
			}
			// Energy supply event: attaching every compatible energy remaining in
			// the deck must make the attack payable. Adding energy cannot invalidate
			// an attack, so this is an exact existence test, not a sample.
			auto attackSupply = [&](CardRef pokemonRef) {
				if (pokemonRef.isNull()) return;
				const State& s = *world.state; const Card& pokemon = s.getCard(pokemonRef);
				auto& existing = s.game->energyList; s.getEnergies(actor, pokemonRef, existing);
				SetAttackEnergy(s, pokemon, existing, true);
				std::unordered_map<int, int> before;
				for (const AttackEnergy& ae : s.game->attackEnergyList) before[ae.attack->attackId] = ae.insufficientEnergy;
				auto all = existing;
				for (CardRef energyRef : s.players[actor].deck) if (!energyRef.isNull()) {
					const Card& energy = s.getCard(energyRef);
					if (!IsEnergy(energy.getMaster().cardType)) continue;
					EnergyInfo info = s.getEnergyInfo(energy, pokemonRef);
					for (int n = 0; n < info.count; ++n) all.push_back(info.type);
				}
				SetAttackEnergy(s, pokemon, all, true);
				for (const AttackEnergy& ae : s.game->attackEnergyList) if (before[ae.attack->attackId] > 0 && ae.insufficientEnergy <= 0)
					worldCombos.insert(ExactSparseEvaluatorV3::ComboTokenBase + 500'000 + ae.attack->attackId);
			};
			for (CardRef ref : world.state->players[actor].active) attackSupply(ref);
			for (CardRef ref : world.state->players[actor].bench) attackSupply(ref);
			for (int token : worldCombos) comboMass[token] += world.weight;
		}
		std::unordered_map<int, int> deckQ8, prizeQ8, deckExistsQ8, prizeExistsQ8, comboQ8;
		for (const auto& item : deckMass) deckQ8[item.first] = beliefQ8(item.second, total);
		for (const auto& item : prizeMass) prizeQ8[item.first] = beliefQ8(item.second, total);
		for (const auto& item : deckExistsMass) deckExistsQ8[item.first] = beliefQ8(item.second, total);
		for (const auto& item : prizeExistsMass) prizeExistsQ8[item.first] = beliefQ8(item.second, total);
		for (const auto& item : comboMass) comboQ8[item.first] = beliefQ8(item.second, total);
		ExactSparseEvaluatorV3::BeliefInput belief{ &deckQ8, &prizeQ8, &deckExistsQ8, &prizeExistsQ8, &comboQ8,
			&worlds.front().knowledge[actor].knownDeckCounts[actor], &worlds.front().knowledge[actor].knownPrizeCounts[actor],
			&worlds.front().knowledge[actor].knownTop, &worlds.front().knowledge[actor].knownBottom };
		if (evaluator && evaluator->isLoaded()) {
			auto features = ExactSparseEvaluatorV3::extractFeatures(representative, actor, &actorProfileCount, &belief);
			metrics.entityFeatureCount += features.entityCount; metrics.comboFeatureCount += comboQ8.size();
			for (int ei = 0; ei < features.entityCount; ++ei) for (int si = 0; si < features.entities[ei].sparse.count; ++si)
				if (features.entities[ei].sparse.values[si].relation == ExactSparseEvaluatorV3::AttackUnavailable)
					metrics.attackPreviewUnavailableCount++;
				else if (features.entities[ei].sparse.values[si].relation == ExactSparseEvaluatorV3::AttackExactDamage)
					metrics.attackPreviewExactCount++;
			long long value = 0;
			if (!evaluator->evaluateV3Features(features, value)) { metrics.informationSetSafe = false; return 0; }
			return value;
		}
		return evaluate(representative);
	}

	void applyEvaluatorSafety(ExactDecision& decision) {
		const bool supportedSchema = evaluator && evaluator->isLoaded() && (
			evaluator->usesV4Search()
				? evaluator->schemaVersion() == ExactSparseEvaluatorV4::ModelSchemaVersion
				: evaluator->schemaVersion() == ExactSparseEvaluatorV3::SchemaVersion);
		if (!evaluator || !evaluator->isLoaded() || !supportedSchema || !evaluator->informationSetSafe()) {
			metrics.informationSetSafe = false;
			decision.score.certified = false;
			decision.actionValueCertified = false;
			decision.bestActionCertified = false;
			decision.exactValueCertified = false;
			for (ExactRootActionValue& action : decision.rootActions) action.certified = false;
		}
		// Keep V4 / Passive-draw experimental until the identity-oracle certification flag.
		if (!v4PassiveDrawCertified
			&& ((evaluator && evaluator->usesV4Search()) || v4PassiveDrawEnabled)) {
			metrics.v4PassiveDrawExperimental = true;
			decision.score.certified = false;
			decision.actionValueCertified = false;
			decision.bestActionCertified = false;
			decision.exactValueCertified = false;
			for (ExactRootActionValue& action : decision.rootActions) action.certified = false;
		}
		if (!ExactMetricsCanCertify(metrics)) {
			decision.score.certified = false;
			decision.score.certificationBlocked = true;
			decision.actionValueCertified = false;
			decision.bestActionCertified = false;
			decision.exactValueCertified = false;
			for (ExactRootActionValue& action : decision.rootActions) action.certified = false;
		}
	}

	ExactScore unknown() const { return {}; }

	ExactScore blockedUnknown() const {
		ExactScore result = unknown();
		result.certificationBlocked = true;
		return result;
	}

	ExactScore boundaryScore(const State& state, long long value) {
		if (state.exact.provisionalOpponentPolicy) {
			// A deterministic opponent choice is an estimate, not a minimax
			// boundary.  Keep all downstream intervals sound by discarding the
			// point estimate before it can enter pruning or a transposition table.
			metrics.provisionalOpponentPolicyNodes++;
			return blockedUnknown();
		}
		return { ExactFraction::integer(value), ExactFraction::integer(value), {}, true, true };
	}

	struct SemanticOptionId {
		std::array<int, 7> values{};
		unsigned char count = 0;
		void add(int value) { values[count++] = value; }
		bool operator==(const SemanticOptionId& other) const {
			return count == other.count && std::equal(values.begin(), values.begin() + count, other.values.begin());
		}
		bool operator<(const SemanticOptionId& other) const {
			return std::lexicographical_compare(values.begin(), values.begin() + count,
				other.values.begin(), other.values.begin() + other.count);
		}
		void write(ExactPackedKeyWriter& writer) const {
			writer.u8(count);
			for (unsigned char i = 0; i < count; ++i) writer.i32(values[i]);
		}
	};

	SemanticOptionId semanticOptionId(const State& state, int selectedIndex) const {
		const SelectOption& option = state.options[selectedIndex];
		SemanticOptionId id;
		if (option.type == SelectOptionType::Play) {
			CardRef ref = state.getCardRef(AreaType::Hand, option.param0, state.selectPlayer);
			if (!ref.isNull()) { id.add(0); id.add(state.getCard(ref).cardId); return id; }
		} else if (option.type == SelectOptionType::Attach || option.type == SelectOptionType::Evolve) {
			CardRef ref = state.getCardRef((AreaType)option.param0, option.param1, state.selectPlayer);
			if (!ref.isNull()) {
				id.add(option.type == SelectOptionType::Attach ? 1 : 2);
				id.add(state.getCard(ref).cardId); id.add(option.param2); id.add(option.param3); return id;
			}
		} else if (option.type == SelectOptionType::Discard
			|| option.type == SelectOptionType::Ability) {
			CardRef ref = state.getCardRef((AreaType)option.param0, option.param1, state.selectPlayer);
			if (!ref.isNull()) {
				id.add(option.type == SelectOptionType::Discard ? 6 : 7);
				id.add(state.getCard(ref).cardId);
				id.add((int)option.param0);
				return id;
			}
		} else if (option.type == SelectOptionType::ToolCard
			|| option.type == SelectOptionType::EnergyCard
			|| option.type == SelectOptionType::Energy) {
			CardRef ref = state.getCardRef((AreaType)option.param0, option.param1, option.param2);
			if (!ref.isNull()) {
				id.add(8); id.add((int)option.type); id.add(state.getCard(ref).cardId);
				id.add(option.param3); id.add(option.param4); return id;
			}
		}
		if (option.type == SelectOptionType::Card) {
			CardPosition pos = option.getCardPosition();
			bool exchangeable = (pos.area == AreaType::Deck && state.exact.deckExchangeable[pos.playerIndex])
				|| (pos.area == AreaType::Prize && state.exact.prizeExchangeable[pos.playerIndex]);
			if (exchangeable) {
				CardRef ref = state.getCardRef(pos);
				id.add(pos.area == AreaType::Deck ? 3 : 4); id.add(pos.playerIndex);
				id.add(ref.isNull() ? 0 : state.getCard(ref).cardId); return id;
			}
			// Hand / trash card picks: identity by cardId, not physical index.
			if (pos.area == AreaType::Hand || pos.area == AreaType::Trash) {
				CardRef ref = state.getCardRef(pos);
				if (!ref.isNull()) {
					id.add(9); id.add((int)pos.area); id.add(pos.playerIndex);
					id.add(state.getCard(ref).cardId); return id;
				}
			}
		}
		id.add(5); id.add((int)option.type); id.add(option.param0); id.add(option.param1);
		id.add(option.param2); id.add(option.param3); id.add(option.param4);
		return id;
	}

	template<class Callback>
	bool forEachLegalAction(const State& state, Callback&& callback) {
		// Evaluate End first so an interrupted main node always has a real leaf
		// lower bound. An assisting reverse worker deliberately reaches it last.
		int endIndex = -1;
		if (state.selectType == SelectType::Main && state.selectMin <= 1 && state.selectMax >= 1) {
			for (int i = 0; i < (int)state.options.size(); ++i) {
				if (state.options[i].type == SelectOptionType::End) { endIndex = i; break; }
			}
			if (!reverseActionOrder && endIndex >= 0
				&& !callback(ExactSmallAction{ endIndex })) return false;
		}
		struct OptionGroup {
			enum : size_t { InlineCount = 8 };
			SemanticOptionId key;
			std::array<int, InlineCount> inlineIndex{};
			std::vector<int> overflow;
			size_t count = 0;
			OptionGroup() = default;
			explicit OptionGroup(const SemanticOptionId& value) : key(value) {}
			void reset(const SemanticOptionId& value) { key = value; count = 0; overflow.clear(); }
			void push(int value) {
				if (count < InlineCount) inlineIndex[count] = value;
				else overflow.push_back(value);
				count++;
			}
			int at(size_t position) const {
				return position < InlineCount ? inlineIndex[position] : overflow[position - InlineCount];
			}
		};
		static constexpr size_t InlineGroups = 8;
		std::array<OptionGroup, InlineGroups> inlineGroups;
		std::vector<OptionGroup> overflowGroups;
		size_t groupCount = 0;
		auto groupAt = [&](size_t position) -> OptionGroup& {
			return position < InlineGroups ? inlineGroups[position] : overflowGroups[position - InlineGroups];
		};
		for (int i = 0; i < (int)state.options.size(); ++i) {
			HotTimer actionTimer(metrics.actionKeyCalls, metrics.actionKeySampleNs);
			SemanticOptionId key = semanticOptionId(state, i);
			size_t found = 0;
			while (found < groupCount && !(groupAt(found).key == key)) ++found;
			if (found == groupCount) {
				if (groupCount < InlineGroups) inlineGroups[groupCount].reset(key);
				else overflowGroups.emplace_back(key);
				groupCount++;
			}
			groupAt(found).push(i);
		}
		auto orderedGroup = [&](size_t position) -> OptionGroup& {
			return groupAt(reverseActionOrder ? groupCount - 1 - position : position);
		};
		ExactSmallAction current;
		ExactSmallAction actionScratch;
		auto choose = [&](auto&& self, int group, int left) -> bool {
			if (expired()) return false;
			if (left == 0) {
				actionScratch = current;
				actionScratch.sort();
				if (!reverseActionOrder && actionScratch.size() == 1 && actionScratch[0] == endIndex) return true;
				return callback(actionScratch);
			}
			if (group >= (int)groupCount) return true;
			int remainingCapacity = 0;
			for (int i = group; i < (int)groupCount; ++i) remainingCapacity += (int)orderedGroup((size_t)i).count;
			if (remainingCapacity < left) return true;
			OptionGroup& selectedGroup = orderedGroup((size_t)group);
			int maximum = std::min(left, (int)selectedGroup.count);
			int take = reverseActionOrder ? maximum : 0;
			for (; reverseActionOrder ? take >= 0 : take <= maximum;
				take += reverseActionOrder ? -1 : 1) {
				for (int i = 0; i < take; ++i) current.push_back(selectedGroup.at((size_t)i));
				if (!self(self, group + 1, left - take)) return false;
				for (int i = 0; i < take; ++i) current.pop_back();
			}
			return true;
		};
		int count = reverseActionOrder ? state.selectMax : state.selectMin;
		for (; reverseActionOrder ? count >= state.selectMin : count <= state.selectMax;
			count += reverseActionOrder ? -1 : 1) {
			if (!choose(choose, 0, count)) return false;
		}
		return true;
	}

	bool advance(State& state, std::initializer_list<int> action) {
		return advance(state, ExactSmallAction(action));
	}

	template<class Action>
	bool advance(State& state, const Action& action) {
		HotTimer timer(metrics.actionApplyCalls, metrics.actionApplySampleNs);
		try {
			state.selected.resize(action.size());
			for (size_t i = 0; i < action.size(); ++i) state.selected[i] = action[i];
			if (state.checkPlayerSelect() != 0) return false;
			stepExact(state);
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
				state.selected.clear(); stepExact(state);
			}
			return true;
		} catch (const std::exception& error) {
			metrics.exceptions++;
			metrics.lastException = error.what();
			state.exact.pending = ExactPendingType::Opaque;
			state.exact.blockReason = ExactBlockReason::Exception;
			return true;
		} catch (...) {
			metrics.exceptions++;
			metrics.lastException = "unknown";
			state.exact.pending = ExactPendingType::Opaque;
			state.exact.blockReason = ExactBlockReason::Exception;
			return true;
		}
	}

	CardRef materialize(State& state, int player, AreaType area, int areaIndex, int cardId) {
		int freeIndex = -1;
		for (int i = 3; i < (int)state.allCard.size(); ++i) if (state.allCard[i].cardId == 0) { freeIndex = i; break; }
		if (freeIndex < 0) throw std::runtime_error("no exact card slot");
		CardRef ref(freeIndex);
		Card& card = state.allCard[freeIndex]; card = {}; card.init(cardId, state.moveCounter++, player); card.area = area;
		PlayerState& ps = state.players[player];
		switch (area) {
		case AreaType::Deck: ps.deck.at(areaIndex) = ref; break;
		case AreaType::Hand: ps.hand.at(areaIndex) = ref; break;
		case AreaType::Prize: ps.prize.at(areaIndex) = ref; break;
		default: throw std::runtime_error("unsupported exact materialization area");
		}
		return ref;
	}

	template<class Action>
	std::string actionEquivalenceKey(const State& state, const Action& action) const {
		HotTimer timer(metrics.actionKeyCalls, metrics.actionKeySampleNs);
		ExactPackedKeyWriter writer;
		writer.u32((std::uint32_t)action.size());
		if (action.size() == 1) {
			semanticOptionId(state, action.front()).write(writer);
			return std::move(writer.bytes);
		}
		std::vector<SemanticOptionId> tokens;
		tokens.reserve(action.size());
		for (size_t i = 0; i < action.size(); ++i) tokens.push_back(semanticOptionId(state, action[i]));
		std::sort(tokens.begin(), tokens.end());
		for (const SemanticOptionId& token : tokens) token.write(writer);
		return std::move(writer.bytes);
	}

	void decrementPool(State& state, int player, int cardId) {
		for (int i = 0; i < state.exact.typeCount[player]; ++i) if (state.exact.cardId[player][i] == cardId) {
			if (state.exact.cardCount[player][i] == 0) throw std::runtime_error("empty hidden card type");
			state.exact.cardCount[player][i]--; return;
		}
		throw std::runtime_error("unknown hidden card type");
	}

	static ExactWeight chooseCount(int n, int k) {
		if (n < 0 || n > DECK_SIZE || k < 0 || k > n) return ExactWeight();
		static const auto table = [] {
			std::array<std::array<unsigned long long, DECK_SIZE + 1>, DECK_SIZE + 1> value{};
			for (int row = 0; row <= DECK_SIZE; ++row) {
				value[row][0] = value[row][row] = 1;
				for (int column = 1; column < row; ++column)
					value[row][column] = value[row - 1][column - 1] + value[row - 1][column];
			}
			return value;
		}();
		return ExactWeight(table[n][k]);
	}

	void noteWeight(const ExactWeight& value, bool operation = true) {
		if (operation) {
			if (value.isLarge()) { metrics.bigWeightPromotions++; metrics.exactWeightSpills++; }
			else { metrics.smallWeightOps++; metrics.exactWeightInlineOps++; }
		}
		metrics.maxWeightBits = std::max(metrics.maxWeightBits, value.bitLength());
	}

	void materializeUnknownZones(State& state, int player, const std::vector<int>& prizeCounts,
		const std::vector<int>& handCounts) {
		PlayerState& ps = state.players[player];
		std::vector<int> prizeIds, handIds, deckIds;
		for (int i = 0; i < state.exact.typeCount[player]; ++i) {
			int p = prizeCounts[i], h = handCounts[i];
			for (int n = 0; n < p; ++n) prizeIds.push_back(state.exact.cardId[player][i]);
			for (int n = 0; n < h; ++n) handIds.push_back(state.exact.cardId[player][i]);
			for (int n = p + h; n < state.exact.cardCount[player][i]; ++n) deckIds.push_back(state.exact.cardId[player][i]);
		}
		int prizeAt = 0, handAt = 0, deckAt = 0;
		for (int i = 0; i < ps.prize.size(); ++i) if (ps.prize[i].isNull()) {
			if (prizeAt >= (int)prizeIds.size()) throw std::runtime_error("prize materialization mismatch");
			materialize(state, player, AreaType::Prize, i, prizeIds[prizeAt++]);
		}
		for (int i = 0; i < ps.hand.size(); ++i) if (ps.hand[i].isNull()) {
			if (handAt >= (int)handIds.size()) throw std::runtime_error("hand materialization mismatch");
			materialize(state, player, AreaType::Hand, i, handIds[handAt++]);
		}
		for (int i = 0; i < ps.deck.size(); ++i) if (ps.deck[i].isNull()) {
			if (deckAt >= (int)deckIds.size()) throw std::runtime_error("deck materialization mismatch");
			materialize(state, player, AreaType::Deck, i, deckIds[deckAt++]);
		}
		if (prizeAt != (int)prizeIds.size() || handAt != (int)handIds.size() || deckAt != (int)deckIds.size()) throw std::runtime_error("hidden zone size mismatch");
		state.exact.deckUnknown[player] = false;
		state.exact.deckExchangeable[player] = true;
		state.exact.prizeExchangeable[player] = true;
		state.exact.clearPending();
	}

	static AreaType pendingArea(const ExactHiddenState& request) {
		int detail = request.pendingDetail;
		if (request.pendingPlayer >= 0) detail -= 100 * request.pendingPlayer;
		return (AreaType)detail;
	}

	void materializeUnknownHand(State& state, int player, const std::vector<int>& handCounts) {
		std::vector<int> ids;
		for (int i = 0; i < state.exact.typeCount[player]; ++i)
			for (int n = 0; n < handCounts[i]; ++n) ids.push_back(state.exact.cardId[player][i]);
		std::sort(ids.begin(), ids.end());
		int at = 0;
		for (int i = 0; i < state.players[player].hand.size(); ++i) {
			if (!state.players[player].hand[i].isNull()) continue;
			if (at >= (int)ids.size()) throw std::runtime_error("hand-only materialization mismatch");
			materialize(state, player, AreaType::Hand, i, ids[at++]);
		}
		if (at != (int)ids.size()) throw std::runtime_error("hand-only materialization overflow");
		state.exact.clearPending();
	}

	void materializeUnknownHandFromPool(State& state, int player,
		const std::vector<int>& handCounts) {
		materializeUnknownHand(state, player, handCounts);
		for (int i = 0; i < (int)handCounts.size(); ++i)
			for (int n = 0; n < handCounts[i]; ++n)
				decrementPool(state, player, state.exact.cardId[player][i]);
	}

	bool expandHiddenHandBelief(const State& parent, const ExactHiddenState& request,
		const std::vector<int>& action, const ExactWeight& baseWeight,
		const std::array<ExactKnowledgeState, 2>& baseKnowledge,
		std::vector<BeliefWorld>& output) {
		const int player = request.pendingPlayer;
		if (player < 0 || player >= 2 || request.pending != ExactPendingType::RevealDeck
			|| pendingArea(request) != AreaType::Hand || !parent.exact.profileKnown[player]) return false;
		int handSize = 0;
		for (CardRef ref : parent.players[player].hand) if (ref.isNull()) handSize++;
		std::vector<int> bounds(parent.exact.typeCount[player]);
		int totalHidden = 0;
		for (int i = 0; i < (int)bounds.size(); ++i) {
			bounds[i] = parent.exact.cardCount[player][i]; totalHidden += bounds[i];
		}
		if (handSize < 0 || handSize > totalHidden) return false;
		ExactWeight expected = ExactWeight::multiply(baseWeight, chooseCount(totalHidden, handSize));
		ExactWeight generated;
		BoundedCompositionCursor cursor; cursor.reset(bounds, handSize);
		std::vector<int> handCounts;
		unsigned long long raw = 0;
		while (cursor.next(handCounts)) {
			if (expired()) return false;
			ExactWeight allocation(1);
			for (int i = 0; i < (int)bounds.size(); ++i)
				allocation = ExactWeight::multiply(allocation, chooseCount(bounds[i], handCounts[i]));
			if (allocation.zero()) continue;
			ExactWeight weight = ExactWeight::multiply(baseWeight, allocation);
			auto child = cloneState(parent);
			auto knowledge = baseKnowledge;
			try {
				materializeUnknownHandFromPool(*child, player, handCounts);
				for (int i = 0; i < (int)handCounts.size(); ++i)
					for (int n = 0; n < handCounts[i]; ++n)
						appendKnowledgeFact(knowledge[player], 'H', parent.exact.cardId[player][i]);
				if (!advance(*child, action)) return false;
			} catch (...) { return false; }
			output.push_back({ std::move(child), weight, std::move(knowledge) });
			generated += weight; raw++; metrics.enumeratedHiddenWorlds++;
		}
		if (generated != expected) {
			metrics.chanceMassMismatches++; metrics.probabilityExact = false; return false;
		}
		metrics.rawOutcomes += raw; metrics.groupedOutcomes += output.size();
		return !output.empty();
	}

	bool isForcedHiddenHandDiscard(const ExactHiddenState& request, int& keepCount) const {
		if (pendingArea(request) != AreaType::Hand || request.pendingSkillId <= 0
			|| request.pendingEffectIndex < 0) return false;
		auto skill = SkillTable.find(request.pendingSkillId);
		if (skill == SkillTable.end() || request.pendingEffectIndex >= (int)skill->second.effects.size()) return false;
		const Effect& effect = skill->second.effects[request.pendingEffectIndex];
		if (effect.effectType != EffectType::ToTrash || effect.effectSelectType != EffectSelectType::CardUntil
			|| !effect.enemySelect) return false;
		keepCount = effect.selectCount;
		return keepCount >= 0 && canForgetOpponentHandAfterForcedDiscard(request);
	}

	static bool targetReadsEnemyHand(const Target& target) {
		bool hand = false;
		for (AreaType area : target.areas) if (area == AreaType::Hand) { hand = true; break; }
		if (!hand) return false;
		return target.targetPlayer == TargetPlayer::Enemy || target.targetPlayer == TargetPlayer::Both;
	}

	bool canForgetOpponentHandAfterForcedDiscard(const ExactHiddenState& request) const {
		const CardMaster* effectCard = FindCardMaster(request.pendingEffectCardId);
		// The proof below relies on the once-per-turn Supporter rule: after this
		// effect resolves, every other Supporter is unreachable until the leaf.
		if (effectCard == nullptr || effectCard->cardType != CardType::Supporter)
			return false;
		for (const auto& profile : actorProfileCount) {
			const CardMaster* card = FindCardMaster(profile.first);
			if (card == nullptr) return false;
			if (card->cardType == CardType::Supporter) continue;
			auto reads = [](const Effect& effect) { return targetReadsEnemyHand(effect.target); };
			for (const Skill* skill : card->getSkills()) {
				if (skill == nullptr) continue;
				for (const Effect& effect : skill->effects) if (reads(effect)) return false;
			}
			for (const Attack* attack : card->attacks) {
				if (attack == nullptr) continue;
				for (const Effect& effect : attack->preEffects) if (reads(effect)) return false;
				for (const Effect& effect : attack->postEffects) if (reads(effect)) return false;
			}
		}
		return true;
	}

	bool selectHiddenHandDiscard(State& state, int player, const std::vector<int>& discardCounts) {
		std::unordered_map<int, int> remaining;
		for (int i = 0; i < (int)discardCounts.size(); ++i)
			if (discardCounts[i] > 0) remaining[state.exact.cardId[player][i]] = discardCounts[i];
		std::vector<int> selected;
		for (int optionIndex = 0; optionIndex < (int)state.options.size(); ++optionIndex) {
			const SelectOption& option = state.options[optionIndex];
			if (option.type != SelectOptionType::Card) continue;
			CardPosition position = option.getCardPosition();
			if (position.playerIndex != player || position.area != AreaType::Hand) continue;
			CardRef ref = state.getCardRef(position);
			if (ref.isNull()) return false;
			int id = state.getCard(ref).cardId;
			auto found = remaining.find(id);
			if (found != remaining.end() && found->second > 0) {
				selected.push_back(optionIndex); found->second--;
			}
		}
		for (const auto& item : remaining) if (item.second != 0) return false;
		if ((int)selected.size() != state.selectMin || state.selectMin != state.selectMax) return false;
		std::sort(selected.begin(), selected.end());
		return advance(state, selected);
	}

	void anonymizeHiddenHandAfterDiscard(State& state, int player,
		const std::vector<int>& originalBounds, const std::vector<int>& discardCounts) {
		PlayerState& ps = state.players[player];
		for (int i = 0; i < ps.hand.size(); ++i) {
			CardRef ref = ps.hand[i];
			if (ref.isNull()) continue;
			state.allCard[ref.cardIndex] = {}; ps.hand[i] = CardRef(0);
		}
		for (int i = 0; i < state.exact.typeCount[player]; ++i) {
			int count = originalBounds[i] - discardCounts[i];
			if (count < 0 || count > 255) throw std::runtime_error("hidden hand residual mismatch");
			state.exact.cardCount[player][i] = (unsigned char)count;
		}
		state.exact.deckUnknown[player] = true;
		state.exact.prizeExchangeable[player] = true;
		state.exact.provisionalOpponentPolicy = false;
		state.exact.clearPending();
	}

	ExactScore solveForcedHiddenHandDiscard(const State& parent, const ExactHiddenState& request,
		const std::vector<int>& action, int keepCount) {
		const int player = request.pendingPlayer;
		if (player < 0 || player >= 2 || player == actor || !parent.exact.profileKnown[player]) return unknown();
		int handSize = 0;
		for (CardRef ref : parent.players[player].hand) {
			if (!ref.isNull()) return unknown();
			handSize++;
		}
		const int discardSize = handSize - keepCount;
		if (discardSize <= 0) return unknown();
		std::vector<int> bounds(parent.exact.typeCount[player]);
		int totalHidden = 0;
		for (int i = 0; i < (int)bounds.size(); ++i) {
			bounds[i] = parent.exact.cardCount[player][i]; totalHidden += bounds[i];
		}
		if (handSize > totalHidden) return unknown();
		struct CountKeyHash { size_t operator()(const std::array<unsigned char, DECK_SIZE>& key) const noexcept {
			size_t value = 1469598103934665603ULL;
			for (unsigned char count : key) { value ^= count; value *= 1099511628211ULL; }
			return value;
		} };
		std::unordered_map<std::array<unsigned char, DECK_SIZE>, ExactScore, CountKeyHash> discardScores;
		ExactWeight totalWeight = chooseCount(totalHidden, handSize), processedWeight;
		ExactFraction lower = ExactFraction::integer(0), upper = ExactFraction::integer(0);
		unsigned long long handWorlds = 0;
		bool boundsSound = true;
		// A root worker normally yields after 20k nodes.  Yielding before one
		// information set has considered every legal discard would restart that
		// minimisation and can never make progress.  Complete a bounded chunk while
		// retaining the wall-clock and RSS checks performed by expired().
		struct QuantumGuard { unsigned long long& limit; unsigned long long previous;
			QuantumGuard(unsigned long long& value, unsigned long long expanded)
				: limit(value), previous(value) {
				if (value != std::numeric_limits<unsigned long long>::max())
					value = std::max(value, expanded + 2'000'000ULL);
			}
			~QuantumGuard() { limit = previous; }
		} quantum(nodeQuantumDeadline, metrics.expanded);
		BoundedCompositionCursor handCursor; handCursor.reset(bounds, handSize);
		std::vector<int> handCounts;
		while (handCursor.next(handCounts)) {
			if (expired()) break;
			ExactWeight handWeight(1);
			for (int i = 0; i < (int)bounds.size(); ++i)
				handWeight = ExactWeight::multiply(handWeight, chooseCount(bounds[i], handCounts[i]));
			BoundedCompositionCursor discardCursor; discardCursor.reset(handCounts, discardSize);
			std::vector<int> discardCounts;
			ExactFraction handLower = ExactFraction::integer(100'000'000);
			ExactFraction handUpper = ExactFraction::integer(100'000'000);
			bool foundAction = false;
			while (discardCursor.next(discardCounts)) {
				if (expired()) break;
				std::array<unsigned char, DECK_SIZE> key{};
				for (int i = 0; i < (int)discardCounts.size(); ++i) key[i] = (unsigned char)discardCounts[i];
				auto cached = discardScores.find(key);
				ExactScore score;
				if (cached != discardScores.end()) { score = cached->second; metrics.successorMerges++; }
				else {
					std::vector<int> representative = discardCounts;
					int left = keepCount;
					for (int i = 0; i < (int)bounds.size() && left > 0; ++i) {
						int take = std::min(left, bounds[i] - discardCounts[i]);
						representative[i] += take; left -= take;
					}
					if (left != 0) return unknown();
					auto child = cloneState(parent);
					try {
						materializeUnknownHand(*child, player, representative);
						if (!advance(*child, action)) return unknown();
						if (!selectHiddenHandDiscard(*child, player, discardCounts)) return unknown();
						anonymizeHiddenHandAfterDiscard(*child, player, bounds, discardCounts);
					} catch (...) { return unknown(); }
					score = solveOwned(std::move(child));
					discardScores.emplace(key, score);
					metrics.enumeratedHiddenWorlds++;
				}
				if (!foundAction || ExactCompare(score.lower, handLower) < 0) handLower = score.lower;
				if (!foundAction || ExactCompare(score.upper, handUpper) < 0) handUpper = score.upper;
				boundsSound = boundsSound && score.boundsSound;
				foundAction = true;
			}
			if (!foundAction || expired()) break;
			lower = ExactFraction::add(lower, handLower.scaled(handWeight, totalWeight));
			upper = ExactFraction::add(upper, handUpper.scaled(handWeight, totalWeight));
			processedWeight += handWeight; metrics.informationSets++; handWorlds++;
		}
		ExactWeight remaining = processedWeight >= totalWeight ? ExactWeight()
			: ExactWeight::subtract(totalWeight, processedWeight);
		if (!remaining.zero()) {
			lower = ExactFraction::add(lower, ExactFraction::integer(-100'000'000).scaled(remaining, totalWeight));
			upper = ExactFraction::add(upper, ExactFraction::integer(100'000'000).scaled(remaining, totalWeight));
			metrics.partialChanceNodes++;
		}
		metrics.rawOutcomes += handWorlds;
		metrics.groupedOutcomes += discardScores.size();
		bool certified = boundsSound && remaining.zero() && ExactCompare(lower, upper) == 0;
		return { lower, upper, {}, certified, boundsSound };
	}

	bool expandRevealBelief(const State& parent, const ExactHiddenState& request,
		const std::vector<int>& action,
		const ExactWeight& baseWeight, const std::array<ExactKnowledgeState, 2>& baseKnowledge,
		std::vector<BeliefWorld>& output) {
		int player = request.pendingPlayer;
		if (player < 0 || player >= 2 || request.pending != ExactPendingType::RevealDeck) return false;
		if (!parent.exact.profileKnown[player]) return false;
		int prizeSize = 0; for (CardRef ref : parent.players[player].prize) if (ref.isNull()) prizeSize++;
		int handSize = 0; for (CardRef ref : parent.players[player].hand) if (ref.isNull()) handSize++;
		int totalHidden = 0;
		std::vector<int> bounds(parent.exact.typeCount[player]);
		for (int i = 0; i < parent.exact.typeCount[player]; ++i) {
			bounds[i] = parent.exact.cardCount[player][i]; totalHidden += bounds[i];
		}
		if (prizeSize < 0 || handSize < 0 || prizeSize + handSize > totalHidden) return false;
		ExactWeight expected = ExactWeight::multiply(baseWeight,
			ExactWeight::multiply(chooseCount(totalHidden, prizeSize), chooseCount(totalHidden - prizeSize, handSize)));
		noteWeight(expected);
		std::string revealKey = keyFor(parent) + "\x1f" "BR3" "\x1f" + actionEquivalenceKey(parent, action)
			+ "\x1f" + baseWeight.text();
		appendSemantic(revealKey, player); appendSemantic(revealKey, request.pendingDetail);
		appendSemantic(revealKey, request.pendingEffectCardId); appendSemantic(revealKey, request.pendingEffectPlayer);
		for (int observer = 0; observer < 2; ++observer) appendKnowledgeKey(revealKey, baseKnowledge[observer]);
		auto [found, inserted] = partialBeliefReveals.try_emplace(revealKey);
		PartialBeliefRevealEntry& partial = found->second;
		if (inserted || !partial.initialized) {
			partial.bounds = bounds; partial.handBounds.resize(bounds.size());
			partial.prizeCounts.resize(bounds.size()); partial.handCounts.resize(bounds.size());
			partial.prizeCursor.reset(bounds, prizeSize); partial.expected = expected;
			partial.initialized = true;
			partial.accountedBytes = revealKey.size() + sizeof(PartialBeliefRevealEntry) + bounds.size() * sizeof(int) * 4;
			partialBytes += partial.accountedBytes;
		} else {
			metrics.partialRevealHits++;
			if (partial.bounds != bounds || partial.expected != expected) return false;
		}
		while (!partial.completed) {
			if (expired()) return false;
			if (!partial.handActive) {
				if (!partial.prizeCursor.next(partial.prizeCounts)) { partial.completed = true; break; }
				for (int i = 0; i < (int)bounds.size(); ++i)
					partial.handBounds[i] = bounds[i] - partial.prizeCounts[i];
				partial.handCursor.reset(partial.handBounds, handSize); partial.handActive = true;
			}
			if (!partial.handCursor.next(partial.handCounts)) { partial.handActive = false; continue; }
				ExactWeight allocation(1);
				for (int i = 0; i < (int)bounds.size(); ++i) {
					allocation = ExactWeight::multiply(allocation, chooseCount(bounds[i], partial.prizeCounts[i]));
					allocation = ExactWeight::multiply(allocation,
						chooseCount(bounds[i] - partial.prizeCounts[i], partial.handCounts[i]));
				}
				ExactWeight worldWeight = ExactWeight::multiply(baseWeight, allocation);
				noteWeight(worldWeight);
				auto child = cloneState(parent);
				try {
					materializeUnknownZones(*child, player, partial.prizeCounts, partial.handCounts);
				} catch (...) { return false; }
				auto knowledge = baseKnowledge;
				int observer = parent.selectPlayer >= 0 ? parent.selectPlayer : actor;
				knowledge[observer].deckKnown[player] = true;
				std::vector<int> observedDeck;
				for (CardRef ref : child->players[player].deck) if (!ref.isNull())
					observedDeck.push_back(child->getCard(ref).cardId);
				std::sort(observedDeck.begin(), observedDeck.end());
				knowledge[observer].publicFacts.push_back('V');
				appendSemantic(knowledge[observer].publicFacts, player);
				knowledge[observer].knownDeckCounts[player].clear();
				for (int id : observedDeck) { appendSemantic(knowledge[observer].publicFacts, id);
					knowledge[observer].knownDeckCounts[player][id]++; }
				knowledge[observer].observationSequence++;
				try { if (!advance(*child, action)) return false; } catch (...) { return false; }
				partial.worlds.push_back({ std::move(child), worldWeight, std::move(knowledge) });
				partial.generated += worldWeight;
				partialBytes += sizeof(State) + sizeof(BeliefWorld) + 128;
				metrics.enumeratedHiddenWorlds++;
		}
		if (partial.generated != expected) {
			metrics.chanceMassMismatches++; metrics.probabilityExact = false; return false;
		}
		output.reserve(output.size() + partial.worlds.size());
		for (const BeliefWorld& world : partial.worlds)
			output.push_back({ cloneState(*world.state), world.weight, world.knowledge });
		return true;
	}

	ExactScore revealAndReplay(const State& parent, const ExactHiddenState& request,
		const std::vector<int>& action) {
		if (pendingArea(request) == AreaType::Hand) {
			int keepCount = 0;
			if (isForcedHiddenHandDiscard(request, keepCount))
				return solveForcedHiddenHandDiscard(parent, request, action, keepCount);
			std::vector<BeliefWorld> worlds;
			auto knowledge = initialKnowledge();
			if (!expandHiddenHandBelief(parent, request, action, ExactWeight(1), knowledge, worlds)
				|| worlds.empty()) return unknown();
			metrics.beliefWorldsBefore += worlds.size();
			return solveBelief(std::move(worlds));
		}
		// A player who searches their own complete deck can distinguish every
		// (hand, prize) allocation represented below.  Different hand counts are
		// visible in their hand; with equal hand counts, different prize counts
		// imply a different observed deck multiset.  Consequently every allocation
		// is a singleton information set and may be evaluated as an exact streaming
		// chance outcome.  This avoids retaining tens of thousands of full State
		// copies without introducing strategy fusion.
		if (request.pendingPlayer == actor && parent.selectPlayer == actor
			&& pendingArea(request) == AreaType::Deck)
			return revealAndReplayOwnDeckStreaming(parent, request, action);
		std::vector<BeliefWorld> worlds;
		auto knowledge = initialKnowledge();
		if (!expandRevealBelief(parent, request, action, ExactWeight(1), knowledge, worlds) || worlds.empty()) return unknown();
		metrics.beliefWorldsBefore += worlds.size();
		return solveBelief(std::move(worlds));
	}

	bool dynamicTurnSearchPartition(const State& parent, const ExactHiddenState& request,
		ExactCardPartition& partition) {
		if (request.pending != ExactPendingType::RevealDeck
			|| request.pendingPlayer != actor || pendingArea(request) != AreaType::Deck) return false;
		partition = turnDependencyPartition(parent, &request).partition;
		return partition.hasCompressedClass();
	}

	ExactScore revealAndReplayPartitionedTurnSearch(const State& parent, const ExactHiddenState& request,
		const std::vector<int>& action, int prizeSize, int totalHidden, const ExactWeight& totalWeight,
		const ExactCardPartition& partition) {
		const int player = request.pendingPlayer;
		std::string revealKey = keyFor(parent) + "\x1fR7-DYNAMIC-TURN\x1f"
			+ partition.schemaKey() + actionEquivalenceKey(parent, action);
		appendSemantic(revealKey, request.pendingDetail); appendSemantic(revealKey, request.pendingEffectCardId);
		appendSemantic(revealKey, request.pendingEffectPlayer);
		auto [found, inserted] = partialPartitionReveals.try_emplace(revealKey);
		PartialPartitionRevealEntry& partial = found->second;
		if (inserted) {
			std::vector<int> relevantIds = partition.visibleCardIds();
			std::vector<int> typeIndex(relevantIds.size(), -1);
			std::vector<int> relevantBounds(relevantIds.size(), 0);
			std::vector<bool> relevantType(parent.exact.typeCount[player], false);
			int totalRelevant = 0;
			for (int i = 0; i < parent.exact.typeCount[player]; ++i) {
				for (int r = 0; r < (int)relevantIds.size(); ++r) if (parent.exact.cardId[player][i] == relevantIds[r]) {
					typeIndex[r] = i; relevantBounds[r] = parent.exact.cardCount[player][i];
					relevantType[i] = true; totalRelevant += relevantBounds[r]; break;
				}
			}
			const int totalIrrelevant = totalHidden - totalRelevant;
			ExactWeight generated;
			for (int relevantPrizeTotal = 0; relevantPrizeTotal <= std::min(prizeSize, totalRelevant); ++relevantPrizeTotal) {
				int irrelevantPrizeTotal = prizeSize - relevantPrizeTotal;
				if (irrelevantPrizeTotal < 0 || irrelevantPrizeTotal > totalIrrelevant) continue;
				BoundedCompositionCursor cursor; cursor.reset(relevantBounds, relevantPrizeTotal);
				std::vector<int> relevantCounts;
				while (cursor.next(relevantCounts)) {
					PartitionRevealAllocation allocation;
					allocation.prizeCounts.assign(parent.exact.typeCount[player], 0);
					allocation.weight = chooseCount(totalIrrelevant, irrelevantPrizeTotal);
					for (int r = 0; r < (int)relevantIds.size(); ++r) {
						if (typeIndex[r] >= 0) allocation.prizeCounts[typeIndex[r]] = relevantCounts[r];
						allocation.weight = ExactWeight::multiply(allocation.weight,
							chooseCount(relevantBounds[r], relevantCounts[r]));
					}
					int left = irrelevantPrizeTotal;
					for (int i = 0; i < parent.exact.typeCount[player] && left > 0; ++i) if (!relevantType[i]) {
						int take = std::min(left, (int)parent.exact.cardCount[player][i]);
						allocation.prizeCounts[i] = take; left -= take;
					}
					if (left != 0 || allocation.weight.zero()) continue;
					generated += allocation.weight;
					partial.allocations.push_back(std::move(allocation));
				}
			}
			if (generated != totalWeight) {
				metrics.chanceMassMismatches++; metrics.probabilityExact = false;
				partialPartitionReveals.erase(found); return unknown();
			}
			if (reverseActionOrder)
				std::reverse(partial.allocations.begin(), partial.allocations.end());
			partial.totalWeight = totalWeight;
			partial.accountedBytes = revealKey.size() + sizeof(PartialPartitionRevealEntry);
			for (const auto& allocation : partial.allocations)
				partial.accountedBytes += sizeof(PartitionRevealAllocation) + allocation.prizeCounts.size() * sizeof(int);
			partialBytes += partial.accountedBytes;
		} else {
			metrics.partialRevealHits++;
			if (partial.totalWeight != totalWeight) return unknown();
		}
		auto incomplete = [&](const ExactScore* current = nullptr, ExactWeight currentWeight = {}) {
			ExactFraction lower = partial.completedLower, upper = partial.completedUpper;
			ExactWeight covered = partial.processedWeight;
			if (current != nullptr) {
				lower = ExactFraction::add(lower, current->lower.scaled(currentWeight, totalWeight));
				upper = ExactFraction::add(upper, current->upper.scaled(currentWeight, totalWeight));
				covered += currentWeight;
			}
			ExactWeight remaining = covered >= totalWeight ? ExactWeight() : ExactWeight::subtract(totalWeight, covered);
			lower = ExactFraction::add(lower, ExactFraction::integer(-100'000'000).scaled(remaining, totalWeight));
			upper = ExactFraction::add(upper, ExactFraction::integer(100'000'000).scaled(remaining, totalWeight));
			return ExactScore{ lower, upper, {}, false };
		};
		std::vector<int> handCounts(parent.exact.typeCount[player], 0);
		while (partial.index < partial.allocations.size()) {
			if (expired()) { metrics.partialChanceNodes++; return incomplete(); }
			const PartitionRevealAllocation& allocation = partial.allocations[partial.index];
			auto world = cloneState(parent);
			try {
				materializeUnknownZones(*world, player, allocation.prizeCounts, handCounts);
				if (!advance(*world, action)) return unknown();
			} catch (...) { return unknown(); }
			std::string quotientKey = observationKeyFor(*world, actor, nullptr, false);
			std::vector<int> relevantIds = partition.visibleCardIds();
			quotientKey += "\x1f" "DYNAMIC-TURN-SEARCH" + partition.schemaKey();
			for (int id : relevantIds) {
				int count = 0;
				for (CardRef ref : world->players[actor].deck) if (!ref.isNull() && world->getCard(ref).cardId == id) count++;
				appendSemantic(quotientKey, count);
			}
			std::string sharedKey = "DYNAMIC-TURN-SEARCH\x1f" + quotientKey;
			ExactScore score;
			bool sharedHit = false;
			auto local = partitionTurnRevealScores.find(quotientKey);
			bool cacheHit = local != partitionTurnRevealScores.end();
			if (cacheHit) score = local->second;
			else if (usingSharedTable) cacheHit = sharedHit = sharedFind(sharedKey, score);
			if (cacheHit) {
				metrics.successorMerges++; metrics.merged++;
				if (sharedHit) metrics.rootSharedTTHits++;
			} else {
				struct StreamingGuard { bool& flag; bool previous;
					StreamingGuard(bool& value) : flag(value), previous(value) { flag = true; }
					~StreamingGuard() { flag = previous; }
				} streaming(singletonRevealStreaming);
				// Root workers normally yield every 20k nodes. Give one concrete world
				// a bounded larger quantum to amortize reconstruction; canonical TT and
				// partial cursors still make the boundary exactly resumable.
				struct QuantumGuard { unsigned long long& limit; unsigned long long previous;
					QuantumGuard(unsigned long long& value, unsigned long long expanded)
						: limit(value), previous(value) {
						if (value != std::numeric_limits<unsigned long long>::max())
							value = std::max(value, expanded + 500'000ULL);
					}
					~QuantumGuard() { limit = previous; }
				} quantum(nodeQuantumDeadline, metrics.expanded);
				score = solveOwned(std::move(world));
				if (score.certified) {
					partitionTurnRevealScores.emplace(std::move(quotientKey), score);
					if (usingSharedTable) sharedStore(std::move(sharedKey), score);
				}
			}
			if (!score.certified) return incomplete(&score, allocation.weight);
			partial.completedLower = ExactFraction::add(partial.completedLower,
				score.lower.scaled(allocation.weight, totalWeight));
			partial.completedUpper = ExactFraction::add(partial.completedUpper,
				score.upper.scaled(allocation.weight, totalWeight));
			partial.processedWeight += allocation.weight; noteWeight(partial.processedWeight);
			partial.index++; metrics.enumeratedHiddenWorlds++;
		}
		if (partial.processedWeight != totalWeight) {
			metrics.chanceMassMismatches++; metrics.probabilityExact = false; return unknown();
		}
		ExactScore result{ partial.completedLower, partial.completedUpper, {},
			ExactCompare(partial.completedLower, partial.completedUpper) == 0, true };
		partialBytes -= std::min(partialBytes, partial.accountedBytes);
		partialPartitionReveals.erase(revealKey);
		return result;
	}

	ExactScore revealAndReplayOwnDeckStreaming(const State& parent, const ExactHiddenState& request,
		const std::vector<int>& action) {
		int player = request.pendingPlayer;
		if (player != actor || parent.selectPlayer != actor
			|| pendingArea(request) != AreaType::Deck) return unknown();
		if (!parent.exact.profileKnown[player]) return unknown();
		int prizeSize = 0; for (CardRef ref : parent.players[player].prize) if (ref.isNull()) prizeSize++;
		int handSize = 0; for (CardRef ref : parent.players[player].hand) if (ref.isNull()) handSize++;
		int totalHidden = 0;
		for (int i = 0; i < parent.exact.typeCount[player]; ++i) totalHidden += parent.exact.cardCount[player][i];
		if (prizeSize < 0 || prizeSize > totalHidden) return unknown();
		ExactWeight totalWeight = ExactWeight::multiply(chooseCount(totalHidden, prizeSize),
			chooseCount(totalHidden - prizeSize, handSize));
		noteWeight(totalWeight);
		if (totalWeight.zero()) return unknown();
		ExactCardPartition turnPartition;
		if (handSize == 0 && dynamicTurnSearchPartition(parent, request, turnPartition))
			return revealAndReplayPartitionedTurnSearch(parent, request, action, prizeSize, totalHidden, totalWeight,
				turnPartition);
		std::string revealKey = keyFor(parent) + "\x1fR4\x1f" + actionEquivalenceKey(parent, action);
		appendSemantic(revealKey, request.pendingDetail);
		appendSemantic(revealKey, request.pendingEffectCardId);
		appendSemantic(revealKey, request.pendingEffectPlayer);
		PartialRevealEntry* partial = partialRevealFor(revealKey, parent.exact.typeCount[player]);
		if (partial == nullptr) return unknown();
		if (!partial->initialized) {
			std::vector<int> bounds(parent.exact.typeCount[player]);
			for (int i = 0; i < (int)bounds.size(); ++i) bounds[i] = parent.exact.cardCount[player][i];
			partial->prizeCursor.reset(bounds, prizeSize);
			partial->prizeCounts.assign(bounds.size(), 0);
			partial->handCounts.assign(bounds.size(), 0);
			partial->totalWeight = totalWeight; partial->initialized = true;
		} else if (partial->totalWeight != totalWeight) {
			return unknown();
		}
		auto incomplete = [&](const ExactScore* current = nullptr, ExactWeight currentWeight = {}) {
			ExactFraction lower = partial->completedLower, upper = partial->completedUpper;
			ExactWeight covered = partial->processedWeight;
			if (current != nullptr) {
				lower = ExactFraction::add(lower, current->lower.scaled(currentWeight, totalWeight));
				upper = ExactFraction::add(upper, current->upper.scaled(currentWeight, totalWeight));
				covered = ExactWeight::add(covered, currentWeight); noteWeight(covered);
			}
			ExactWeight remaining = covered >= totalWeight ? ExactWeight() : ExactWeight::subtract(totalWeight, covered);
			lower = ExactFraction::add(lower, ExactFraction::integer(-100'000'000).scaled(remaining, totalWeight));
			upper = ExactFraction::add(upper, ExactFraction::integer(100'000'000).scaled(remaining, totalWeight));
			if (!lower.valid || !upper.valid) { metrics.arithmeticOverflow = true; return unknown(); }
			return ExactScore{ lower, upper, {}, false };
		};
		while (true) {
			if (expired()) { metrics.partialChanceNodes++; return incomplete(); }
			if (!partial->handActive) {
				if (!partial->prizeCursor.next(partial->prizeCounts)) {
					if (partial->processedWeight != totalWeight) return unknown();
					ExactScore result{ partial->completedLower, partial->completedUpper, {},
						ExactCompare(partial->completedLower, partial->completedUpper) == 0 };
					partialBytes -= std::min(partialBytes, partial->accountedBytes);
					partialReveals.erase(revealKey);
					return result;
				}
				std::vector<int> handBounds(parent.exact.typeCount[player]);
				for (int i = 0; i < (int)handBounds.size(); ++i)
					handBounds[i] = parent.exact.cardCount[player][i] - partial->prizeCounts[i];
				partial->handCursor.reset(handBounds, handSize);
				partial->handActive = true;
			}
			if (!partial->pendingWorld) {
				if (!partial->handCursor.next(partial->handCounts)) {
					partial->handActive = false; continue;
				}
				ExactWeight weight(1);
				for (int i = 0; i < parent.exact.typeCount[player]; ++i) {
					int available = parent.exact.cardCount[player][i];
					weight = ExactWeight::multiply(weight, chooseCount(available, partial->prizeCounts[i]));
					weight = ExactWeight::multiply(weight,
						chooseCount(available - partial->prizeCounts[i], partial->handCounts[i]));
				}
				noteWeight(weight);
				partial->pendingWeight = weight; partial->pendingWorld = true;
			}
			auto world = cloneState(parent);
			try {
				materializeUnknownZones(*world, player, partial->prizeCounts, partial->handCounts);
				if (!advance(*world, action)) return unknown();
			} catch (...) { return unknown(); }
			ExactScore score;
			ExactCardPartition dynamicPartition;
			const bool partitionedTurnSearch = handSize == 0
				&& dynamicTurnSearchPartition(parent, request, dynamicPartition);
			std::string quotientKey;
			std::string sharedQuotientKey;
			if (partitionedTurnSearch) {
				quotientKey = observationKeyFor(*world, actor, nullptr, false);
				std::vector<int> relevantIds = dynamicPartition.visibleCardIds();
				std::vector<int> deckCounts(relevantIds.size(), 0);
				for (CardRef ref : world->players[actor].deck) if (!ref.isNull()) {
					int id = world->getCard(ref).cardId;
					for (int i = 0; i < (int)relevantIds.size(); ++i)
						if (id == relevantIds[i]) { deckCounts[i]++; break; }
				}
				quotientKey += "\x1f" "DYNAMIC-TURN-SEARCH" + dynamicPartition.schemaKey();
				for (int count : deckCounts) appendSemantic(quotientKey, count);
				sharedQuotientKey = "DYNAMIC-TURN-SEARCH\x1f" + quotientKey;
				bool sharedHit = false;
				auto cached = partitionTurnRevealScores.find(quotientKey);
				bool cacheHit = cached != partitionTurnRevealScores.end();
				if (cacheHit) score = cached->second;
				else if (usingSharedTable) cacheHit = sharedHit = sharedFind(sharedQuotientKey, score);
				if (cacheHit) {
					metrics.successorMerges++; metrics.merged++;
					if (sharedHit) metrics.rootSharedTTHits++;
				}
			}
			if (!score.certified) {
				struct StreamingGuard {
					bool& flag; bool previous;
					StreamingGuard(bool& value) : flag(value), previous(value) { flag = true; }
					~StreamingGuard() { flag = previous; }
				} streaming(singletonRevealStreaming);
				score = solveOwned(std::move(world));
				if (partitionedTurnSearch && score.certified) {
					partitionTurnRevealScores.emplace(std::move(quotientKey), score);
					if (usingSharedTable) sharedStore(std::move(sharedQuotientKey), score);
				}
			}
			if (!score.certified) return incomplete(&score, partial->pendingWeight);
			partial->completedLower = ExactFraction::add(partial->completedLower,
				score.lower.scaled(partial->pendingWeight, totalWeight));
			partial->completedUpper = ExactFraction::add(partial->completedUpper,
				score.upper.scaled(partial->pendingWeight, totalWeight));
			if (!partial->completedLower.valid || !partial->completedUpper.valid) {
				metrics.arithmeticOverflow = true; return unknown();
			}
			partial->processedWeight += partial->pendingWeight; noteWeight(partial->processedWeight);
			metrics.enumeratedHiddenWorlds++;
			partial->pendingWeight = ExactWeight(); partial->pendingWorld = false;
		}
	}

	std::string beliefWorldKey(const BeliefWorld& world) const {
		std::string key = keyFor(*world.state);
		for (int observer = 0; observer < 2; ++observer) {
			appendKnowledgeKey(key, world.knowledge[observer]);
		}
		return key;
	}

	void normalizeBelief(std::vector<BeliefWorld>& worlds) {
		std::unordered_map<std::string, size_t, ExactStringHasher> byWorld;
		std::vector<BeliefWorld> normalized;
		normalized.reserve(worlds.size());
		for (BeliefWorld& world : worlds) {
			std::string key = beliefWorldKey(world);
			auto [found, inserted] = byWorld.emplace(std::move(key), normalized.size());
			if (inserted) normalized.push_back(std::move(world));
			else {
				normalized[found->second].weight += world.weight;
				metrics.beliefWorldsAfter++;
			}
		}
		worlds = std::move(normalized);
		ExactWeight common;
		for (const BeliefWorld& world : worlds)
			common = common.zero() ? world.weight : ExactWeight::gcd(common, world.weight);
		if (!common.zero() && common != ExactWeight(1)) {
			for (BeliefWorld& world : worlds) {
				auto divided = ExactWeight::divideRemainder(world.weight, common);
				if (!divided.second.zero()) throw std::runtime_error("belief GCD normalization failed");
				world.weight = std::move(divided.first);
			}
		}
	}

	ExactWeight beliefMass(const std::vector<BeliefWorld>& worlds) const {
		ExactWeight total; for (const BeliefWorld& world : worlds) total += world.weight; return total;
	}

	ExactScore aggregateBeliefScores(const std::vector<std::pair<ExactScore, ExactWeight>>& scores,
		const ExactWeight& total) {
		if (scores.empty() || total.zero()) return unknown();
		ExactFraction lower = ExactFraction::integer(0), upper = ExactFraction::integer(0);
		bool certified = true;
		bool boundsSound = true;
		ExactWeight processed;
		for (const auto& item : scores) {
			lower = ExactFraction::add(lower, item.first.lower.scaled(item.second, total));
			upper = ExactFraction::add(upper, item.first.upper.scaled(item.second, total));
			processed += item.second;
			certified = certified && item.first.certified;
			boundsSound = boundsSound && item.first.boundsSound;
		}
		if (processed != total) {
			metrics.chanceMassMismatches++; metrics.probabilityExact = false; return unknown();
		}
		return { lower, upper, {}, certified && boundsSound && ExactCompare(lower, upper) == 0, boundsSound };
	}

	bool settleBeliefState(State& state) {
		try {
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
				stepExact(state); metrics.expanded++;
				if (expired()) return false;
			}
			return true;
		} catch (const std::exception& error) {
			metrics.exceptions++; metrics.lastException = error.what(); return false;
		} catch (...) {
			metrics.exceptions++; metrics.lastException = "belief automatic transition"; return false;
		}
	}

	std::string beliefControlKey(const BeliefWorld& world) const {
		const State& state = *world.state;
		std::string key;
		if (state.isFinish()) key = "F";
		else if (IsExactTurnLeaf(state)) key = "L";
		else if (state.exact.pending != ExactPendingType::None) {
			key = "P"; appendSemantic(key, (int)state.exact.pending);
			appendSemantic(key, state.exact.pendingPlayer); appendSemantic(key, state.exact.pendingCount);
			if (state.exact.pendingPlayer >= 0) {
				const PlayerState& ps = state.players[state.exact.pendingPlayer];
				appendSemantic(key, state.exact.pending == ExactPendingType::Draw ? ps.deck.size() : ps.prize.size());
			}
		} else if (state.selectType == SelectType::YesNo && state.selectContext == SelectContext::CoinHead) key = "C";
		else {
			key = "D"; appendSemantic(key, state.selectPlayer); appendSemantic(key, (int)state.selectType);
		}
		return key;
	}

	ExactScore solveBeliefInformationSet(std::vector<BeliefWorld> worlds) {
		if (worlds.empty()) return unknown();
		State& representative = *worlds.front().state;
		const int decisionPlayer = representative.selectPlayer;
		const std::string expectedObservation = observationKeyFor(representative, decisionPlayer,
			&worlds.front().knowledge[decisionPlayer]);
		for (const BeliefWorld& world : worlds) {
			if (world.state->selectPlayer != decisionPlayer
				|| observationKeyFor(*world.state, decisionPlayer, &world.knowledge[decisionPlayer]) != expectedObservation) {
				metrics.illegalInformationSetSplits++; metrics.informationSetSafe = false;
				metrics.hiddenInformationLeakDetected = true; return unknown();
			}
		}
		metrics.informationSets++;
		if (worlds.size() > 1) metrics.strategyFusionPrevented += worlds.size() - 1;
		struct CommonAction { std::vector<int> representative; std::vector<std::string> semantic; };
		std::vector<CommonAction> actions;
		if (!forEachLegalAction(representative, [&](const std::vector<int>& action) {
			actions.push_back({ action, semanticAction(representative, action) }); return true;
		})) return unknown();
		if (actions.empty()) return unknown();
		const bool maximize = decisionPlayer == actor;
		ExactScore best;
		bool first = true;
		bool allBoundsSound = true;
		ExactFraction decisionLower = ExactFraction::integer(maximize ? -100'000'000 : 100'000'000);
		ExactFraction decisionUpper = ExactFraction::integer(maximize ? -100'000'000 : 100'000'000);
		std::vector<ExactScore> candidateScores;
		candidateScores.reserve(actions.size());
		size_t bestIndex = 0;
		for (const CommonAction& common : actions) {
			if (expired()) return unknown();
			std::vector<BeliefWorld> children;
			for (const BeliefWorld& world : worlds) {
				std::vector<int> mapped;
				if (!remapAction(*world.state, common.semantic, mapped)) {
					metrics.illegalInformationSetSplits++; metrics.informationSetSafe = false;
					metrics.hiddenInformationLeakDetected = true; return unknown();
				}
				auto child = cloneState(*world.state);
				if (!advance(*child, mapped)) return unknown();
				if (child->exact.pending == ExactPendingType::RevealDeck) {
					bool expanded = pendingArea(child->exact) == AreaType::Hand
						? expandHiddenHandBelief(*world.state, child->exact, mapped, world.weight, world.knowledge, children)
						: expandRevealBelief(*world.state, child->exact, mapped, world.weight, world.knowledge, children);
					if (!expanded) return unknown();
				} else children.push_back({ std::move(child), world.weight, world.knowledge });
			}
			ExactScore score = solveBelief(std::move(children));
			candidateScores.push_back(score);
			allBoundsSound = allBoundsSound && score.boundsSound;
			if (maximize) {
				if (first || ExactCompare(score.lower, decisionLower) > 0) decisionLower = score.lower;
				if (first || ExactCompare(score.upper, decisionUpper) > 0) decisionUpper = score.upper;
			} else {
				if (first || ExactCompare(score.lower, decisionLower) < 0) decisionLower = score.lower;
				if (first || ExactCompare(score.upper, decisionUpper) < 0) decisionUpper = score.upper;
			}
			if (first || (maximize ? ExactCompare(score.lower, best.lower) > 0
				: ExactCompare(score.upper, best.upper) < 0)) {
				best = score; best.action = common.representative; bestIndex = candidateScores.size() - 1; first = false;
			}
		}
		// A selected action's exact value is not enough to certify an
		// information-set decision.  Every alternative must have sound bounds
		// that cannot beat it; otherwise keep the decision resumable.
		bool haveOther = false;
		ExactFraction bestOtherUpper;
		ExactFraction bestOtherLower;
		if (!first) {
			for (size_t index = 0; index < candidateScores.size(); ++index) {
				if (index == bestIndex) continue;
				const ExactScore& score = candidateScores[index];
				if (!haveOther) {
					bestOtherUpper = score.upper;
					bestOtherLower = score.lower;
					haveOther = true;
				} else if (maximize) {
					if (ExactCompare(score.upper, bestOtherUpper) > 0) bestOtherUpper = score.upper;
				} else if (ExactCompare(score.lower, bestOtherLower) < 0) bestOtherLower = score.lower;
			}
		}
		if (!first) {
			const bool selectedProven = maximize
				? (!haveOther || ExactCompare(best.lower, bestOtherUpper) >= 0)
				: (!haveOther || ExactCompare(best.upper, bestOtherLower) <= 0);
			best.lower = decisionLower;
			best.upper = decisionUpper;
			best.certified = best.certified && allBoundsSound && selectedProven
				&& ExactCompare(best.lower, best.upper) == 0;
			best.boundsSound = allBoundsSound;
		}
		return best;
	}

	ExactScore solveBelief(std::vector<BeliefWorld> worlds) {
		struct DepthGuard { int& value; DepthGuard(int& v) : value(v) { ++value; } ~DepthGuard() { --value; } } guard(recursionDepth);
		if (worlds.empty() || recursionDepth > 384 || expired()) return unknown();
		metrics.beliefNodes++; metrics.expanded++;
		for (BeliefWorld& world : worlds) if (!settleBeliefState(*world.state)) return unknown();
		normalizeBelief(worlds);
		ExactWeight total = beliefMass(worlds); noteWeight(total, false);
		std::vector<std::string> beliefKeyParts;
		beliefKeyParts.reserve(worlds.size());
		for (const BeliefWorld& world : worlds)
			beliefKeyParts.push_back(beliefWorldKey(world) + "@" + world.weight.text());
		std::sort(beliefKeyParts.begin(), beliefKeyParts.end());
		PlayerInformationStateV3 informationState;
		informationState.observer = actor; informationState.environmentPriorId = environmentPriorId;
		informationState.evaluatorSchema = evaluator ? evaluator->schemaVersion() : 0;
		informationState.evaluatorModel = evaluator ? evaluator->modelHash() : 0;
		informationState.normalizedWorlds = std::move(beliefKeyParts);
		std::string beliefKey = informationState.canonicalKey();
		auto cachedBelief = beliefTransposition.find(beliefKey);
		if (cachedBelief != beliefTransposition.end()) { metrics.merged++; return cachedBelief->second; }
		auto finish = [&](ExactScore result) {
			if (result.certified && beliefTransposition.size() < 250'000
				&& beliefTranspositionBytes + beliefKey.size() + sizeof(ExactScore) + 64 < 256ULL * 1024ULL * 1024ULL) {
				auto [_, inserted] = beliefTransposition.emplace(beliefKey, result);
				if (inserted) beliefTranspositionBytes += beliefKey.size() + sizeof(ExactScore) + 64;
			}
			return result;
		};

		std::unordered_map<std::string, std::vector<BeliefWorld>, ExactStringHasher> controlGroups;
		for (BeliefWorld& world : worlds) controlGroups[beliefControlKey(world)].push_back(std::move(world));
		if (controlGroups.size() > 1) {
			std::vector<std::pair<ExactScore, ExactWeight>> scores;
			for (auto& item : controlGroups) {
				ExactWeight mass = beliefMass(item.second);
				scores.push_back({ solveBelief(std::move(item.second)), mass });
			}
			return finish(aggregateBeliefScores(scores, total));
		}
		worlds = std::move(controlGroups.begin()->second);
		State& representative = *worlds.front().state;

		if (representative.isFinish() || IsExactTurnLeaf(representative)) {
			std::unordered_map<std::string, std::vector<BeliefWorld>, ExactStringHasher> observations;
			for (BeliefWorld& world : worlds) {
				observations[observationKeyFor(*world.state, actor, &world.knowledge[actor])].push_back(std::move(world));
			}
			std::vector<std::pair<ExactScore, ExactWeight>> scores;
			for (auto& item : observations) {
				ExactWeight mass = beliefMass(item.second);
				bool policyCertified = true;
				for (const BeliefWorld& world : item.second)
					policyCertified = policyCertified && !world.state->exact.provisionalOpponentPolicy;
				std::vector<std::string> beliefTokens;
				for (const BeliefWorld& world : item.second)
					beliefTokens.push_back(beliefWorldKey(world) + "@" + world.weight.text());
				std::sort(beliefTokens.begin(), beliefTokens.end());
				std::string cacheKey = item.first + "#" + std::to_string(evaluator ? evaluator->modelHash() : 0);
				for (const std::string& token : beliefTokens) { appendSemantic(cacheKey, token.size()); cacheKey += token; }
				auto cached = evaluationCache.find(cacheKey);
				long long value;
				if (cached != evaluationCache.end()) { value = cached->second; metrics.evaluatorCacheHits++; }
				else { value = evaluateBeliefInformationState(item.second, mass); evaluationCache.emplace(std::move(cacheKey), value); }
					ExactScore leaf = policyCertified
						? ExactScore{ ExactFraction::integer(value), ExactFraction::integer(value), {}, true, true }
						: blockedUnknown();
					if (!policyCertified) metrics.provisionalOpponentPolicyNodes++;
					scores.push_back({ leaf, mass });
				metrics.leaves++;
			}
			return finish(aggregateBeliefScores(scores, total));
		}

		if (representative.exact.pending == ExactPendingType::Opaque
			|| representative.exact.pending == ExactPendingType::RevealDeck) return unknown();

		if (representative.exact.pending == ExactPendingType::Draw
			|| representative.exact.pending == ExactPendingType::TakePrize) {
			std::vector<BeliefWorld> children;
			ExactWeight expected, generated;
			for (BeliefWorld& world : worlds) {
				auto types = chanceCardTypes(*world.state);
				if (types.empty()) return unknown();
				ExactWeight localTotal; for (const auto& type : types) localTotal += type.second;
				expected += ExactWeight::multiply(world.weight, localTotal);
				for (const auto& type : types) {
					auto child = cloneState(*world.state);
					try {
					if (world.state->exact.pending == ExactPendingType::Draw) resolveDraw(*child, type.first);
					else resolvePrize(*child, type.first);
					} catch (...) { return unknown(); }
					auto knowledge = world.knowledge;
					int pendingPlayer = world.state->exact.pendingPlayer;
					appendKnowledgeFact(knowledge[pendingPlayer],
						world.state->exact.pending == ExactPendingType::Draw ? 'D' : 'P', type.first);
					if (world.state->exact.pending == ExactPendingType::Draw) {
						if (!knowledge[pendingPlayer].knownTop[pendingPlayer].empty())
							knowledge[pendingPlayer].knownTop[pendingPlayer].erase(knowledge[pendingPlayer].knownTop[pendingPlayer].begin());
						auto ownKnown = knowledge[pendingPlayer].knownDeckCounts[pendingPlayer].find(type.first);
						if (ownKnown != knowledge[pendingPlayer].knownDeckCounts[pendingPlayer].end() && --ownKnown->second <= 0)
							knowledge[pendingPlayer].knownDeckCounts[pendingPlayer].erase(ownKnown);
						for (int observer = 0; observer < 2; ++observer) if (observer != pendingPlayer) {
							knowledge[observer].deckKnown[pendingPlayer] = false;
							knowledge[observer].knownDeckCounts[pendingPlayer].clear();
							knowledge[observer].knownTop[pendingPlayer].clear();
							knowledge[observer].knownBottom[pendingPlayer].clear();
						}
					} else {
						auto known = knowledge[pendingPlayer].knownPrizeCounts[pendingPlayer].find(type.first);
						if (known != knowledge[pendingPlayer].knownPrizeCounts[pendingPlayer].end() && --known->second <= 0)
							knowledge[pendingPlayer].knownPrizeCounts[pendingPlayer].erase(known);
					}
					ExactWeight weight = ExactWeight::multiply(world.weight, type.second);
					generated += weight;
					children.push_back({ std::move(child), weight, std::move(knowledge) });
				}
			}
			if (generated != expected) {
				metrics.chanceMassMismatches++; metrics.probabilityExact = false; return unknown();
			}
			return finish(solveBelief(std::move(children)));
		}

		if (representative.selectType == SelectType::YesNo && representative.selectContext == SelectContext::CoinHead) {
			std::vector<BeliefWorld> children;
			for (BeliefWorld& world : worlds) for (int option = 0; option < 2; ++option) {
					auto child = cloneState(*world.state);
				if (!advance(*child, { option })) return unknown();
				auto knowledge = world.knowledge;
				appendKnowledgeFact(knowledge[0], 'C', option); appendKnowledgeFact(knowledge[1], 'C', option);
				children.push_back({ std::move(child), world.weight, std::move(knowledge) });
			}
			return finish(solveBelief(std::move(children)));
		}

		std::unordered_map<std::string, std::vector<BeliefWorld>, ExactStringHasher> informationSets;
		for (BeliefWorld& world : worlds) {
			int observer = world.state->selectPlayer;
			informationSets[observationKeyFor(*world.state, observer, &world.knowledge[observer])].push_back(std::move(world));
		}
		std::vector<std::pair<ExactScore, ExactWeight>> scores;
		for (auto& item : informationSets) {
			ExactWeight mass = beliefMass(item.second);
			scores.push_back({ solveBeliefInformationSet(std::move(item.second)), mass });
		}
		return finish(aggregateBeliefScores(scores, total));
	}

	void resolveDraw(State& state, int cardId) {
		int player = state.exact.pendingPlayer;
		PlayerState& ps = state.players[player];
		int index = -1;
		if (state.exact.deckUnknown[player]) {
			for (int i = 0; i < ps.deck.size(); ++i) {
				if (!ps.deck[i].isNull() && state.getCard(ps.deck[i]).cardId == cardId) { index = i; break; }
			}
			if (index < 0) {
				for (int i = 0; i < ps.deck.size(); ++i) if (ps.deck[i].isNull()) { index = i; break; }
				if (index < 0) throw std::runtime_error("hidden deck slot missing");
				materialize(state, player, AreaType::Deck, index, cardId);
			}
			decrementPool(state, player, cardId);
		} else {
			for (int i = 0; i < ps.deck.size(); ++i) if (state.getCard(ps.deck[i]).cardId == cardId) { index = i; break; }
			if (index < 0) throw std::runtime_error("draw card missing");
		}
		CardRef ref = MoveCard(state, player, AreaType::Deck, index, AreaType::Hand);
		LogDraw(state, player, ref);
		if (--state.exact.pendingCount == 0) state.exact.clearPending();
	}

	void resolvePrize(State& state, int cardId) {
		int player = state.exact.pendingPlayer;
		PlayerState& ps = state.players[player];
		int index = -1;
		for (int i = 0; i < ps.prize.size(); ++i) if (ps.prize[i].isNull()) { index = i; break; }
		if (index < 0) {
			for (int i = 0; i < ps.prize.size(); ++i) if (state.getCard(ps.prize[i]).cardId == cardId) { index = i; break; }
		} else {
			materialize(state, player, AreaType::Prize, index, cardId); decrementPool(state, player, cardId);
		}
		CardRef ref = ps.prize[index];
		state.targetList.push_back(state.makeAreaRef(ref));
		if (--state.exact.pendingCount == 0) {
			state.exact.clearPending();
			SelectedPrizeTarget(state);
		}
	}

	std::vector<std::pair<int, ExactWeight>> chanceCardTypes(const State& state) const {
		std::vector<std::pair<int, ExactWeight>> result;
		int player = state.exact.pendingPlayer;
		if (player != actor && state.exact.deckUnknown[player] && !state.exact.profileKnown[player]) return result;
		if (state.exact.deckUnknown[player]) {
			for (int i = 0; i < state.exact.typeCount[player]; ++i) if (state.exact.cardCount[player][i] > 0)
				result.push_back({ state.exact.cardId[player][i], ExactWeight(state.exact.cardCount[player][i]) });
		} else {
			std::unordered_map<int, unsigned long long> counts;
			const auto& list = state.exact.pending == ExactPendingType::Draw ? state.players[player].deck : state.players[player].prize;
			for (CardRef ref : list) if (!ref.isNull()) counts[state.getCard(ref).cardId]++;
			for (auto [id, count] : counts) result.push_back({ id, ExactWeight(count) });
		}
		std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
			return left.first < right.first;
		});
		return result;
	}

	std::unordered_set<int> collectAssumedInert(const State& state) const {
		std::unordered_set<int> inert;
		for (const auto& item : actorProfileCount) {
			if (ExactSkeleton::IsTurnInertCardId(state, actor, item.first)) {
				inert.insert(item.first);
			}
		}
		return inert;
	}

	bool optionReferencesInert(const State& state, int optionIndex,
		const std::unordered_set<int>& assumedInert) const {
		if (optionIndex < 0 || optionIndex >= (int)state.options.size()) return false;
		const SelectOption& option = state.options[optionIndex];
		auto checkRef = [&](AreaType area, int index) {
			try {
				CardRef ref = state.getCardRef(area, index, state.selectPlayer);
				if (ref.isNull()) return false;
				return assumedInert.contains(state.getCard(ref).cardId);
			} catch (...) { return false; }
		};
		switch (option.type) {
		case SelectOptionType::Play: return checkRef(AreaType::Hand, option.param0);
		case SelectOptionType::Attach:
		case SelectOptionType::Evolve: return checkRef((AreaType)option.param0, option.param1);
		case SelectOptionType::Ability: return checkRef((AreaType)option.param0, option.param1);
		case SelectOptionType::Card: {
			CardPosition pos = option.getCardPosition();
			if (pos.area == AreaType::Hand || pos.area == AreaType::Deck || pos.area == AreaType::Prize)
				return checkRef(pos.area, pos.areaIndex);
			return false;
		}
		default: return false;
		}
	}

	int internSkeletonNode(ExactSkeleton::Dag& dag, ExactSkeleton::NodeKind kind, std::string internKey) {
		auto found = dag.intern.find(internKey);
		if (found != dag.intern.end()) {
			// Only reuse fully built nodes. Hitting an in-progress node would create
			// a cycle and stack-overflow the guided walk.
			if (dag.nodes[found->second].complete) return found->second;
			internKey.append("#");
			internKey.append(std::to_string(dag.nodes.size()));
		}
		ExactSkeleton::DagNode node;
		node.kind = kind;
		node.internKey = internKey;
		node.complete = false;
		int id = (int)dag.nodes.size();
		dag.nodes.push_back(std::move(node));
		dag.intern.emplace(std::move(internKey), id);
		return id;
	}

	bool sameContinuationSignature(int leftId, int rightId) {
		return continuationIdentityKey(leftId) == continuationIdentityKey(rightId);
	}

	bool canMergeInertIdentities(const State& state, int leftId, int rightId) {
		if (leftId == rightId) return true;
		if (!ExactSkeleton::IsTurnInertCardId(state, actor, leftId)) return false;
		if (!ExactSkeleton::IsTurnInertCardId(state, actor, rightId)) return false;
		return sameContinuationSignature(leftId, rightId);
	}

	int expandSkeletonNode(ExactStatePtr owned, ExactSkeleton::Dag& dag, int depthLimit) {
		if (dag.inertGuardFailed) return -1;
		if (dag.expandedNodes > 200'000ULL) {
			dag.inertGuardFailed = true;
			metrics.skeletonGuardFallbacks++;
			metrics.skeletonGuardNodeCap++;
			return -1;
		}
		if (depthLimit <= 0 || expired()) {
			return internSkeletonNode(dag, ExactSkeleton::NodeKind::Blocked, "BLOCKED");
		}
		State& state = *owned;
		metrics.expanded++;
		dag.expandedNodes++;
		try {
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
				stepExact(state);
				metrics.expanded++;
				dag.expandedNodes++;
				if (expired()) break;
			}
		} catch (...) {
			dag.inertGuardFailed = true;
			return -1;
		}
		if (state.isFinish() || IsExactTurnLeaf(state)) {
			metrics.leaves++;
			int leaf = internSkeletonNode(dag, ExactSkeleton::NodeKind::Leaf, keyFor(state) + "\x1f" "LEAF");
			dag.nodes[leaf].complete = true;
			return leaf;
		}
		for (int i = 0; i < (int)state.options.size(); ++i) {
			if (optionReferencesInert(state, i, dag.assumedInert)) {
				dag.inertGuardFailed = true;
				metrics.skeletonGuardFallbacks++;
				metrics.skeletonGuardInertLegal++;
				return -1;
			}
		}
		if (state.selectType != SelectType::Main && state.selectType != SelectType::Attack
			&& state.selectMin == state.selectMax && state.selectMin >= 1
			&& (int)state.options.size() == state.selectMax) {
			metrics.macroCollapsedTransitions++;
			dag.macroCollapsed++;
			ExactSmallAction only;
			only.clear();
			for (int i = 0; i < state.selectMax; ++i) only.push_back(i);
			if (!advance(state, only)) { dag.inertGuardFailed = true; return -1; }
			return expandSkeletonNode(std::move(owned), dag, depthLimit - 1);
		}
		if (state.exact.pending == ExactPendingType::Draw || state.exact.pending == ExactPendingType::TakePrize) {
			dag.interiorChanceNodes++;
			metrics.skeletonInteriorChances++;
			std::string chanceKey = keyFor(state) + "\x1f" "CHANCE";
			if (state.exact.pendingCount > 1) {
				// Depth-1+ interior multi-draw: share the Main prefix, then per-member
				// solveOwned from this bridge (exact; TT still shared across members).
				std::string bridgeKey = chanceKey + "\x1f" "BRIDGE";
				auto existingBridge = dag.intern.find(bridgeKey);
				if (existingBridge != dag.intern.end() && dag.nodes[existingBridge->second].complete)
					return existingBridge->second;
				int bridge = internSkeletonNode(dag, ExactSkeleton::NodeKind::SolveBridge, bridgeKey);
				dag.nodes[bridge].complete = true;
				return bridge;
			}
			auto existing = dag.intern.find(chanceKey);
			if (existing != dag.intern.end() && dag.nodes[existing->second].complete)
				return existing->second;
			int chanceNode = internSkeletonNode(dag, ExactSkeleton::NodeKind::Chance, chanceKey);
			auto types = chanceCardTypes(state);
			if (types.empty()) { dag.inertGuardFailed = true; return -1; }

			auto appendChanceEdge = [&](std::string label, std::vector<int> atomCounts, ExactWeight weight,
				ExactStatePtr childState) -> bool {
				int child = expandSkeletonNode(std::move(childState), dag, depthLimit - 1);
				if (dag.inertGuardFailed || child < 0) return false;
				ExactSkeleton::DagEdge edge;
				edge.label = std::move(label);
				edge.child = child;
				edge.atomCounts = std::move(atomCounts);
				edge.weight = weight;
				dag.nodes[chanceNode].edges.push_back(std::move(edge));
				return true;
			};

			{
				std::vector<bool> merged(types.size(), false);
				for (int ti = 0; ti < (int)types.size(); ++ti) {
					if (merged[ti]) continue;
					std::vector<int> group = { ti };
					ExactWeight weight = types[ti].second;
					for (int tj = ti + 1; tj < (int)types.size(); ++tj) {
						if (merged[tj]) continue;
						if (!canMergeInertIdentities(state, types[ti].first, types[tj].first)) continue;
						merged[tj] = true;
						group.push_back(tj);
						weight += types[tj].second;
					}
					merged[ti] = true;
					auto childState = cloneState(state);
					try {
						if (state.exact.pending == ExactPendingType::Draw) resolveDraw(*childState, types[ti].first);
						else resolvePrize(*childState, types[ti].first);
					} catch (...) { dag.inertGuardFailed = true; return -1; }
					std::string label = "CHANCE:";
					label.append(std::to_string(types[ti].first));
					if (group.size() > 1) {
						label.append(":SIG");
						for (int gi : group) { label.push_back('+'); label.append(std::to_string(types[gi].first)); }
					}
					std::vector<int> atomCounts(types.size(), 0);
					for (int gi : group) atomCounts[gi] = 1;
					if (!appendChanceEdge(std::move(label), std::move(atomCounts), weight, std::move(childState)))
						return -1;
				}
			}
			if (dag.nodes[chanceNode].edges.empty()) { dag.inertGuardFailed = true; return -1; }
			dag.nodes[chanceNode].complete = true;
			return chanceNode;
		}
		if (state.selectType == SelectType::YesNo && state.selectContext == SelectContext::CoinHead) {
			// Exact 50/50 coin as a skeleton Chance node (same mass as coinChance).
			std::string coinKey = keyFor(state) + "\x1f" "COIN";
			auto existingCoin = dag.intern.find(coinKey);
			if (existingCoin != dag.intern.end() && dag.nodes[existingCoin->second].complete)
				return existingCoin->second;
			int coinNode = internSkeletonNode(dag, ExactSkeleton::NodeKind::Chance, coinKey);
			for (int option = 0; option < 2; ++option) {
				auto childState = cloneState(state);
				if (!advance(*childState, { option })) { dag.inertGuardFailed = true; return -1; }
				int child = expandSkeletonNode(std::move(childState), dag, depthLimit - 1);
				if (dag.inertGuardFailed || child < 0) return -1;
				ExactSkeleton::DagEdge edge;
				edge.label = std::string("COIN:") + (char)('0' + option);
				edge.child = child;
				edge.weight = ExactWeight(1);
				dag.nodes[coinNode].edges.push_back(std::move(edge));
			}
			dag.nodes[coinNode].complete = true;
			return coinNode;
		}
		const bool maximize = state.selectPlayer == actor;
		std::string decisionKey = keyFor(state) + (maximize ? "\x1f" "MAX" : "\x1f" "MIN");
		auto existingDecision = dag.intern.find(decisionKey);
		if (existingDecision != dag.intern.end() && dag.nodes[existingDecision->second].complete)
			return existingDecision->second;
		int decisionNode = internSkeletonNode(dag,
			maximize ? ExactSkeleton::NodeKind::DecisionMax : ExactSkeleton::NodeKind::DecisionMin,
			decisionKey);
		bool completed = forEachLegalAction(state, [&](const ExactSmallAction& action) {
			if (dag.inertGuardFailed) return false;
			std::string label = actionEquivalenceKey(state, action);
			auto child = cloneState(state);
			if (!advance(*child, action)) return true;
			int childId = expandSkeletonNode(std::move(child), dag, depthLimit - 1);
			if (dag.inertGuardFailed || childId < 0) return false;
			ExactSkeleton::DagEdge edge;
			edge.label = std::move(label);
			edge.child = childId;
			dag.nodes[decisionNode].edges.push_back(std::move(edge));
			return true;
		});
		if (!completed || dag.inertGuardFailed) return -1;
		if (dag.nodes[decisionNode].edges.empty()) {
			dag.inertGuardFailed = true;
			return -1;
		}
		dag.nodes[decisionNode].complete = true;
		return decisionNode;
	}

	ExactWeight memberChanceEdgeWeight(const std::vector<std::pair<int, ExactWeight>>& types,
		const std::vector<int>& atomCounts) const {
		if (atomCounts.size() != types.size()) return ExactWeight();
		int take = 0;
		for (int n : atomCounts) take += n;
		if (take <= 1) {
			ExactWeight sum;
			for (int i = 0; i < (int)atomCounts.size(); ++i) if (atomCounts[i] > 0) sum += types[i].second;
			return sum;
		}
		ExactWeight weight(1);
		for (int i = 0; i < (int)atomCounts.size(); ++i) {
			if (atomCounts[i] <= 0) continue;
			if (!types[i].second.fitsUnsignedLongLong()) return ExactWeight();
			int available = (int)types[i].second.unsignedLongLong();
			if (atomCounts[i] > available) return ExactWeight();
			weight = ExactWeight::multiply(weight, chooseCount(available, atomCounts[i]));
		}
		return weight;
	}

	ExactScore walkSkeletonNode(ExactSkeleton::Dag& dag, int nodeId, ExactStatePtr owned, int depthLimit = 96) {
		if (nodeId < 0 || nodeId >= (int)dag.nodes.size() || !owned || depthLimit <= 0) {
			dag.walkFailed = true; return unknown();
		}
		const ExactSkeleton::DagNode& node = dag.nodes[nodeId];
		if (node.kind == ExactSkeleton::NodeKind::Blocked) { dag.walkFailed = true; return unknown(); }
		State& state = *owned;
		try {
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
				stepExact(state);
			}
			// Mirror expand's macro-collapse so member state aligns with interned nodes.
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None
				&& state.selectType != SelectType::Main
				&& state.selectType != SelectType::Attack
				&& state.selectType != SelectType::YesNo
				&& state.selectMin == state.selectMax && state.selectMin >= 1
				&& (int)state.options.size() == state.selectMax) {
				ExactSmallAction only;
				only.clear();
				for (int i = 0; i < state.selectMax; ++i) only.push_back(i);
				if (!advance(state, only)) { dag.walkFailed = true; return unknown(); }
				while (!state.isFinish() && !IsExactTurnLeaf(state)
					&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
					stepExact(state);
				}
			}
		} catch (...) { dag.walkFailed = true; return unknown(); }

		if (node.kind == ExactSkeleton::NodeKind::Leaf) {
			if (!(state.isFinish() || IsExactTurnLeaf(state))) { dag.walkFailed = true; return unknown(); }
			return boundaryScore(state, evaluate(state));
		}

		if (node.kind == ExactSkeleton::NodeKind::SolveBridge) {
			// Shared Main prefix ended at an interior multi-draw / opaque suffix.
			return solveOwned(std::move(owned));
		}

		if (node.kind == ExactSkeleton::NodeKind::Chance) {
			const bool coinNode = !node.edges.empty()
				&& node.edges.front().label.rfind("COIN:", 0) == 0;
			if (!coinNode
				&& state.exact.pending != ExactPendingType::Draw
				&& state.exact.pending != ExactPendingType::TakePrize) {
				dag.walkFailed = true; return unknown();
			}
			if (coinNode
				&& !(state.selectType == SelectType::YesNo
					&& state.selectContext == SelectContext::CoinHead)) {
				dag.walkFailed = true; return unknown();
			}
			auto types = coinNode ? std::vector<std::pair<int, ExactWeight>>{}
				: chanceCardTypes(state);
			if (!coinNode && types.empty()) { dag.walkFailed = true; return unknown(); }
			ExactFraction lower = ExactFraction::integer(0), upper = ExactFraction::integer(0);
			bool certified = true;
			bool boundsSound = true;
			ExactWeight total;
			struct Branch { ExactWeight weight; ExactScore score; };
			std::vector<Branch> branches;
			branches.reserve(node.edges.size());
			for (const ExactSkeleton::DagEdge& edge : node.edges) {
				ExactWeight weight;
				if (edge.label.rfind("COIN:", 0) == 0) {
					weight = edge.weight.zero() ? ExactWeight(1) : edge.weight;
				} else {
					weight = memberChanceEdgeWeight(types, edge.atomCounts);
				}
				if (weight.zero()) { dag.walkFailed = true; return unknown(); }
				auto childState = cloneState(state);
				try {
					if (edge.label.rfind("COIN:", 0) == 0) {
						int option = edge.label.size() >= 6 ? edge.label[5] - '0' : -1;
						if (option < 0 || option > 1 || !advance(*childState, { option })) {
							dag.walkFailed = true; return unknown();
						}
					} else if (state.exact.pendingCount > 1 || edge.label.rfind("CHANCE-MULTI:", 0) == 0) {
						for (int i = 0; i < (int)edge.atomCounts.size(); ++i)
							for (int n = 0; n < edge.atomCounts[i]; ++n) {
								if (state.exact.pending == ExactPendingType::Draw)
									resolveDraw(*childState, types[i].first);
								else resolvePrize(*childState, types[i].first);
							}
					} else {
						int pick = -1;
						for (int i = 0; i < (int)edge.atomCounts.size(); ++i) if (edge.atomCounts[i] > 0) { pick = i; break; }
						if (pick < 0) { dag.walkFailed = true; return unknown(); }
						if (state.exact.pending == ExactPendingType::Draw) resolveDraw(*childState, types[pick].first);
						else resolvePrize(*childState, types[pick].first);
					}
				} catch (...) { dag.walkFailed = true; return unknown(); }
				ExactScore child = walkSkeletonNode(dag, edge.child, std::move(childState), depthLimit - 1);
				if (dag.walkFailed) return unknown();
				branches.push_back({ weight, child });
				total += weight;
				certified = certified && child.certified;
				boundsSound = boundsSound && child.boundsSound;
			}
			if (total.zero()) { dag.walkFailed = true; return unknown(); }
			for (const Branch& branch : branches) {
				lower = ExactFraction::add(lower, branch.score.lower.scaled(branch.weight, total));
				upper = ExactFraction::add(upper, branch.score.upper.scaled(branch.weight, total));
				if (!lower.valid || !upper.valid) { metrics.arithmeticOverflow = true; dag.walkFailed = true; return unknown(); }
			}
			return { lower, upper, {}, certified && boundsSound && ExactCompare(lower, upper) == 0, boundsSound };
		}

		std::vector<std::pair<std::string, ExactSmallAction>> legal;
		bool listed = forEachLegalAction(state, [&](const ExactSmallAction& action) {
			legal.push_back({ actionEquivalenceKey(state, action), action });
			return true;
		});
		if (!listed) { dag.walkFailed = true; return unknown(); }
		std::sort(legal.begin(), legal.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		std::vector<std::string> edgeLabels;
		edgeLabels.reserve(node.edges.size());
		for (const auto& edge : node.edges) edgeLabels.push_back(edge.label);
		std::sort(edgeLabels.begin(), edgeLabels.end());
		std::vector<std::string> legalLabels;
		legalLabels.reserve(legal.size());
		for (const auto& item : legal) legalLabels.push_back(item.first);
		if (legalLabels != edgeLabels) { dag.walkFailed = true; return unknown(); }

		ExactScore best;
		bool first = true;
		bool allCertified = true;
		bool allBoundsSound = true;
		const bool maximize = node.kind == ExactSkeleton::NodeKind::DecisionMax;
		for (const ExactSkeleton::DagEdge& edge : node.edges) {
			auto found = std::find_if(legal.begin(), legal.end(),
				[&](const auto& item) { return item.first == edge.label; });
			if (found == legal.end()) { dag.walkFailed = true; return unknown(); }
			auto child = cloneState(state);
			if (!advance(*child, found->second)) { dag.walkFailed = true; return unknown(); }
			ExactScore childScore = walkSkeletonNode(dag, edge.child, std::move(child), depthLimit - 1);
			if (dag.walkFailed) return unknown();
			allCertified = allCertified && childScore.certified;
			allBoundsSound = allBoundsSound && childScore.boundsSound;
			if (first || (maximize ? ExactCompare(childScore.lower, best.lower) > 0
				: ExactCompare(childScore.upper, best.upper) < 0)) {
				best = childScore;
				first = false;
			}
		}
		if (first) { dag.walkFailed = true; return unknown(); }
		best.certified = allCertified && allBoundsSound && ExactCompare(best.lower, best.upper) == 0;
		best.boundsSound = allBoundsSound;
		return best;
	}

	ExactScore solveSkeletonClass(const State& preDrawState,
		const std::vector<std::pair<int, ExactWeight>>& types,
		const std::vector<MultiDrawOutcome>& outcomes,
		const std::vector<size_t>& members,
		const ExactWeight& totalMass) {
		if (members.empty()) return unknown();
		ExactSkeleton::Dag dag;
		dag.assumedInert = collectAssumedInert(preDrawState);
		metrics.turnInertIdentities += dag.assumedInert.size();
		metrics.skeletonClassMembers += members.size();
		auto makePostDraw = [&](size_t outcomeIndex) -> ExactStatePtr {
			auto child = cloneState(preDrawState);
			const MultiDrawOutcome& outcome = outcomes[outcomeIndex];
			for (int i = 0; i < (int)outcome.atomCounts.size(); ++i)
				for (int n = 0; n < outcome.atomCounts[i]; ++n) resolveDraw(*child, types[i].first);
			return child;
		};
		ExactStatePtr representative;
		try {
			representative = makePostDraw(members.front());
		} catch (...) { return unknown(); }
		metrics.skeletonExpansions++;
		dag.root = expandSkeletonNode(std::move(representative), dag, 96);
		metrics.skeletonNodes += dag.nodes.size();
		if (dag.inertGuardFailed || dag.root < 0) {
			// Expand failed — leave members for the parallel Active-outcome path.
			metrics.skeletonGuardFallbacks++;
			metrics.skeletonGuardOther++;
			return unknown();
		}
		ExactFraction lower = ExactFraction::integer(0), upper = ExactFraction::integer(0);
		bool certified = true;
		bool boundsSound = true;
		for (size_t member : members) {
			if (expired()) return unknown();
			ExactStatePtr child;
			try { child = makePostDraw(member); }
			catch (...) { return unknown(); }
			metrics.skeletonSweeps++;
			dag.walkFailed = false;
			const bool restoreStrip = v4StripPassiveOnly;
			v4StripPassiveOnly = outcomes[member].hasPassiveExpectation;
			ExactScore score = walkSkeletonNode(dag, dag.root, std::move(child));
			v4StripPassiveOnly = restoreStrip;
			if (dag.walkFailed || !score.certified) {
				// Do not silently serial-solve the rest of the class here — that
				// starves the parallel Active-outcome path. Abandon the class.
				metrics.skeletonGuardFallbacks++;
				metrics.skeletonGuardOther++;
				return unknown();
			}
			if (outcomes[member].hasPassiveExpectation) {
				ExactFraction residual;
				residual.big = std::make_shared<ExactBigRational>(outcomes[member].expectedPassiveResidual);
				score.lower = ExactFraction::add(score.lower, residual);
				score.upper = ExactFraction::add(score.upper, residual);
				metrics.passiveResidualCalls++;
				metrics.richPassiveIntegratedWeight += outcomes[member].weight;
			}
			lower = ExactFraction::add(lower, score.lower.scaled(outcomes[member].weight, totalMass));
			upper = ExactFraction::add(upper, score.upper.scaled(outcomes[member].weight, totalMass));
			if (!lower.valid || !upper.valid) { metrics.arithmeticOverflow = true; return unknown(); }
			certified = certified && score.certified;
			boundsSound = boundsSound && score.boundsSound;
		}
		return { lower, upper, {}, certified && boundsSound && ExactCompare(lower, upper) == 0, boundsSound };
	}

	struct DrawContinuationClass {
		std::vector<std::pair<int, int>> atoms;
		int count = 0;
		int sourceClassId = -1;
		bool passiveIntegrated = false; // skip atom split only when PassiveProofV4 is complete
	};

	// Representative physical draws must not change Semantic FeatureRecord bytes.
	// Does not reclassify the whole hand: strips only basePassive + cards drawn from group.
	bool passiveSemanticInvariant(const State& state, const DrawContinuationClass& group, int take,
		const ExactCardLivenessV4::OperatorClosure& /*closure*/,
		const ExactPassivePayloadV4& basePassive) {
		if (group.atoms.size() <= 1) return true;
		if (take <= 0 || !evaluator || !evaluator->isLoaded()) return false;
		// Rich Energy can take up to 4; never silently shrink the check.
		if ((int)group.atoms.size() > 6 || take > 4) return false;
		std::vector<int> bounds;
		bounds.reserve(group.atoms.size());
		for (const auto& atom : group.atoms) bounds.push_back(atom.second);
		BoundedCompositionCursor cursor;
		cursor.reset(bounds, take);
		std::vector<int> localCounts;
		std::string canonical;
		bool haveCanonical = false;
		int outcomes = 0;
		while (cursor.next(localCounts)) {
			if (++outcomes > 256) return false; // too heavy ⇒ Active fallback
			auto child = cloneState(state);
			std::vector<std::pair<int, int>> drawn;
			try {
				for (int ai = 0; ai < (int)group.atoms.size(); ++ai) {
					if (localCounts[ai] <= 0) continue;
					drawn.push_back({ group.atoms[ai].first, localCounts[ai] });
					for (int n = 0; n < localCounts[ai]; ++n)
						resolveDraw(*child, group.atoms[ai].first);
				}
			} catch (...) { return false; }
			ExactSparseEvaluatorV3::FeatureRecord features;
			ExactSparseEvaluatorV3::extractFeaturesInto(features, *child, actor, &actorProfileCount,
				nullptr, nullptr, nullptr, &actorProfileSorted, false);
			ExactPassivePayloadV4 passive = ExactFeatureV4::MergePassiveCounts(basePassive, drawn);
			auto record = ExactFeatureV4::BuildFromV3(features, passive, nullptr);
			std::string bytes = ExactFeatureV4::SerializeSemanticFeatures(record.semantic.features);
			bytes.push_back((char)(record.overflow ? 1 : 0));
			bytes.append(reinterpret_cast<const char*>(&record.semantic.passiveHandTotal),
				sizeof(record.semantic.passiveHandTotal));
			long long semanticValue = 0;
			if (!evaluator->evaluateV4FeaturesUnclamped(record.semantic.features,
				ExactPassivePayloadV4{}, semanticValue))
				return false;
			bytes.append(reinterpret_cast<const char*>(&semanticValue), sizeof(semanticValue));
			if (!haveCanonical) { canonical = std::move(bytes); haveCanonical = true; }
			else if (bytes != canonical) return false;
		}
		return haveCanonical;
	}

	bool passiveSemanticInvariantAllTakes(const State& state, const DrawContinuationClass& group,
		int drawCount, const ExactCardLivenessV4::OperatorClosure& closure,
		const ExactPassivePayloadV4& basePassive) {
		const int maxTake = std::min(drawCount, group.count);
		for (int take = 1; take <= maxTake; ++take) {
			if (!passiveSemanticInvariant(state, group, take, closure, basePassive))
				return false;
		}
		return maxTake >= 1 || group.atoms.size() <= 1;
	}

	std::vector<DrawContinuationClass> drawContinuationClasses(const State& state,
		const std::vector<std::pair<int, ExactWeight>>& types, std::string& schema) {
		std::map<int, int> available;
		for (const auto& item : types) {
			if (!item.second.fitsUnsignedLongLong()
				|| item.second.unsignedLongLong() > (unsigned long long)DECK_SIZE) return {};
			available[item.first] = (int)item.second.unsignedLongLong();
		}
		const auto analysisStart = std::chrono::steady_clock::now();
		ExactEnsureDeferredFunctionRegistry();
		const PartitionAnalysis& analysis = turnDependencyPartition(state, nullptr, true);
		const ExactCardPartition& partition = analysis.partition;
		// P0-1/P0-2: classify with operator closure; keep Passive pools per source class.
		std::unordered_set<int> reachable;
		for (int id : analysis.visibleIds) reachable.insert(id);
		const std::uint64_t partitionHash = ExactCardLivenessV4::StableHashString(analysis.schema);
		ExactCardLivenessV4::OperatorClosure closure =
			ExactCardLivenessV4::BuildOperatorClosure(reachable, partitionHash);
		ExactCardLivenessV4::ApplyStateCoverageScanners(closure, state);
		const ExactCardLivenessV4::OperatorSourceKey currentDrawEffect =
			ExactCardLivenessV4::PendingEffectSourceKey(state);
		const ExactCardLivenessV4::PassiveMomentStateV4 moment =
			ExactCardLivenessV4::BuildPassiveMomentState(state, actor);
		// Nested-chance: remaining effects after pendingEffectIndex, plus any
		// moment-playable further-chance operator (excluding only this Draw Effect).
		const bool furtherChance = ExactCardLivenessV4::FurtherChanceUntilTurnEnd(closure, state);
		const bool anyFutureChance = ExactCardLivenessV4::AnyMomentFurtherChance(
			closure, state, moment, currentDrawEffect);
		const bool v4Loaded = evaluator && evaluator->v4().isLoaded();
		const bool analyticOk = v4Loaded; // residual clamp proof is certification-only (see analyticIntegralSafe)
		if (v4PassiveDrawEnabled && v4Loaded && !evaluator->v4().analyticIntegralSafe())
			++metrics.fallbackAnalyticBound;
		// Per-card proveCandidate gates Passive classification — not closure.complete().
		const bool allowPassiveIntegral = v4PassiveDrawEnabled && !furtherChance
			&& !anyFutureChance && analyticOk;
		if (v4PassiveDrawEnabled && (furtherChance || anyFutureChance)) {
			++metrics.nestedChancePassiveFallbacks;
			++metrics.fallbackFurtherChance;
		}
		const int drawCount = state.exact.pendingCount;
		ExactPassivePayloadV4 basePassiveForGuard;
		{
			std::unordered_map<int, int> handCounts;
			for (CardRef ref : state.players[actor].hand) {
				if (ref.isNull()) continue;
				++handCounts[state.getCard(ref).cardId];
			}
			auto split = ExactCardLivenessV4::SplitHandCounts(state, actor, handCounts, closure, moment);
			basePassiveForGuard.setCounts(std::move(split.passiveCounts), split.proofHash);
		}
		std::set<int> assigned;
		std::vector<DrawContinuationClass> result;
		int sourceClassId = 0;

		auto classifyAtom = [&](int cardId) -> ExactCardLivenessV4::CardLivenessResult {
			auto live = ExactCardLivenessV4::ClassifyCardId(state, actor, cardId, closure, moment);
			if (live.liveness == ExactCardLivenessV4::CardLiveness::Passive
				&& evaluator && evaluator->v4().isLoaded()
				&& !evaluator->v4().hasPassiveToken(cardId)) {
				live.liveness = ExactCardLivenessV4::CardLiveness::Active;
				live.reasonMask |= ExactCardLivenessV4::UnsupportedTarget;
				++metrics.livenessFallbackCount;
				++metrics.fallbackUnknownToken;
			}
			if (live.liveness != ExactCardLivenessV4::CardLiveness::Passive) {
				if (!live.coverage.actionCostSafe) ++metrics.fallbackIncompleteCosts;
				if (!closure.pendingEffectsCovered || !live.coverage.handIdentitySafe)
					++metrics.fallbackIncompletePending;
				if (!closure.globalEffectsCovered) ++metrics.fallbackIncompleteGlobal;
				if (!live.coverage.selectionSafe) ++metrics.fallbackIncompleteSelection;
				if (!live.coverage.conditionSafe) ++metrics.fallbackIncompleteConditions;
				if (!live.proof.semanticInvariant) ++metrics.fallbackSemanticInvariant;
			}
			if (live.liveness == ExactCardLivenessV4::CardLiveness::Unknown) {
				++metrics.unknownLivenessCount;
				live.liveness = ExactCardLivenessV4::CardLiveness::Active;
				++metrics.livenessFallbackCount;
			}
			return live;
		};

		for (const ExactCardClass& source : partition.classes()) {
			DrawContinuationClass target;
			target.sourceClassId = sourceClassId++;
			bool allPassive = true;
			bool deckRemovalOk = true;
			bool anyAtom = false;
			for (const ExactCardAtom& atom : source.atoms) {
				auto found = available.find(atom.cardId);
				if (found == available.end() || found->second <= 0) continue;
				anyAtom = true;
				target.atoms.push_back({ atom.cardId, found->second });
				target.count += found->second;
				assigned.insert(atom.cardId);
				if (!v4PassiveDrawEnabled) continue;
				auto live = classifyAtom(atom.cardId);
				if (live.liveness != ExactCardLivenessV4::CardLiveness::Passive
					|| !live.proof.allProven() || !live.proof.deckRemovalInvariant) {
					allPassive = false;
					deckRemovalOk = false;
					metrics.activeCardCount += (unsigned long long)found->second;
				} else {
					metrics.passiveCardCount += (unsigned long long)found->second;
				}
			}
			if (!anyAtom) continue;
			// Never merge across source classes. Integrate only when the whole class
			// proves Passive + deckRemovalInvariant.
			if (allowPassiveIntegral && allPassive && deckRemovalOk && target.count > 0) {
				target.passiveIntegrated = true;
				if (!passiveSemanticInvariantAllTakes(state, target, drawCount, closure, basePassiveForGuard)) {
					target.passiveIntegrated = false;
					++metrics.representativeInvariantFallbacks;
					++metrics.fallbackSemanticInvariant;
				} else {
					metrics.passiveCardsIntegrated += (unsigned long long)target.count;
				}
			} else {
				target.passiveIntegrated = false;
			}
			result.push_back(std::move(target));
		}
		for (const auto& item : available) if (!assigned.contains(item.first)) {
			DrawContinuationClass singleton;
			singleton.sourceClassId = sourceClassId++;
			singleton.atoms = { { item.first, item.second } };
			singleton.count = item.second;
			if (v4PassiveDrawEnabled) {
				auto live = classifyAtom(item.first);
				if (allowPassiveIntegral
					&& live.liveness == ExactCardLivenessV4::CardLiveness::Passive
					&& live.proof.allProven() && live.proof.deckRemovalInvariant) {
					singleton.passiveIntegrated = true;
					if (!passiveSemanticInvariantAllTakes(state, singleton, drawCount, closure, basePassiveForGuard)) {
						singleton.passiveIntegrated = false;
						++metrics.representativeInvariantFallbacks;
						++metrics.fallbackSemanticInvariant;
						metrics.activeCardCount += (unsigned long long)item.second;
					} else {
						metrics.passiveCardCount += (unsigned long long)item.second;
						metrics.passiveCardsIntegrated += (unsigned long long)item.second;
					}
				} else {
					metrics.activeCardCount += (unsigned long long)item.second;
				}
			}
			result.push_back(std::move(singleton));
		}
		std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
			if (left.passiveIntegrated != right.passiveIntegrated) return left.passiveIntegrated < right.passiveIntegrated;
			return left.atoms.front().first < right.atoms.front().first;
		});
		schema = v4PassiveDrawEnabled ? "CONTINUATION-DRAW-V4P2|" : "CONTINUATION-DRAW-V1|";
		for (const DrawContinuationClass& group : result) {
			appendSemantic(schema, group.count); appendSemantic(schema, (long long)group.atoms.size());
			appendSemantic(schema, group.passiveIntegrated ? 1 : 0);
			appendSemantic(schema, group.sourceClassId);
			for (const auto& atom : group.atoms) {
				appendSemantic(schema, atom.first); appendSemantic(schema, atom.second);
			}
		}
		metrics.livenessAnalysisNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - analysisStart).count();
		return result;
	}

	ExactScore multiDrawChanceStreaming(const State& state, const std::string& nodeKey,
		const std::vector<std::pair<int, ExactWeight>>& types) {
		const int drawCount = state.exact.pendingCount;
		int available = 0;
		for (const auto& item : types) {
			if (!item.second.fitsUnsignedLongLong()
				|| item.second.unsignedLongLong() > (unsigned long long)DECK_SIZE) return unknown();
			available += (int)item.second.unsignedLongLong();
		}
		if (drawCount <= 1 || drawCount > available) return unknown();
		const std::string resumeKey = nodeKey + "\x1fMULTI-DRAW-STREAM";
		auto [found, inserted] = partialMultiDraws.try_emplace(resumeKey);
		PartialMultiDrawEntry& partial = found->second;
		if (inserted || !partial.initialized) {
			partial.bounds.clear();
			for (const auto& item : types) partial.bounds.push_back((int)item.second.unsignedLongLong());
			partial.continuationSchema = "STREAMING-COUNT-V1";
			partial.totalWeight = chooseCount(available, drawCount);
			partial.cursor.reset(types, drawCount);
			if (!partial.cursor.initialized) { partialMultiDraws.erase(found); return unknown(); }
			partial.streaming = true;
			partial.initialized = true;
			updateAccountedBytes(partial.accountedBytes,
				resumeKey.size() + sizeof(PartialMultiDrawEntry) + partial.cursor.bytes());
			metrics.streamingCursorHits++;
			metrics.continuationDraws++;
			metrics.continuationDrawClasses += types.size();
		} else {
			metrics.streamingCursorResumes++;
			if (!partial.streaming || partial.bounds.size() != types.size()
				|| partial.continuationSchema != "STREAMING-COUNT-V1"
				|| partial.totalWeight != chooseCount(available, drawCount)) return unknown();
		}
		const ExactWeight& total = partial.totalWeight;
		noteWeight(total);
		updateAccountedBytes(partial.accountedBytes,
			resumeKey.size() + sizeof(PartialMultiDrawEntry) + partial.cursor.bytes()
			+ partial.pendingOutcome.atomCounts.size() * sizeof(int)
			+ partial.pendingOutcome.continuationKey.size());
		metrics.streamingCursorPeakBytes = std::max<unsigned long long>(
			metrics.streamingCursorPeakBytes, partial.accountedBytes);
		auto incomplete = [&](const ExactScore* current = nullptr) {
			ExactFraction lower = partial.completedLower, upper = partial.completedUpper;
			ExactWeight covered = partial.processedWeight;
			if (current != nullptr) {
				lower = ExactFraction::add(lower, current->lower.scaled(partial.pendingWeight, total));
				upper = ExactFraction::add(upper, current->upper.scaled(partial.pendingWeight, total));
				covered += partial.pendingWeight;
			}
			ExactWeight remaining = covered >= total ? ExactWeight() : ExactWeight::subtract(total, covered);
			lower = ExactFraction::add(lower, ExactFraction::integer(-100'000'000).scaled(remaining, total));
			upper = ExactFraction::add(upper, ExactFraction::integer(100'000'000).scaled(remaining, total));
			return ExactScore{ lower, upper, {}, false };
		};
		while (true) {
			if (!partial.pending) {
				if (!partial.cursor.next(partial.pendingOutcome)) {
					if (partial.processedWeight != total) {
						metrics.chanceMassMismatches++; metrics.probabilityExact = false;
						updateAccountedBytes(partial.accountedBytes, 0);
						partialMultiDraws.erase(found); return unknown();
					}
					ExactScore result{ partial.completedLower, partial.completedUpper, {},
						ExactCompare(partial.completedLower, partial.completedUpper) == 0 };
					updateAccountedBytes(partial.accountedBytes, 0);
					partialMultiDraws.erase(found); return result;
				}
				partial.pendingWeight = partial.pendingOutcome.weight;
				partial.pending = true;
				metrics.streamingCursorGenerated++;
				metrics.rawOutcomes++;
				metrics.continuationPreparedOutcomes++;
				updateAccountedBytes(partial.accountedBytes,
					resumeKey.size() + sizeof(PartialMultiDrawEntry) + partial.cursor.bytes()
					+ partial.pendingOutcome.atomCounts.size() * sizeof(int)
					+ partial.pendingOutcome.continuationKey.size());
				metrics.streamingCursorPeakBytes = std::max<unsigned long long>(
					metrics.streamingCursorPeakBytes, partial.accountedBytes);
			}
			if (expired()) { metrics.partialChanceNodes++; return incomplete(); }
			ExactStatePtr child = cloneState(state);
			try {
				for (int i = 0; i < (int)partial.pendingOutcome.atomCounts.size(); ++i)
					for (int n = 0; n < partial.pendingOutcome.atomCounts[i]; ++n)
						resolveDraw(*child, types[i].first);
			} catch (...) { return unknown(); }
			ExactScore score = solveOwned(std::move(child));
			if (!score.certified) { metrics.partialChanceNodes++; return incomplete(&score); }
			partial.completedLower = ExactFraction::add(partial.completedLower,
				score.lower.scaled(partial.pendingWeight, total));
			partial.completedUpper = ExactFraction::add(partial.completedUpper,
				score.upper.scaled(partial.pendingWeight, total));
			if (!partial.completedLower.valid || !partial.completedUpper.valid) {
				metrics.arithmeticOverflow = true; return unknown();
			}
			partial.processedWeight += partial.pendingWeight;
			metrics.enumeratedHiddenWorlds++;
			metrics.continuationDrawOutcomes++;
			partial.pendingWeight = ExactWeight();
			partial.pendingOutcome = {};
			partial.pending = false;
			if (partial.processedWeight == total) {
				ExactScore result{ partial.completedLower, partial.completedUpper, {},
					ExactCompare(partial.completedLower, partial.completedUpper) == 0 };
				updateAccountedBytes(partial.accountedBytes, 0);
				partialMultiDraws.erase(found); return result;
			}
		}
	}

	ExactScore multiDrawChance(const State& state, const std::string& nodeKey) {
		auto types = chanceCardTypes(state);
		const int drawCount = state.exact.pendingCount;
		if (types.empty() || drawCount <= 1) return unknown();
		// The count-vector upper bound is cheap and conservative.  Large spaces
		// use the resumable cursor path before continuation classes can materialize
		// a product of allocations.
		ExactWeight compositionUpper = chooseCount((int)types.size() + drawCount - 1, drawCount);
		if (!compositionUpper.fitsUnsignedLongLong()
			|| compositionUpper.unsignedLongLong() > 1024ULL)
			return multiDrawChanceStreaming(state, nodeKey, types);
		std::string continuationSchema;
		std::vector<DrawContinuationClass> classes;
		if (state.exact.pendingPlayer == actor) {
			classes = drawContinuationClasses(state, types, continuationSchema);
			// P0-4: freeze Passive hand counts before draw outcomes (context-free residual).
			chanceNodeBasePassive.clear();
			std::unordered_map<int, int> handCounts;
			for (CardRef ref : state.players[actor].hand) {
				if (ref.isNull()) continue;
				++handCounts[state.getCard(ref).cardId];
			}
			std::unordered_set<int> reachable;
			const PartitionAnalysis& baseAnalysis = turnDependencyPartition(state, nullptr, true);
			for (int id : baseAnalysis.visibleIds) reachable.insert(id);
			auto closure = ExactCardLivenessV4::BuildOperatorClosure(reachable,
				ExactCardLivenessV4::StableHashString(baseAnalysis.schema));
			ExactEnsureDeferredFunctionRegistry();
			ExactCardLivenessV4::ApplyStateCoverageScanners(closure, state);
			const ExactCardLivenessV4::PassiveMomentStateV4 multiDrawMoment =
				ExactCardLivenessV4::BuildPassiveMomentState(state, actor);
			auto split = ExactCardLivenessV4::SplitHandCounts(state, actor, handCounts, closure, multiDrawMoment);
			// Only keep basePassive when this chance itself may analytic-integrate.
			// Otherwise nested chances would double-count base in E[R].
			bool anyIntegrated = false;
			for (const auto& group : classes) if (group.passiveIntegrated) { anyIntegrated = true; break; }
			if (anyIntegrated)
				chanceNodeBasePassive.setCounts(std::move(split.passiveCounts), split.proofHash);
			else
				chanceNodeBasePassive.clear();
		} else {
			chanceNodeBasePassive.clear();
			continuationSchema = "CONTINUATION-DRAW-SINGLETON|";
			for (const auto& item : types) {
				if (!item.second.fitsUnsignedLongLong()) return unknown();
				int count = (int)item.second.unsignedLongLong();
				classes.push_back({ { { item.first, count } }, count });
				appendSemantic(continuationSchema, item.first); appendSemantic(continuationSchema, count);
			}
		}
		if (classes.empty()) return unknown();
		std::vector<int> bounds; bounds.reserve(types.size());
		int available = 0;
		for (const auto& type : types) {
			if (!type.second.fitsUnsignedLongLong()
				|| type.second.unsignedLongLong() > (unsigned long long)DECK_SIZE) return unknown();
			bounds.push_back((int)type.second.unsignedLongLong()); available += bounds.back();
		}
		if (drawCount > available) return unknown();
		std::string resumeKey = nodeKey + "\x1fMULTI-DRAW";
		auto [found, inserted] = partialMultiDraws.try_emplace(resumeKey);
		PartialMultiDrawEntry& partial = found->second;
		if (inserted || !partial.initialized) {
			partial.bounds = bounds;
			partial.continuationSchema = continuationSchema;
			partial.totalWeight = chooseCount(available, drawCount);
			std::map<int, int> typeIndex;
			for (int i = 0; i < (int)types.size(); ++i) typeIndex[types[i].first] = i;
			std::vector<int> classBounds; classBounds.reserve(classes.size());
			for (const DrawContinuationClass& group : classes) classBounds.push_back(group.count);
			BoundedCompositionCursor classCursor; classCursor.reset(classBounds, drawCount);
			std::vector<int> classCounts;
			std::unordered_map<std::string, size_t, ExactStringHasher> byContinuation;
			ExactWeight generated;
			unsigned long long rawDrawOutcomes = 0;
			std::vector<std::pair<int, long long>> cachedPassiveValues;
			if (evaluator && evaluator->v4().isLoaded() && evaluator->v4().passiveEnabled())
				cachedPassiveValues = evaluator->v4().passiveValueTableContextFree();
			// The outer enumeration axis is the continuation class, not card ID.
			// Only after observing a class-count vector do we conditionally split a
			// class into atoms. This preserves exact hypergeometric mass while
			// avoiding identity enumeration for classes which need no refinement.
			struct ConditionalAllocation {
				std::vector<std::pair<int, int>> counts;
				ExactWeight weight;
				std::string symmetricKey;
			};
			while (classCursor.next(classCounts)) {
				metrics.continuationClassOutcomes++;
				std::vector<std::vector<ConditionalAllocation>> allocations(classes.size());
				ExactWeight outerWeight(1);
				bool valid = true;
				for (int groupIndex = 0; groupIndex < (int)classes.size(); ++groupIndex) {
					const DrawContinuationClass& group = classes[groupIndex];
					const int take = classCounts[groupIndex];
					outerWeight = ExactWeight::multiply(outerWeight, chooseCount(group.count, take));
					if (group.passiveIntegrated) {
						// Phase 4: Passive pool contributes hypergeometric mass without
						// identity enumeration. Canonical physical draw preserves deck
						// size/hand count; residual value is added once V4 weights load.
						ConditionalAllocation allocation;
						allocation.weight = chooseCount(group.count, take);
						int remaining = take;
						for (const auto& atom : group.atoms) {
							const int use = std::min(remaining, atom.second);
							if (use <= 0) continue;
							allocation.counts.push_back({ typeIndex.at(atom.first), use });
							remaining -= use;
						}
						appendSemantic(allocation.symmetricKey, (long long)0);
						appendSemantic(allocation.symmetricKey, take);
						appendSemantic(allocation.symmetricKey, 1); // passive marker
						allocations[groupIndex].push_back(std::move(allocation));
						metrics.passiveExpectationCalls++;
						continue;
					}
					std::vector<int> atomBounds; atomBounds.reserve(group.atoms.size());
					for (const auto& atom : group.atoms) atomBounds.push_back(atom.second);
					BoundedCompositionCursor atomCursor; atomCursor.reset(atomBounds, take);
					std::vector<int> localCounts;
					ExactWeight conditionalMass;
					while (atomCursor.next(localCounts)) {
						ConditionalAllocation allocation;
						allocation.weight = ExactWeight(1);
						std::vector<std::pair<int, int>> symmetric;
						for (int atomIndex = 0; atomIndex < (int)group.atoms.size(); ++atomIndex) {
							const auto& atom = group.atoms[atomIndex];
							allocation.weight = ExactWeight::multiply(allocation.weight,
								chooseCount(atom.second, localCounts[atomIndex]));
							if (localCounts[atomIndex] > 0)
								allocation.counts.push_back({ typeIndex.at(atom.first), localCounts[atomIndex] });
							symmetric.push_back({ atom.second, localCounts[atomIndex] });
						}
						std::sort(symmetric.begin(), symmetric.end());
						appendSemantic(allocation.symmetricKey, (long long)symmetric.size());
						for (const auto& pair : symmetric) {
							appendSemantic(allocation.symmetricKey, pair.first);
							appendSemantic(allocation.symmetricKey, pair.second);
						}
						conditionalMass += allocation.weight;
						allocations[groupIndex].push_back(std::move(allocation));
					}
					if (conditionalMass != chooseCount(group.count, take)) { valid = false; break; }
					if (allocations[groupIndex].size() > 1)
						metrics.continuationConditionalSplits += allocations[groupIndex].size() - 1;
				}
				if (!valid) {
					metrics.chanceMassMismatches++; metrics.probabilityExact = false;
					partialMultiDraws.erase(found); return unknown();
				}
				ExactWeight generatedForClassVector;
				std::vector<int> atomCounts(bounds.size(), 0);
				std::vector<ExactPassiveExpectationV4::PassiveDrawPool> passivePools;
				bool anyPassiveTake = false;
				for (int gi = 0; gi < (int)classes.size(); ++gi) if (classes[gi].passiveIntegrated) {
					ExactPassiveExpectationV4::PassiveDrawPool pool;
					pool.copies = classes[gi].atoms;
					pool.take = classCounts[gi];
					if (pool.take > 0) anyPassiveTake = true;
					passivePools.push_back(std::move(pool));
				}
				std::function<void(int, const ExactWeight&, std::string)> combine;
				combine = [&](int groupIndex, const ExactWeight& weight, std::string key) {
					if (groupIndex == (int)allocations.size()) {
						MultiDrawOutcome outcome;
						outcome.atomCounts = atomCounts;
						outcome.weight = weight;
						outcome.continuationKey = std::move(key);
						if (anyPassiveTake
							&& evaluator && evaluator->v4().isLoaded()
							&& evaluator->v4().passiveEnabled()) {
							outcome.expectedPassiveResidual =
								ExactPassiveExpectationV4::ExpectedPassiveResidual(
									chanceNodeBasePassive.counts, passivePools,
									cachedPassiveValues, evaluator->v4().pairs());
							outcome.hasPassiveExpectation = true;
							metrics.passiveExpectationCalls++;
						}
						auto [position, created] = byContinuation.emplace(outcome.continuationKey, partial.outcomes.size());
						if (created) partial.outcomes.push_back(std::move(outcome));
						else partial.outcomes[position->second].weight += weight;
						generated += weight; generatedForClassVector += weight;
						metrics.rawOutcomes++; rawDrawOutcomes++;
						return;
					}
					for (const ConditionalAllocation& allocation : allocations[groupIndex]) {
						for (const auto& item : allocation.counts) atomCounts[item.first] = item.second;
						combine(groupIndex + 1, ExactWeight::multiply(weight, allocation.weight),
							key + allocation.symmetricKey);
						for (const auto& item : allocation.counts) atomCounts[item.first] = 0;
					}
				};
				combine(0, ExactWeight(1), {});
				if (generatedForClassVector != outerWeight) {
					metrics.chanceMassMismatches++; metrics.probabilityExact = false;
					partialMultiDraws.erase(found); return unknown();
				}
			}
			if (generated != partial.totalWeight) {
				metrics.chanceMassMismatches++; metrics.probabilityExact = false;
				partialMultiDraws.erase(found); return unknown();
			}
			std::sort(partial.outcomes.begin(), partial.outcomes.end(), [](const auto& left, const auto& right) {
				return left.continuationKey < right.continuationKey;
			});
			partial.outcomeDone.assign(partial.outcomes.size(), 0);
			partial.skeletonAttempted = false;
			metrics.continuationPreparedOutcomes += partial.outcomes.size();
			if (v4PassiveDrawEnabled) {
				metrics.richActiveOutcomeCount += partial.outcomes.size();
				metrics.richTotalChanceWeight += partial.totalWeight;
			}
			metrics.groupedOutcomes += rawDrawOutcomes >= partial.outcomes.size()
				? rawDrawOutcomes - partial.outcomes.size() : 0;
			partial.initialized = true;
			partial.accountedBytes = resumeKey.size() + sizeof(PartialMultiDrawEntry)
				+ bounds.size() * sizeof(int) * 8;
			for (const auto& outcome : partial.outcomes)
				partial.accountedBytes += sizeof(MultiDrawOutcome)
					+ outcome.atomCounts.size() * sizeof(int) + outcome.continuationKey.size();
			partialBytes += partial.accountedBytes;
			metrics.continuationDraws++;
			metrics.continuationDrawClasses += classes.size();
			for (const auto& group : classes) if (group.atoms.size() > 1)
				metrics.continuationAtomsMerged += group.atoms.size() - 1;
		} else {
			metrics.partialChanceHits++;
			if (partial.bounds != bounds || partial.continuationSchema != continuationSchema
				|| partial.totalWeight != chooseCount(available, drawCount)) return unknown();
		}
		const ExactWeight& total = partial.totalWeight;
		noteWeight(total);
		auto incomplete = [&](const ExactScore* current = nullptr) {
			ExactFraction lower = partial.completedLower, upper = partial.completedUpper;
			ExactWeight covered = partial.processedWeight;
			if (current != nullptr) {
				lower = ExactFraction::add(lower, current->lower.scaled(partial.pendingWeight, total));
				upper = ExactFraction::add(upper, current->upper.scaled(partial.pendingWeight, total));
				covered += partial.pendingWeight;
			}
			ExactWeight remaining = covered >= total ? ExactWeight() : ExactWeight::subtract(total, covered);
			lower = ExactFraction::add(lower, ExactFraction::integer(-100'000'000).scaled(remaining, total));
			upper = ExactFraction::add(upper, ExactFraction::integer(100'000'000).scaled(remaining, total));
			return ExactScore{ lower, upper, {}, false };
		};
		// A combination can be much larger than the normal 20k-node root
		// scheduling quantum.  Preserve the wall-clock/RSS deadline while allowing
		// one exact multiset outcome to finish and become reusable in the TT.
		struct QuantumGuard { unsigned long long& limit; unsigned long long previous;
			QuantumGuard(unsigned long long& value, unsigned long long expanded)
				: limit(value), previous(value) {
				if (value != std::numeric_limits<unsigned long long>::max())
					value = std::max(value, expanded + 500'000ULL);
			}
			~QuantumGuard() { limit = previous; }
		} quantum(nodeQuantumDeadline, metrics.expanded);

		// Phase 0 diagnostics: count turn-inert identities at the draw root.
		{
			auto inert = collectAssumedInert(state);
			metrics.turnInertIdentities = std::max(metrics.turnInertIdentities, (unsigned long long)inert.size());
		}

		// Skeleton isomorphism classes: expand once per active multiset, sweep members.
		// Commit each finished class into partial.*; never wipe progress on incomplete.
		const bool useSkeleton = skeletonSharingEnabled
			&& state.exact.pendingPlayer == actor
			&& !partial.pending
			&& !partial.skeletonAttempted
			&& partial.processedWeight.zero()
			// Item-heavy fanouts: prefer parallel Active solves over many failing expands.
			&& partial.outcomes.size() <= 512;
		if (useSkeleton && partial.outcomes.size() > 1) {
			partial.skeletonAttempted = true;
			if (partial.outcomeDone.size() != partial.outcomes.size())
				partial.outcomeDone.assign(partial.outcomes.size(), 0);
			std::vector<std::vector<int>> atomLists;
			atomLists.reserve(partial.outcomes.size());
			for (const MultiDrawOutcome& outcome : partial.outcomes)
				atomLists.push_back(outcome.atomCounts);
			auto classes = ExactSkeleton::ClassOutcomes(state, actor, types, atomLists);
			metrics.skeletonClassCount += classes.size();
			metrics.skeletonClasses += classes.size();
			std::stable_sort(classes.begin(), classes.end(), [](const ExactSkeleton::OutcomeClass& a,
				const ExactSkeleton::OutcomeClass& b) {
				return a.memberIndices.size() > b.memberIndices.size();
			});
			int skeletonAttempts = 0;
			int consecutiveFailures = 0;
			static constexpr int MaxSkeletonAttempts = 48;
			static constexpr int MaxConsecutiveSkeletonFailures = 6;
			for (const ExactSkeleton::OutcomeClass& group : classes) {
				if (expired()) break;
				if (group.memberIndices.size() < 2) continue;
				if (skeletonAttempts >= MaxSkeletonAttempts) break;
				if (consecutiveFailures >= MaxConsecutiveSkeletonFailures) {
					metrics.skeletonGuardIncomplete++;
					break;
				}
				++skeletonAttempts;
				ExactScore score = solveSkeletonClass(state, types, partial.outcomes, group.memberIndices, total);
				if (!score.certified) {
					metrics.skeletonGuardFallbacks++;
					metrics.skeletonGuardOther++;
					++consecutiveFailures;
					continue; // leave members for legacy / parallel loop
				}
				consecutiveFailures = 0;
				partial.completedLower = ExactFraction::add(partial.completedLower, score.lower);
				partial.completedUpper = ExactFraction::add(partial.completedUpper, score.upper);
				if (!partial.completedLower.valid || !partial.completedUpper.valid) {
					metrics.arithmeticOverflow = true; return unknown();
				}
				for (size_t member : group.memberIndices) {
					if (member >= partial.outcomeDone.size() || partial.outcomeDone[member]) continue;
					partial.outcomeDone[member] = 1;
					partial.processedWeight += partial.outcomes[member].weight;
					metrics.enumeratedHiddenWorlds++;
					metrics.continuationDrawOutcomes++;
				}
			}
			if (partial.processedWeight == total) {
				ExactScore result{ partial.completedLower, partial.completedUpper, {},
					ExactCompare(partial.completedLower, partial.completedUpper) == 0 };
				partialBytes -= std::min(partialBytes, partial.accountedBytes);
				partialMultiDraws.erase(resumeKey);
				return result;
			}
			// Resume legacy from the first unfinished outcome.
			partial.outcomeIndex = 0;
			while (partial.outcomeIndex < partial.outcomes.size()
				&& partial.outcomeIndex < partial.outcomeDone.size()
				&& partial.outcomeDone[partial.outcomeIndex])
				++partial.outcomeIndex;
			partial.pending = false;
		}

		// Parallel Active-outcome solve: shared TT, worker-local planners.
		if (!partial.pending && !actorDeckSnapshot.empty()
			&& partial.outcomeIndex < partial.outcomes.size()) {
			size_t remaining = 0;
			for (size_t i = partial.outcomeIndex; i < partial.outcomes.size(); ++i) {
				if (partial.outcomeDone.empty() || i >= partial.outcomeDone.size()
					|| !partial.outcomeDone[i])
					++remaining;
			}
				unsigned requestedWorkers = std::thread::hardware_concurrency();
				if (requestedWorkers == 0) requestedWorkers = 2;
				requestedWorkers = std::min(std::max(2u, requestedWorkers / 4u), 4u);
				std::vector<ExactThreadBudget::Lease> workerLeases;
				if (chanceParallelEnabled) {
					workerLeases.reserve(requestedWorkers);
					for (unsigned w = 0; w < requestedWorkers; ++w) {
						auto lease = ExactGlobalThreadBudget().tryLease();
						if (!lease) break;
						workerLeases.push_back(std::move(lease));
					}
				}
				const unsigned workers = (unsigned)workerLeases.size();
				if (remaining >= workers && workers > 0u) {
					std::vector<size_t> todo;
					todo.reserve(remaining);
					for (size_t i = 0; i < partial.outcomes.size(); ++i) {
						if (!partial.outcomeDone.empty() && i < partial.outcomeDone.size()
							&& partial.outcomeDone[i])
							continue;
						todo.push_back(i);
					}
					std::atomic<size_t> cursor{ 0 };
					std::mutex mergeMutex;
					const auto sharedDeadline = deadline;
					const int sharedActor = actor;
					bool v4Passive = v4PassiveDrawEnabled;
					// Prefer private TTs: shared in-flight claims serialize Item-heavy DAGs.
					auto sharedEval = evaluator;
					struct WorkerOutcome {
						size_t index = 0;
						ExactScore score;
						unsigned long long solveNs = 0;
						bool ok = false;
					};
					auto workerBody = [&]() {
						std::vector<WorkerOutcome> local;
						ExactPlanner worker(actorDeckSnapshot.data(),
							actorHandValuesSnapshot.empty() ? nullptr : actorHandValuesSnapshot.data(),
							(int)actorDeckSnapshot.size(), 1,
							opponentDeckSnapshot.empty() ? nullptr : opponentDeckSnapshot.data(),
							(int)opponentDeckSnapshot.size(),
							nullptr, sharedEval);
						worker.setAbsoluteDeadline(sharedDeadline);
						worker.v4PassiveDrawEnabled = v4Passive;
						worker.v4PassiveDrawCertified = v4PassiveDrawCertified;
					worker.chanceParallelEnabled = false;
					worker.threadPermitHeld = true;
						worker.skeletonSharingEnabled = false;
						worker.actor = sharedActor;
						Game scratch;
						State boot = state;
						prepareExactRoot(boot, scratch);
						// Keep the parent's exact/pending draw context; initializeHidden would wipe it.
						boot.game->config.manualCoin = true;
						boot.exact.enabled = true;
						boot.exact.actor = (signed char)sharedActor;
						while (true) {
							if (worker.expired()) break;
							const size_t pos = cursor.fetch_add(1);
							if (pos >= todo.size()) break;
							const size_t outcomeIndex = todo[pos];
							const MultiDrawOutcome& outcome = partial.outcomes[outcomeIndex];
							WorkerOutcome item;
							item.index = outcomeIndex;
							try {
								auto child = worker.cloneState(boot);
								for (int i = 0; i < (int)outcome.atomCounts.size(); ++i)
									for (int n = 0; n < outcome.atomCounts[i]; ++n)
										worker.resolveDraw(*child, types[i].first);
								const bool restoreStrip = worker.v4StripPassiveOnly;
								worker.v4StripPassiveOnly = outcome.hasPassiveExpectation;
								const auto started = std::chrono::steady_clock::now();
								ExactScore score = worker.solveOwned(std::move(child));
								item.solveNs = (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
									std::chrono::steady_clock::now() - started).count();
								worker.v4StripPassiveOnly = restoreStrip;
								if (outcome.hasPassiveExpectation) {
									ExactFraction residual;
									residual.big = std::make_shared<ExactBigRational>(outcome.expectedPassiveResidual);
									score.lower = ExactFraction::add(score.lower, residual);
									score.upper = ExactFraction::add(score.upper, residual);
								}
								item.score = score;
								item.ok = score.certified;
							} catch (...) {
								item.ok = false;
							}
							local.push_back(std::move(item));
						}
						std::lock_guard<std::mutex> lock(mergeMutex);
						for (WorkerOutcome& item : local) {
							if (!item.ok) continue;
							if (item.index < partial.outcomeDone.size() && partial.outcomeDone[item.index])
								continue;
							partial.completedLower = ExactFraction::add(partial.completedLower,
								item.score.lower.scaled(partial.outcomes[item.index].weight, total));
							partial.completedUpper = ExactFraction::add(partial.completedUpper,
								item.score.upper.scaled(partial.outcomes[item.index].weight, total));
							partial.processedWeight += partial.outcomes[item.index].weight;
							if (item.index < partial.outcomeDone.size())
								partial.outcomeDone[item.index] = 1;
							metrics.activeOutcomeSolveNs += item.solveNs;
							metrics.activeOutcomeSolveCount++;
							metrics.enumeratedHiddenWorlds++;
							metrics.continuationDrawOutcomes++;
							if (partial.outcomes[item.index].hasPassiveExpectation) {
								metrics.passiveResidualCalls++;
								metrics.richPassiveIntegratedWeight += partial.outcomes[item.index].weight;
							}
						}
						metrics.expanded += worker.metrics.expanded;
						metrics.merged += worker.metrics.merged;
						metrics.leaves += worker.metrics.leaves;
						return;
					};
				std::vector<std::future<void>> futures;
				futures.reserve(workers);
				for (unsigned w = 0; w < workers; ++w)
					futures.push_back(std::async(std::launch::async, workerBody));
				for (auto& future : futures) future.get();
				if (!partial.completedLower.valid || !partial.completedUpper.valid) {
					metrics.arithmeticOverflow = true; return unknown();
				}
				if (partial.processedWeight == total) {
					ExactScore result{ partial.completedLower, partial.completedUpper, {},
						ExactCompare(partial.completedLower, partial.completedUpper) == 0 };
					partialBytes -= std::min(partialBytes, partial.accountedBytes);
					partialMultiDraws.erase(resumeKey);
					return result;
				}
				partial.outcomeIndex = 0;
				while (partial.outcomeIndex < partial.outcomes.size()
					&& partial.outcomeIndex < partial.outcomeDone.size()
					&& partial.outcomeDone[partial.outcomeIndex])
					++partial.outcomeIndex;
				partial.pending = false;
				if (expired()) {
					metrics.partialChanceNodes++;
					return incomplete();
				}
			}
		}

		while (partial.outcomeIndex < partial.outcomes.size()) {
			if (!partial.outcomeDone.empty() && partial.outcomeIndex < partial.outcomeDone.size()
				&& partial.outcomeDone[partial.outcomeIndex]) {
				++partial.outcomeIndex;
				continue;
			}
			const MultiDrawOutcome& outcome = partial.outcomes[partial.outcomeIndex];
			if (!partial.pending) {
				partial.pendingWeight = outcome.weight;
				partial.pendingExpandedNodes = 0;
				partial.pending = true;
			}
			if (expired()) {
				metrics.partialChanceNodes++;
				return incomplete();
			}
			auto child = cloneState(state);
			try {
				for (int i = 0; i < (int)outcome.atomCounts.size(); ++i)
					for (int n = 0; n < outcome.atomCounts[i]; ++n) resolveDraw(*child, types[i].first);
			} catch (...) { return unknown(); }
			const unsigned long long expandedBeforeOutcomeSlice = metrics.expanded;
			const bool restoreStrip = v4StripPassiveOnly;
			v4StripPassiveOnly = outcome.hasPassiveExpectation;
			const auto solveStarted = std::chrono::steady_clock::now();
			ExactScore score = solveOwned(std::move(child));
			metrics.activeOutcomeSolveNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - solveStarted).count();
			metrics.activeOutcomeSolveCount++;
			v4StripPassiveOnly = restoreStrip;
			if (outcome.hasPassiveExpectation) {
				const auto residualStarted = std::chrono::steady_clock::now();
				ExactFraction residual;
				residual.big = std::make_shared<ExactBigRational>(outcome.expectedPassiveResidual);
				score.lower = ExactFraction::add(score.lower, residual);
				score.upper = ExactFraction::add(score.upper, residual);
				metrics.passiveResidualCalls++;
				metrics.passiveResidualElapsedNs += (unsigned long long)
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - residualStarted).count();
				metrics.richPassiveIntegratedWeight += outcome.weight;
			}
			if (metrics.expanded >= expandedBeforeOutcomeSlice)
				partial.pendingExpandedNodes += metrics.expanded - expandedBeforeOutcomeSlice;
			if (!score.certified) { metrics.partialChanceNodes++; return incomplete(&score); }
			partial.completedLower = ExactFraction::add(partial.completedLower,
				score.lower.scaled(partial.pendingWeight, total));
			partial.completedUpper = ExactFraction::add(partial.completedUpper,
				score.upper.scaled(partial.pendingWeight, total));
			if (!partial.completedLower.valid || !partial.completedUpper.valid) {
				metrics.arithmeticOverflow = true; return unknown();
			}
			partial.processedWeight += partial.pendingWeight; noteWeight(partial.processedWeight);
			const unsigned long long outcomeNodes = partial.pendingExpandedNodes;
			metrics.continuationCompletedOutcomeNodes += outcomeNodes;
			metrics.continuationMaxOutcomeNodes = std::max(metrics.continuationMaxOutcomeNodes, outcomeNodes);
			metrics.enumeratedHiddenWorlds++; metrics.continuationDrawOutcomes++;
			if (partial.outcomeIndex < partial.outcomeDone.size())
				partial.outcomeDone[partial.outcomeIndex] = 1;
			partial.outcomeIndex++;
			partial.pendingWeight = ExactWeight(); partial.pending = false; partial.pendingExpandedNodes = 0;
		}
		if (partial.processedWeight != total) {
			metrics.chanceMassMismatches++; metrics.probabilityExact = false; return unknown();
		}
		ExactScore result{ partial.completedLower, partial.completedUpper, {},
			ExactCompare(partial.completedLower, partial.completedUpper) == 0, true };
		partialBytes -= std::min(partialBytes, partial.accountedBytes);
		partialMultiDraws.erase(resumeKey);
		return result;
	}

	ExactScore chance(const State& state, const std::string& nodeKey) {
		if (state.exact.pending == ExactPendingType::Draw && state.exact.pendingCount > 1
			&& (state.exact.deckUnknown[state.exact.pendingPlayer]
				|| state.exact.deckExchangeable[state.exact.pendingPlayer]))
			return multiDrawChance(state, nodeKey);
		auto types = chanceCardTypes(state);
		if (types.empty()) return unknown();
		const bool continuationKey = nodeKey.size() >= 4 && std::memcmp(nodeKey.data(), "PDC2", 4) == 0;
		PartialChanceEntry* partial = (!nodeKey.empty()
			&& (!singletonRevealStreaming || continuationKey
				|| (canonicalMainEnabled && concreteWorldCaching)))
			? partialChanceFor(nodeKey) : nullptr;
		ExactWeight total; for (const auto& item : types) total += item.second;
		noteWeight(total);
		ExactFraction lower = ExactFraction::integer(0), upper = ExactFraction::integer(0);
		bool certified = true;
		bool boundsSound = true;
		ExactWeight processed;
		for (const auto& item : types) {
			int id = item.first; const ExactWeight& weight = item.second;
			if (expired()) {
				metrics.partialChanceNodes++;
				ExactWeight remaining = ExactWeight::subtract(total, processed);
				lower = ExactFraction::add(lower, ExactFraction::integer(-100'000'000).scaled(remaining, total));
				upper = ExactFraction::add(upper, ExactFraction::integer(100'000'000).scaled(remaining, total));
				return { lower, upper, {}, false };
			}
			ExactScore score;
			auto saved = partial == nullptr ? nullptr : [&]() -> ExactScore* {
				auto found = partial->completedOutcomes.find(id);
				return found == partial->completedOutcomes.end() ? nullptr : &found->second;
			}();
			if (saved != nullptr) {
				score = *saved;
				if (weight.fitsUnsignedLongLong()
					&& weight.unsignedLongLong() <= std::numeric_limits<unsigned long long>::max() - metrics.resumedChanceMass)
					metrics.resumedChanceMass += weight.unsignedLongLong();
				else metrics.resumedChanceMass = std::numeric_limits<unsigned long long>::max();
			} else {
				auto child = cloneState(state);
				try {
					if (state.exact.pending == ExactPendingType::Draw) resolveDraw(*child, id); else resolvePrize(*child, id);
				} catch (...) { return unknown(); }
				score = solveOwned(std::move(child));
				if (partial != nullptr && score.certified) partial->completedOutcomes.emplace(id, score);
			}
			auto l = score.lower.scaled(weight, total), u = score.upper.scaled(weight, total);
			lower = ExactFraction::add(lower, l); upper = ExactFraction::add(upper, u);
			processed += weight;
			if (!lower.valid || !upper.valid) { metrics.arithmeticOverflow = true; return unknown(); }
			certified = certified && score.certified;
			boundsSound = boundsSound && score.boundsSound;
		}
		if (processed != total) { metrics.chanceMassMismatches++; metrics.probabilityExact = false; return unknown(); }
		return { lower, upper, {}, certified && boundsSound && ExactCompare(lower, upper) == 0, boundsSound };
	}

	ExactScore coinChance(const State& state, const std::string& nodeKey) {
		(void)nodeKey;
		struct CoinOutcome { ExactStatePtr state; ExactWeight weight; };
		std::vector<CoinOutcome> outcomes;
		std::unordered_map<std::string, size_t, ExactStringHasher> bySuccessor;
		for (int option = 0; option < 2; ++option) {
			if (expired()) { metrics.partialChanceNodes++; return unknown(); }
				auto child = cloneState(state);
			if (!advance(*child, { option })) return unknown();
			std::string key = keyFor(*child);
			auto [found, inserted] = bySuccessor.emplace(std::move(key), outcomes.size());
			if (inserted) outcomes.push_back({ std::move(child), ExactWeight(1) });
			else { outcomes[found->second].weight += ExactWeight(1); metrics.distributionMerges++; }
		}
		ExactFraction lower = ExactFraction::integer(0), upper = ExactFraction::integer(0);
		bool certified = true;
		bool boundsSound = true;
		for (CoinOutcome& outcome : outcomes) {
			if (expired()) { metrics.partialChanceNodes++; return unknown(); }
			ExactScore score = solveOwned(std::move(outcome.state));
			lower = ExactFraction::add(lower, score.lower.scaled(outcome.weight, ExactWeight(2)));
			upper = ExactFraction::add(upper, score.upper.scaled(outcome.weight, ExactWeight(2)));
			if (!lower.valid || !upper.valid) { metrics.arithmeticOverflow = true; return unknown(); }
			certified = certified && score.certified;
			boundsSound = boundsSound && score.boundsSound;
		}
		return { lower, upper, {}, certified && boundsSound && ExactCompare(lower, upper) == 0, boundsSound };
	}

	ExactScore decision(const State& state, bool maximize, const std::string& nodeKey) {
		unsigned long long expandedBefore = metrics.expanded;
		ExactScore result;
		bool first = true;
		bool selectedActionValueCertified = false;
		ExactFraction aggregate = maximize ? ExactFraction::integer(-100'000'000) : ExactFraction::integer(100'000'000);
		bool allCertified = true;
		bool allBoundsSound = true;
		struct SuccessorScoreEntry {
			std::string key;
			ExactScore score;
			unsigned long long count = 1;
		};
		std::vector<SuccessorScoreEntry> successorScores;
		successorScores.reserve(std::min<size_t>(32, std::max<size_t>(1, state.options.size())));
		PartialDecisionEntry* partial = (!nodeKey.empty()
			&& (!singletonRevealStreaming || (canonicalMainEnabled && concreteWorldCaching)))
			? partialDecisionFor(nodeKey) : nullptr;
		size_t actionOrdinal = 0;
		bool completed = forEachLegalAction(state, [&](const ExactSmallAction& action) {
			metrics.rawOutcomes++;
			// forEachLegalAction already groups physical options by this exact
			// semantic key. Build it again only when a persistent partial/transition
			// table needs a stable identifier; the Rich streaming path needs neither.
			const bool needActionKey = partial != nullptr || (!singletonRevealStreaming && !nodeKey.empty());
			std::string actionKey;
			if (needActionKey) actionKey = actionEquivalenceKey(state, action);
		const size_t thisOrdinal = actionOrdinal++;
			ExactScore* savedAction = nullptr;
			if (partial != nullptr) {
				auto found = partial->actionBounds.find(actionKey);
				if (found != partial->actionBounds.end()) savedAction = &found->second;
			}
			if (savedAction != nullptr && (savedAction->certified || thisOrdinal < partial->resumeOrdinal)) {
				ExactScore score = *savedAction;
				if (!score.certified) metrics.resumedActionCount++;
				if (maximize) { if (ExactCompare(score.upper, aggregate) > 0) aggregate = score.upper; }
				else { if (ExactCompare(score.lower, aggregate) < 0) aggregate = score.lower; }
				allCertified = allCertified && score.certified;
				allBoundsSound = allBoundsSound && score.boundsSound;
				if (first || (maximize ? ExactCompare(score.lower, result.lower) > 0 : ExactCompare(score.upper, result.upper) < 0)) {
					result = score; result.action = action;
					selectedActionValueCertified = score.certified;
					first = false;
				}
				if (recursionDepth == 1)
					rootActionValues.push_back({ action, score.lower, score.upper,
						score.certified && score.boundsSound });
				return true;
			}
			std::string transitionKey;
			// Streaming hidden-world searches almost never revisit the exact same
			// (node, action) pair before StateId interning. Building and hashing the
			// composite string at every edge was pure overhead on Rich Energy (zero
			// hits in the 30-second gate). Keep the cache for ordinary canonical DAGs.
			const bool useTransitionCache = !singletonRevealStreaming && !nodeKey.empty();
			if (useTransitionCache) {
				transitionKey.reserve(nodeKey.size() + actionKey.size() + 16);
				transitionKey.append(nodeKey);
				transitionKey.push_back('\0');
				transitionKey.append(std::to_string(actionKey.size()));
				transitionKey.push_back(':');
				transitionKey.append(actionKey);
			}
			ExactScore score;
			bool haveScore = false;
			if (!transitionKey.empty()) {
				auto cachedTransition = transitionScoreCache.find(transitionKey);
				if (cachedTransition != transitionScoreCache.end()) {
					score = cachedTransition->second;
					score.action = action;
					metrics.transitionCacheHits++;
					haveScore = true;
				}
			}
			if (!haveScore) {
				auto child = cloneState(state);
				if (!advance(*child, action)) return true;
				if (canonicalMainEnabled && (!singletonRevealStreaming || concreteWorldCaching)
					&& child->selectType == SelectType::Main
					&& child->exact.pending == ExactPendingType::None) {
					std::string successorKey = keyFor(*child);
					auto successor = std::find_if(successorScores.begin(), successorScores.end(),
						[&](const SuccessorScoreEntry& entry) { return entry.key == successorKey; });
					if (successor != successorScores.end()) {
						score = successor->score;
						successor->count++;
						metrics.successorMerges++; metrics.groupedOutcomes++;
						metrics.largestEquivalenceClass = std::max(metrics.largestEquivalenceClass, successor->count);
					} else {
						// The successor key was built for the grouping lookup immediately
						// above.  Carry it into solveOwned so the same canonical bytes are
						// not rebuilt at the child entry point.
						score = solveOwned(std::move(child), successorKey);
						successorScores.push_back({ std::move(successorKey), score, 1ULL });
					}
				} else {
					if (child->exact.pending == ExactPendingType::RevealDeck) {
						score = revealAndReplay(state, child->exact, action);
					} else if ((child->exact.pending == ExactPendingType::Draw
						|| child->exact.pending == ExactPendingType::TakePrize) && !nodeKey.empty()) {
						// The pending state itself has no Main observation key. Derive a
						// collision-free continuation identity from the lossless parent PTM3
						// key and semantic action, so unrelated Rich draws cannot share one
						// resumable cursor.
						std::string semantic = actionKey.empty() ? actionEquivalenceKey(state, action) : actionKey;
						ExactPackedKeyWriter pendingKey;
						pendingKey.bytes.append("PDC2", 4); pendingKey.blob(nodeKey); pendingKey.blob(semantic);
						pendingKey.i32((int)child->exact.pending); pendingKey.i32(child->exact.pendingPlayer);
						pendingKey.i32(child->exact.pendingCount);
						score = solveOwned(std::move(child), std::move(pendingKey.bytes));
					} else score = solveOwned(std::move(child));
				}
				if (score.certified && useTransitionCache && !transitionKey.empty()
					&& transitionScoreCache.size() < MaxTransitionCacheEntries)
					transitionScoreCache.emplace(std::move(transitionKey), score);
			}
			if (partial != nullptr) {
				auto found = partial->actionBounds.find(actionKey);
				if (found == partial->actionBounds.end()) {
					size_t bytes = actionKey.size() + sizeof(ExactScore) + 32;
					partialBytes += bytes; partial->accountedBytes += bytes;
					partial->actionBounds.emplace(actionKey, score);
				} else {
					BoundMergeResult merged = mergeSoundBounds(found->second, score);
					if (merged.contradiction) ++metrics.boundContradictions;
					found->second = merged.score;
					score = found->second;
					metrics.resumedActionCount++;
				}
				partial->resumeOrdinal = thisOrdinal + 1;
			}
			if (recursionDepth == 1)
				rootActionValues.push_back({ action, score.lower, score.upper,
					score.certified && score.boundsSound });
			if (maximize) {
				if (ExactCompare(score.upper, aggregate) > 0) aggregate = score.upper;
			} else {
				if (ExactCompare(score.lower, aggregate) < 0) aggregate = score.lower;
			}
			allCertified = allCertified && score.certified;
			allBoundsSound = allBoundsSound && score.boundsSound;
			if (first || (maximize ? ExactCompare(score.lower, result.lower) > 0 : ExactCompare(score.upper, result.upper) < 0)) {
				result = score; result.action = action;
				selectedActionValueCertified = score.certified;
				first = false;
			}
			return !expired();
		});
		if (completed && partial != nullptr) partial->resumeOrdinal = 0;
		if (first) return unknown();
		if (!completed) {
			if (maximize) result.upper = ExactFraction::integer(100'000'000);
			else result.lower = ExactFraction::integer(-100'000'000);
			result.certified = false;
			result.boundsSound = allBoundsSound;
			metrics.partialDecisionNodes++;
			if (!singletonRevealStreaming) rememberPolicy(state, result,
				selectedActionValueCertified, false, false,
				metrics.expanded - expandedBefore);
			return result;
		}
		if (maximize) result.upper = aggregate; else result.lower = aggregate;
		result.certified = allCertified && allBoundsSound && ExactCompare(result.lower, result.upper) == 0;
		result.boundsSound = allBoundsSound;
		if (!singletonRevealStreaming) rememberPolicy(state, result,
			selectedActionValueCertified, result.certified, result.certified,
			metrics.expanded - expandedBefore);
		return result;
	}

	ExactScore solve(const State& input) { return solveOwned(cloneState(input)); }

	ExactScore solveOwned(ExactStatePtr owned, std::string keyHint = {}) {
		struct DepthGuard { int& depth; DepthGuard(int& value) : depth(value) { depth++; } ~DepthGuard() { depth--; } } guard(recursionDepth);
		metrics.maxDepth = std::max(metrics.maxDepth, recursionDepth);
		if (recursionDepth > 384) {
			metrics.depthLimitNodes++;
			metrics.lastDepthSelectType = (int)owned->selectType;
			metrics.lastDepthTurnActionCount = owned->turnActionCount;
			return unknown();
		}
		State& state = *owned;
		metrics.expanded++;
		if (expired()) return unknown();
		// Automatic engine continuations are not search-tree depth.  Running
		// each one through solve() recursively can exhaust the C++ stack long
		// before the wall-clock budget, so settle them iteratively.
		try {
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
				stepExact(state);
				metrics.expanded++;
				if (expired()) return unknown();
			}
			// Macro-collapse forced single-option effect selections into one edge.
			while (!state.isFinish() && !IsExactTurnLeaf(state)
				&& state.exact.pending == ExactPendingType::None
				&& state.selectType != SelectType::Main
				&& state.selectType != SelectType::Attack
				&& state.selectType != SelectType::YesNo
				&& state.selectMin == 1 && state.selectMax == 1
				&& state.options.size() == 1) {
				if (!advance(state, ExactSmallAction{ 0 })) break;
				metrics.macroCollapsedTransitions++;
				metrics.expanded++;
				while (!state.isFinish() && !IsExactTurnLeaf(state)
					&& state.exact.pending == ExactPendingType::None && state.selectType == SelectType::None) {
					stepExact(state);
					metrics.expanded++;
					if (expired()) return unknown();
				}
				if (expired()) return unknown();
			}
		} catch (const std::exception& error) {
			metrics.exceptions++; metrics.lastException = error.what(); return unknown();
		} catch (...) {
			metrics.exceptions++; metrics.lastException = "automatic transition"; return unknown();
		}
		// Terminal turn leaves are cheap to evaluate and overwhelmingly unique.
		// Storing their full State keys consumed the TT before any reusable Main
		// node could be completed.
		if (state.isFinish() || IsExactTurnLeaf(state)) {
			metrics.leaves++;
			return boundaryScore(state, evaluate(state));
		}
		// A search has made this concrete world a singleton information set, but
		// Main states still contain many commuting action orders.  The
		// dynamic partition key preserves every card count that can affect another
		// turn-local deck query while omitting the irrelevant prize identities already
		// quotiented by revealAndReplayPartitionedTurnSearch.
		const std::string* partitionMainKeyPtr = partitionTurnMainKey(state);
		const bool partitionLookupDone = partitionMainKeyPtr != nullptr;
		size_t partitionMainHash = 0;
		bool partitionMainHashKnown = false;
		if (partitionMainKeyPtr != nullptr) {
			const std::string& partitionMainKey = *partitionMainKeyPtr;
			ExactScore partitionCached;
			bool partitionHit = false, sharedHit = false;
			if (usingSharedTable) {
				partitionHit = sharedHit = sharedFind(partitionMainKey, partitionCached, &partitionMainHash);
				partitionMainHashKnown = true;
			}
			else {
				auto local = partitionTurnMainScores.find(partitionMainKey);
				partitionHit = local != partitionTurnMainScores.end();
				if (partitionHit) partitionCached = local->second;
			}
			if (partitionHit) {
				metrics.merged++; metrics.canonicalStateMerges++;
				if (sharedHit) metrics.rootSharedTTHits++;
				return partitionCached;
			}
		}
		// A concrete reveal world can still contain a large turn DAG (notably when
		// Enriching Energy enables a four-card draw). The former streaming fast path
		// disabled both TT and partial cursors, so a 500k-node quantum restarted that
		// world from its first action forever. Retain exact canonical state and
		// resumable entries whenever root canonicalization is enabled.
		const bool cacheConcreteWorld = singletonRevealStreaming && canonicalMainEnabled && concreteWorldCaching
			&& (state.selectType == SelectType::Main
				|| state.exact.pending == ExactPendingType::Draw
				|| state.exact.pending == ExactPendingType::TakePrize);
		std::string key;
		if (partitionMainKeyPtr == nullptr) {
			if (!keyHint.empty()) key = std::move(keyHint);
			else if (!singletonRevealStreaming || cacheConcreteWorld) key = keyFor(state);
		}
		const std::string& nodeKey = partitionMainKeyPtr != nullptr ? *partitionMainKeyPtr : key;
		const bool shareable = usingSharedTable && (!singletonRevealStreaming || cacheConcreteWorld);
		ExactScore cached;
		bool cacheHit = false;
		size_t nodeHash = 0;
		bool nodeHashKnown = false;
		// partitionMainKey was already queried above.  It is also the primary TT
		// key for this node; hashing and probing the same bytes a second time made
		// every Main miss pay for two TT lookups.
		if (shareable && !partitionLookupDone) {
			cacheHit = sharedFind(nodeKey, cached, &nodeHash);
			nodeHashKnown = true;
		}
		else if (!shareable && !partitionLookupDone && (!singletonRevealStreaming || cacheConcreteWorld)) {
			auto found = localTransposition.find(nodeKey);
			if (found != localTransposition.end()) {
				cached = found->second; cacheHit = true; metrics.ttReadHits++;
			} else metrics.ttReadMisses++;
		}
		if (cacheHit) {
			metrics.merged++;
			if (shareable) metrics.canonicalStateMerges++;
			if (shareable && usingSharedTable) metrics.rootSharedTTHits++;
			return cached;
		}
		struct FlightGuard {
			ExactSharedTransposition* table = nullptr;
			std::shared_ptr<ExactSharedTransposition::FlightState> flight;
			bool finished = false;
			~FlightGuard() { if (!finished && table != nullptr && flight) table->finishFlight(flight, nullptr); }
			void finish(const ExactScore* value) {
				if (!finished && table != nullptr && flight) table->finishFlight(flight, value);
				finished = true;
			}
		} flightGuard;
		if (shareable && runtimeMode != ExactRuntimeMode::Legacy && inFlightClaimsEnabled) {
			while (true) {
				auto waitStarted = std::chrono::steady_clock::now();
				auto claim = transposition->claim(nodeKey, deadline);
				if (claim.waited) {
					metrics.inFlightWaits++;
					metrics.workerWaitNs += (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - waitStarted).count();
				}
				if (claim.status == ExactSharedTransposition::ClaimStatus::Released) {
					if (std::chrono::steady_clock::now() >= deadline) { metrics.timedOut = true; return unknown(); }
					continue;
				}
				if (claim.status == ExactSharedTransposition::ClaimStatus::TimedOut) {
					metrics.timedOut = true; return unknown();
				}
				if (claim.status == ExactSharedTransposition::ClaimStatus::SelfDuplicate) {
					metrics.workerDuplicateClaims++; break;
				}
				if (claim.status == ExactSharedTransposition::ClaimStatus::Completed) {
					metrics.ttReadHits++; metrics.rootSharedTTHits++; metrics.merged++; metrics.canonicalStateMerges++;
					return claim.value;
				}
				flightGuard.table = transposition.get(); flightGuard.flight = std::move(claim.flight);
				break;
			}
		}
		ExactScore result;
		if (state.exact.pending == ExactPendingType::Opaque || state.exact.pending == ExactPendingType::RevealDeck) {
			metrics.opaque++; metrics.lastPendingDetail = state.exact.pendingDetail;
			metrics.lastPendingPlayer = state.exact.pendingPlayer;
			metrics.lastPendingEffectCardId = state.exact.pendingEffectCardId;
			metrics.lastPendingEffectPlayer = state.exact.pendingEffectPlayer;
			metrics.lastPendingNullCount = state.exact.pendingNullCount;
			if (state.exact.pendingPlayer >= 0) metrics.lastPendingDeckUnknown = state.exact.deckUnknown[state.exact.pendingPlayer];
			switch (state.exact.blockReason) {
			case ExactBlockReason::UnknownOpponentList:
				metrics.unknownOpponentList++; metrics.structurallyBlocked = true; break;
			case ExactBlockReason::InterruptedTransition: metrics.interruptedTransition++; break;
			default: metrics.unsupportedConcreteReference++; break;
			}
			result = unknown();
		} else if (state.exact.pending == ExactPendingType::Draw || state.exact.pending == ExactPendingType::TakePrize) {
			result = chance(state, nodeKey);
		} else if (state.selectType == SelectType::YesNo && state.selectContext == SelectContext::CoinHead) {
			result = coinChance(state, nodeKey);
		} else {
			result = decision(state, state.selectPlayer == actor, nodeKey);
		}
		if (result.certified && (!singletonRevealStreaming || cacheConcreteWorld)) {
			auto partialDecision = partialDecisions.find(nodeKey);
			if (partialDecision != partialDecisions.end()) {
				partialBytes -= std::min(partialBytes, partialDecision->second.accountedBytes);
				partialDecisions.erase(partialDecision);
			}
			auto partialChance = partialChances.find(nodeKey);
			if (partialChance != partialChances.end()) {
				partialBytes -= std::min(partialBytes, partialChance->second.accountedBytes);
				partialChances.erase(partialChance);
			}
			if (partitionMainKeyPtr == nullptr && shareable)
				sharedStore(std::move(key), result, nodeHashKnown ? &nodeHash : nullptr);
			else if (partitionMainKeyPtr == nullptr) {
				size_t bytes = key.size() + sizeof(ExactScore) + 96;
				if (localTransposition.size() < MaxLocalTranspositionEntries
					&& localTranspositionBytes + bytes <= MaxLocalTranspositionBytes) {
					localTranspositionBytes += bytes;
					localTransposition.emplace(std::move(key), result);
					metrics.ttInsertions++;
				}
			}
		}
		// Partition Main entries remain valid even while a singleton reveal is in
		// streaming mode.  This store is intentionally outside the generic-world
		// cache condition above.
		if (result.certified && partitionMainKeyPtr != nullptr) {
			if (usingSharedTable) sharedStore(*partitionMainKeyPtr, result,
				partitionMainHashKnown ? &partitionMainHash : nullptr);
			else {
				partitionTurnMainScores.emplace(*partitionMainKeyPtr, result);
				metrics.ttInsertions++;
			}
		}
		flightGuard.finish(result.certified ? &result : nullptr);
		metrics.partialTableBytes = partialBytes;
		metrics.sessionBytes = transposition->bytes() + localTranspositionBytes + policyBytes + partialBytes
			+ beliefTranspositionBytes + evaluationCache.size() * 128ULL
			+ transitionScoreCache.size() * 128ULL;
		return result;
	}
};
