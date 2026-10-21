#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   zero_stride_oob                 = false;
		bool                   scalar_empty                    = false;
		uint32_t               indirect_root                   = BufferResource::NoIndirectBuffer;
		uint32_t               indirect_mapping_offset         = 0;
		uint32_t               indirect_search_iterations      = 0;
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		bool                          operator==(const Image&) const = default;
	};

	struct Sampler {
		uint32_t indirect_root = SamplerResource::NoIndirectSampler;
		uint32_t indirect_mapping_offset = 0;
		uint32_t indirect_search_iterations = 0;
		bool operator==(const Sampler&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;
	std::vector<Sampler> samplers;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Refreshes cached resources and specialization in place. A failed refresh must not be used.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization);

// MaterializeResources for one plan that reuses earlier interpreted results. An entry matches
// when the user data registers, shader base and workgroup inputs it read are unchanged and every
// guest read it made, re-read in the original order, returns the same words. Plans refreshed by
// the native SRT walker are cheap already and always rematerialize.
class ResourceMemo {
public:
	bool Materialize(const ResourcePlan& program, const SrtRuntime& runtime,
	                 ResourceSnapshot& snapshot, ResourceSpecialization& specialization);
	// Required whenever the plan changes.
	void Clear();

	[[nodiscard]] uint64_t Hits() const { return m_hits; }
	[[nodiscard]] uint64_t Misses() const { return m_misses; }

private:
	struct Read {
		uint64_t address = 0;
		uint32_t first   = 0;
		uint32_t count   = 0;
		bool     strict  = false;
		bool     ok      = false;
	};
	struct Entry {
		bool                   valid          = false;
		uint64_t               user_data_mask = 0;
		size_t                 user_data_size = 0;
		std::vector<uint32_t>  user_data;
		uint64_t               shader_base           = 0;
		uint64_t               indirect_args_address = 0;
		std::vector<uint32_t>  workgroup_counts;
		std::vector<Read>      reads;
		std::vector<uint32_t>  words;
		ResourceSnapshot       snapshot;
		ResourceSpecialization specialization;
	};
	class Recorder;

	static constexpr size_t Capacity = 32;

	[[nodiscard]] static bool InputsMatch(const Entry& entry, const SrtRuntime& runtime);
	bool                      ReadsMatch(const Entry& entry, const SrtRuntime& runtime);

	std::vector<Entry>    m_entries;
	size_t                m_next   = 0;
	uint64_t              m_hits   = 0;
	uint64_t              m_misses = 0;
	std::vector<uint32_t> m_scratch;
};

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
