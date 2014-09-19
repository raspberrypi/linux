/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * DOC: Shader validator for VC4.
 *
 * The VC4 has no IOMMU between it and system memory.  So, a user with access
 * to execute shaders could escalate privilege by overwriting system memory
 * (using the VPM write address register in the general-purpose DMA mode) or
 * reading system memory it shouldn't (reading it as a texture, or uniform
 * data, or vertex data).
 *
 * This walks over a shader starting from some offset within a BO, ensuring
 * that its accesses are appropriately bounded, and recording how many texture
 * accesses are made and where so that we can do relocations for them in the
 * uniform stream.
 *
 * The kernel API has shaders stored in user-mapped BOs.  The BOs will be
 * forcibly unmapped from the process before validation, and any cache of
 * validated state will be flushed if the mapping is faulted back in.
 *
 * Storing the shaders in BOs means that the validation process will be slow
 * due to uncached reads, but since shaders are long-lived and shader BOs are
 * never actually modified, this shouldn't be a problem.
 */

#include "vc4_drv.h"
#include "vc4_qpu_defines.h"

struct vc4_shader_validation_state {
	struct vc4_texture_sample_info tmu_setup[2];
	int tmu_write_count[2];
};

static bool
is_tmu_write(uint32_t waddr)
{
	return (waddr >= QPU_W_TMU0_S &&
		waddr <= QPU_W_TMU1_B);
}

static bool
record_validated_texture_sample(struct vc4_validated_shader_info *validated_shader,
				struct vc4_shader_validation_state *validation_state,
				int tmu)
{
	uint32_t s = validated_shader->num_texture_samples;
	int i;
	struct vc4_texture_sample_info *temp_samples;

	temp_samples = krealloc(validated_shader->texture_samples,
				(s + 1) * sizeof(*temp_samples),
				GFP_KERNEL);
	if (!temp_samples)
		return false;

	memcpy(temp_samples[s].p_offset,
	       validation_state->tmu_setup[tmu].p_offset,
	       validation_state->tmu_write_count[tmu] * sizeof(uint32_t));
	for (i = validation_state->tmu_write_count[tmu]; i < 4; i++)
		temp_samples[s].p_offset[i] = ~0;

	validated_shader->num_texture_samples = s + 1;
	validated_shader->texture_samples = temp_samples;

	return true;
}

static bool
check_tmu_write(struct vc4_validated_shader_info *validated_shader,
		struct vc4_shader_validation_state *validation_state,
		uint32_t waddr)
{
	int tmu = waddr > QPU_W_TMU0_B;

	if (!is_tmu_write(waddr))
		return true;

	if (validation_state->tmu_write_count[tmu] >= 4) {
		DRM_ERROR("TMU%d got too many parameters before dispatch\n",
			  tmu);
		return false;
	}
	validation_state->tmu_setup[tmu].p_offset[validation_state->tmu_write_count[tmu]] =
		validated_shader->uniforms_size;
	validation_state->tmu_write_count[tmu]++;
	validated_shader->uniforms_size += 4;

	if (waddr == QPU_W_TMU0_S || waddr == QPU_W_TMU1_S) {
		if (!record_validated_texture_sample(validated_shader,
						     validation_state, tmu)) {
			return false;
		}

		validation_state->tmu_write_count[tmu] = 0;
	}

	return true;
}

static bool
check_register_write(struct vc4_validated_shader_info *validated_shader,
		     struct vc4_shader_validation_state *validation_state,
		     uint32_t waddr)
{
	switch (waddr) {
	case QPU_W_UNIFORMS_ADDRESS:
		/* XXX: We'll probably need to support this for reladdr, but
		 * it's definitely a security-related one.
		 */
		DRM_ERROR("uniforms address load unsupported\n");
		return false;

	case QPU_W_TLB_COLOR_MS:
	case QPU_W_TLB_COLOR_ALL:
	case QPU_W_TLB_Z:
		/* These only interact with the tile buffer, not main memory,
		 * so they're safe.
		 */
		return true;

	case QPU_W_TMU0_S:
	case QPU_W_TMU0_T:
	case QPU_W_TMU0_R:
	case QPU_W_TMU0_B:
	case QPU_W_TMU1_S:
	case QPU_W_TMU1_T:
	case QPU_W_TMU1_R:
	case QPU_W_TMU1_B:
		return check_tmu_write(validated_shader, validation_state,
				       waddr);

	case QPU_W_HOST_INT:
	case QPU_W_TMU_NOSWAP:
	case QPU_W_TLB_ALPHA_MASK:
	case QPU_W_MUTEX_RELEASE:
		/* XXX: I haven't thought about these, so don't support them
		 * for now.
		 */
		DRM_ERROR("Unsupported waddr %d\n", waddr);
		return false;

	case QPU_W_VPM_ADDR:
		DRM_ERROR("General VPM DMA unsupported\n");
		return false;

	case QPU_W_VPM:
	case QPU_W_VPMVCD_SETUP:
		/* We allow VPM setup in general, even including VPM DMA
		 * configuration setup, because the (unsafe) DMA can only be
		 * triggered by QPU_W_VPM_ADDR writes.
		 */
		return true;

	case QPU_W_TLB_STENCIL_SETUP:
                return true;
	}

	return true;
}

static bool
check_instruction_writes(uint64_t inst,
			 struct vc4_validated_shader_info *validated_shader,
			 struct vc4_shader_validation_state *validation_state)
{
	uint32_t waddr_add = QPU_GET_FIELD(inst, QPU_WADDR_ADD);
	uint32_t waddr_mul = QPU_GET_FIELD(inst, QPU_WADDR_MUL);

	if (is_tmu_write(waddr_add) && is_tmu_write(waddr_mul)) {
		DRM_ERROR("ADD and MUL both set up textures\n");
		return false;
	}

	return (check_register_write(validated_shader, validation_state, waddr_add) &&
		check_register_write(validated_shader, validation_state, waddr_mul));
}

static bool
check_instruction_reads(uint64_t inst,
			struct vc4_validated_shader_info *validated_shader)
{
	uint32_t waddr_add = QPU_GET_FIELD(inst, QPU_WADDR_ADD);
	uint32_t waddr_mul = QPU_GET_FIELD(inst, QPU_WADDR_MUL);
	uint32_t raddr_a = QPU_GET_FIELD(inst, QPU_RADDR_A);
	uint32_t raddr_b = QPU_GET_FIELD(inst, QPU_RADDR_B);

	if (raddr_a == QPU_R_UNIF ||
	    raddr_b == QPU_R_UNIF) {
		if (is_tmu_write(waddr_add) || is_tmu_write(waddr_mul)) {
			DRM_ERROR("uniform read in the same instruction as "
				  "texture setup");
			return false;
		}

		/* This can't overflow the uint32_t, because we're reading 8
		 * bytes of instruction to increment by 4 here, so we'd
		 * already be OOM.
		 */
		validated_shader->uniforms_size += 4;
	}

	return true;
}

struct vc4_validated_shader_info *
vc4_validate_shader(struct drm_gem_cma_object *shader_obj,
		    uint32_t start_offset)
{
	bool found_shader_end = false;
	int shader_end_ip = 0;
	uint32_t ip, max_ip;
	uint64_t *shader;
	struct vc4_validated_shader_info *validated_shader;
	struct vc4_shader_validation_state validation_state;

	memset(&validation_state, 0, sizeof(validation_state));

	if (start_offset + sizeof(uint64_t) > shader_obj->base.size) {
		DRM_ERROR("shader starting at %d outside of BO sized %d\n",
			  start_offset,
			  shader_obj->base.size);
		return NULL;
	}
	shader = shader_obj->vaddr + start_offset;
	max_ip = (shader_obj->base.size - start_offset) / sizeof(uint64_t);

	validated_shader = kcalloc(sizeof(*validated_shader), 1, GFP_KERNEL);
	if (!validated_shader)
		return NULL;

	for (ip = 0; ip < max_ip; ip++) {
		uint64_t inst = shader[ip];
		uint32_t sig = QPU_GET_FIELD(inst, QPU_SIG);

		switch (sig) {
		case QPU_SIG_NONE:
		case QPU_SIG_WAIT_FOR_SCOREBOARD:
		case QPU_SIG_SCOREBOARD_UNLOCK:
		case QPU_SIG_COLOR_LOAD:
		case QPU_SIG_LOAD_TMU0:
		case QPU_SIG_LOAD_TMU1:
			if (!check_instruction_writes(inst, validated_shader,
						      &validation_state)) {
				DRM_ERROR("Bad write at ip %d\n", ip);
				goto fail;
			}

			if (!check_instruction_reads(inst, validated_shader))
				goto fail;

			break;

		case QPU_SIG_LOAD_IMM:
			if (!check_instruction_writes(inst, validated_shader,
						      &validation_state)) {
				DRM_ERROR("Bad LOAD_IMM write at ip %d\n", ip);
				goto fail;
			}
			break;

		case QPU_SIG_PROG_END:
			found_shader_end = true;
			shader_end_ip = ip;
			break;

		default:
			DRM_ERROR("Unsupported QPU signal %d at "
				  "instruction %d\n", sig, ip);
			goto fail;
		}

		/* There are two delay slots after program end is signaled
		 * that are still executed, then we're finished.
		 */
		if (found_shader_end && ip == shader_end_ip + 2)
			break;
	}

	if (ip == max_ip) {
		DRM_ERROR("shader starting at %d failed to terminate before "
			  "shader BO end at %d\n",
			  start_offset,
			  shader_obj->base.size);
		goto fail;
	}

	/* Again, no chance of integer overflow here because the worst case
	 * scenario is 8 bytes of uniforms plus handles per 8-byte
	 * instruction.
	 */
	validated_shader->uniforms_src_size =
		(validated_shader->uniforms_size +
		 4 * validated_shader->num_texture_samples);

	return validated_shader;

fail:
	kfree(validated_shader);
	return NULL;
}
