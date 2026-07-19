#pragma once

#include <cstdint>
#include <cstring>

// A model being loadable is not a proof that its feature projection is
// information-set safe.  This fixed-size manifest is appended to every native
// evaluator payload and is validated against the code's schema constants.
// The full model bytes remain covered by the model checksum; the manifest is
// also included in the file-size check by each loader.
#pragma pack(push, 1)
struct ExactEvaluatorManifestDisk {
	char magic[8];
	std::uint32_t version;
	std::uint32_t schemaVersion;
	std::uint64_t featureSchemaHash;
	std::uint64_t beliefSchemaHash;
	std::uint8_t informationSetSafe;
	std::uint8_t boundaryOnly;
	std::uint8_t reserved[6];
};
#pragma pack(pop)

static_assert(sizeof(ExactEvaluatorManifestDisk) == 40);

inline constexpr char ExactEvaluatorManifestMagic[8] = {
	'P', 'T', 'C', 'G', 'M', 'A', 'N', '3'
};
inline constexpr std::uint32_t ExactEvaluatorManifestVersion = 1;
inline constexpr std::uint64_t ExactFeatureSchemaHash = 0xb694f079709a9522ULL;
inline constexpr std::uint64_t ExactBeliefSchemaHash = 0xa30c8939b0a5dbb2ULL;

inline bool ExactEvaluatorManifestMatches(const ExactEvaluatorManifestDisk& manifest,
	std::uint32_t schemaVersion, bool boundaryOnly = true) {
	return std::memcmp(manifest.magic, ExactEvaluatorManifestMagic, sizeof(manifest.magic)) == 0
		&& manifest.version == ExactEvaluatorManifestVersion
		&& manifest.schemaVersion == schemaVersion
		&& manifest.featureSchemaHash == ExactFeatureSchemaHash
		&& manifest.beliefSchemaHash == ExactBeliefSchemaHash
		&& manifest.informationSetSafe != 0
		&& (manifest.boundaryOnly != 0) == boundaryOnly;
}
