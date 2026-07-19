#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
inline bool ExactCpuSupportsAvx2() {
	int registers[4]{};
	__cpuidex(registers, 1, 0);
	constexpr int Osxsave = 1 << 27;
	constexpr int Avx = 1 << 28;
	if ((registers[2] & (Osxsave | Avx)) != (Osxsave | Avx)) return false;
	const unsigned long long xcr0 = _xgetbv(0);
	if ((xcr0 & 0x6) != 0x6) return false;
	__cpuidex(registers, 7, 0);
	return (registers[1] & (1 << 5)) != 0;
}
#elif (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
inline bool ExactCpuSupportsAvx2() {
	__builtin_cpu_init();
	return __builtin_cpu_supports("avx2");
}
#else
inline bool ExactCpuSupportsAvx2() { return false; }
#endif

// Implemented in its own translation unit.  On MSVC only this file is built
// with /arch:AVX2, so the portable engine cannot accidentally acquire an AVX2
// requirement through auto-vectorisation.
void ExactAddScaledI16ToI64Avx2(const std::int16_t* weights, std::int32_t value,
	std::int64_t* accumulator, std::size_t count);
void ExactAddScaledI16ToI32Avx2(const std::int16_t* weights, std::int32_t value,
	std::int32_t* accumulator, std::size_t count);
void ExactAddDenseI16ToI32Avx2(const std::int16_t* weightsByInput,
	const std::int32_t* values, std::int32_t* accumulator,
	std::size_t inputCount, std::size_t hiddenCount);
