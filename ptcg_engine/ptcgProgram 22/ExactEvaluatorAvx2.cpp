#include "ExactEvaluatorAvx2.h"

#if defined(_MSC_VER) || ((defined(__GNUC__) || defined(__clang__)) \
	&& (defined(__x86_64__) || defined(__i386__)))
#include <immintrin.h>

#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
#define EXACT_AVX2_TARGET __attribute__((target("avx2")))
#else
#define EXACT_AVX2_TARGET
#endif
EXACT_AVX2_TARGET
void ExactAddScaledI16ToI64Avx2(const std::int16_t* weights, std::int32_t value,
	std::int64_t* accumulator, std::size_t count) {
	const __m256i multiplier = _mm256_set1_epi32(value);
	std::size_t at = 0;
	for (; at + 8 <= count; at += 8) {
		const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + at));
		const __m256i widened = _mm256_cvtepi16_epi32(packed);
		const __m256i products = _mm256_mullo_epi32(widened, multiplier);
		const __m256i low = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(products));
		const __m256i high = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(products, 1));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(accumulator + at),
			_mm256_add_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(accumulator + at)), low));
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(accumulator + at + 4),
			_mm256_add_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(accumulator + at + 4)), high));
	}
	for (; at < count; ++at) accumulator[at] += (std::int64_t)weights[at] * value;
}

EXACT_AVX2_TARGET void ExactAddScaledI16ToI32Avx2(const std::int16_t* weights, std::int32_t value,
	std::int32_t* accumulator, std::size_t count) {
	const __m256i multiplier = _mm256_set1_epi32(value);
	std::size_t at = 0;
	for (; at + 8 <= count; at += 8) {
		const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + at));
		const __m256i widened = _mm256_cvtepi16_epi32(packed);
		const __m256i products = _mm256_mullo_epi32(widened, multiplier);
		_mm256_storeu_si256(reinterpret_cast<__m256i*>(accumulator + at),
			_mm256_add_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(accumulator + at)), products));
	}
	for (; at < count; ++at) accumulator[at] += (std::int32_t)weights[at] * value;
}

EXACT_AVX2_TARGET void ExactAddDenseI16ToI32Avx2(const std::int16_t* weightsByInput,
	const std::int32_t* values, std::int32_t* accumulator,
	std::size_t inputCount, std::size_t hiddenCount) {
	for (std::size_t input = 0; input < inputCount; ++input) {
		if (values[input] == 0) continue;
		ExactAddScaledI16ToI32Avx2(weightsByInput + input * hiddenCount,
			values[input], accumulator, hiddenCount);
	}
}

#else

void ExactAddScaledI16ToI64Avx2(const std::int16_t* weights, std::int32_t value,
	std::int64_t* accumulator, std::size_t count) {
	for (std::size_t at = 0; at < count; ++at) accumulator[at] += (std::int64_t)weights[at] * value;
}

void ExactAddScaledI16ToI32Avx2(const std::int16_t* weights, std::int32_t value,
	std::int32_t* accumulator, std::size_t count) {
	for (std::size_t at = 0; at < count; ++at) accumulator[at] += (std::int32_t)weights[at] * value;
}

void ExactAddDenseI16ToI32Avx2(const std::int16_t* weightsByInput,
	const std::int32_t* values, std::int32_t* accumulator,
	std::size_t inputCount, std::size_t hiddenCount) {
	for (std::size_t input = 0; input < inputCount; ++input)
		ExactAddScaledI16ToI32Avx2(weightsByInput + input * hiddenCount,
			values[input], accumulator, hiddenCount);
}

#endif
