/*
 * Copyright © 2013 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "intel_batchbuffer.h"


static void
gen7_upload_gs_push_constants(struct brw_context *brw)
{
   /* BRW_NEW_GEOMETRY_PROGRAM */
   const struct brw_geometry_program *vp =
      (struct brw_geometry_program *) brw->geometry_program;
   if (!vp)
      return;

   /* CACHE_NEW_GS_PROG */
   const struct brw_vec4_prog_data *prog_data = &brw->gs.prog_data->base;
   struct brw_stage_state *stage_state = &brw->gs.base;

   gen6_upload_vec4_push_constants(brw, &vp->program.Base, prog_data,
                                   stage_state, AUB_TRACE_VS_CONSTANTS);
}

const struct brw_tracked_state gen7_gs_push_constants = {
   .dirty = {
      .mesa  = _NEW_TRANSFORM | _NEW_PROGRAM_CONSTANTS,
      .brw   = (BRW_NEW_BATCH |
		BRW_NEW_GEOMETRY_PROGRAM),
      .cache = CACHE_NEW_GS_PROG,
   },
   .emit = gen7_upload_gs_push_constants,
};


static void
upload_gs_state(struct brw_context *brw)
{
   const struct brw_stage_state *stage_state = &brw->gs.base;
   const int max_threads_shift = brw->is_haswell ?
      HSW_GS_MAX_THREADS_SHIFT : GEN6_GS_MAX_THREADS_SHIFT;
   /* BRW_NEW_GEOMETRY_PROGRAM */
   bool active = brw->geometry_program;
   /* CACHE_NEW_GS_PROG */
   const struct brw_vec4_prog_data *prog_data = &brw->gs.prog_data->base;

   /* BRW_NEW_GS_BINDING_TABLE */
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_BINDING_TABLE_POINTERS_GS << 16 | (2 - 2));
   OUT_BATCH(stage_state->bind_bo_offset);
   ADVANCE_BATCH();

   /* CACHE_NEW_SAMPLER */
   BEGIN_BATCH(2);
   OUT_BATCH(_3DSTATE_SAMPLER_STATE_POINTERS_GS << 16 | (2 - 2));
   OUT_BATCH(stage_state->sampler_offset);
   ADVANCE_BATCH();

   gen7_upload_constant_state(brw, stage_state, active, _3DSTATE_CONSTANT_GS);

   if (active) {
      BEGIN_BATCH(7);
      OUT_BATCH(_3DSTATE_GS << 16 | (7 - 2));
      OUT_BATCH(stage_state->prog_offset);
      OUT_BATCH(((ALIGN(stage_state->sampler_count, 4)/4) <<
                 GEN6_GS_SAMPLER_COUNT_SHIFT) |
                ((brw->gs.prog_data->base.base.binding_table.size_bytes / 4) <<
                 GEN6_GS_BINDING_TABLE_ENTRY_COUNT_SHIFT));

      if (brw->gs.prog_data->base.total_scratch) {
         OUT_RELOC(stage_state->scratch_bo,
                   I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                   ffs(brw->gs.prog_data->base.total_scratch) - 11);
      } else {
         OUT_BATCH(0);
      }

      uint32_t dw4 =
         ((brw->gs.prog_data->output_vertex_size_hwords * 2 - 1) <<
          GEN7_GS_OUTPUT_VERTEX_SIZE_SHIFT) |
         (brw->gs.prog_data->output_topology <<
          GEN7_GS_OUTPUT_TOPOLOGY_SHIFT) |
         (prog_data->urb_read_length <<
          GEN6_GS_URB_READ_LENGTH_SHIFT) |
         (0 << GEN6_GS_URB_ENTRY_READ_OFFSET_SHIFT) |
         (prog_data->dispatch_grf_start_reg <<
          GEN6_GS_DISPATCH_START_GRF_SHIFT);

      /* Note: the meaning of the GEN7_GS_REORDER_TRAILING bit changes between
       * Ivy Bridge and Haswell.
       *
       * On Ivy Bridge, setting this bit causes the vertices of a triangle
       * strip to be delivered to the geometry shader in an order that does
       * not strictly follow the OpenGL spec, but preserves triangle
       * orientation.  For example, if the vertices are (1, 2, 3, 4, 5), then
       * the geometry shader sees triangles:
       *
       * (1, 2, 3), (2, 4, 3), (3, 4, 5)
       *
       * (Clearing the bit is even worse, because it fails to preserve
       * orientation).
       *
       * Triangle strips with adjacency always ordered in a way that preserves
       * triangle orientation but does not strictly follow the OpenGL spec,
       * regardless of the setting of this bit.
       *
       * On Haswell, both triangle strips and triangle strips with adjacency
       * are always ordered in a way that preserves triangle orientation.
       * Setting this bit causes the ordering to strictly follow the OpenGL
       * spec.
       *
       * So in either case we want to set the bit.  Unfortunately on Ivy
       * Bridge this will get the order close to correct but not perfect.
       */
      uint32_t dw5 =
         ((brw->max_gs_threads - 1) << max_threads_shift) |
         (brw->gs.prog_data->control_data_header_size_hwords <<
          GEN7_GS_CONTROL_DATA_HEADER_SIZE_SHIFT) |
         (brw->gs.prog_data->dual_instanced_dispatch ?
          GEN7_GS_DISPATCH_MODE_DUAL_INSTANCE :
          GEN7_GS_DISPATCH_MODE_DUAL_OBJECT) |
         GEN6_GS_STATISTICS_ENABLE |
         (brw->gs.prog_data->include_primitive_id ?
          GEN7_GS_INCLUDE_PRIMITIVE_ID : 0) |
         GEN7_GS_REORDER_TRAILING |
         GEN7_GS_ENABLE;
      uint32_t dw6 = 0;

      if (brw->is_haswell) {
         dw6 |= brw->gs.prog_data->control_data_format <<
            HSW_GS_CONTROL_DATA_FORMAT_SHIFT;
      } else {
         dw5 |= brw->gs.prog_data->control_data_format <<
            IVB_GS_CONTROL_DATA_FORMAT_SHIFT;
      }

      OUT_BATCH(dw4);
      OUT_BATCH(dw5);
      OUT_BATCH(dw6);
      ADVANCE_BATCH();
   } else {
      BEGIN_BATCH(7);
      OUT_BATCH(_3DSTATE_GS << 16 | (7 - 2));
      OUT_BATCH(0); /* prog_bo */
      OUT_BATCH((0 << GEN6_GS_SAMPLER_COUNT_SHIFT) |
                (0 << GEN6_GS_BINDING_TABLE_ENTRY_COUNT_SHIFT));
      OUT_BATCH(0); /* scratch space base offset */
      OUT_BATCH((1 << GEN6_GS_DISPATCH_START_GRF_SHIFT) |
                (0 << GEN6_GS_URB_READ_LENGTH_SHIFT) |
                GEN7_GS_INCLUDE_VERTEX_HANDLES |
                (0 << GEN6_GS_URB_ENTRY_READ_OFFSET_SHIFT));
      OUT_BATCH((0 << GEN6_GS_MAX_THREADS_SHIFT) |
                GEN6_GS_STATISTICS_ENABLE);
      OUT_BATCH(0);
      ADVANCE_BATCH();
   }
}

const struct brw_tracked_state gen7_gs_state = {
   .dirty = {
      .mesa  = _NEW_TRANSFORM | _NEW_PROGRAM_CONSTANTS,
      .brw   = (BRW_NEW_CONTEXT |
                BRW_NEW_GEOMETRY_PROGRAM |
                BRW_NEW_GS_BINDING_TABLE |
                BRW_NEW_BATCH |
                BRW_NEW_PUSH_CONSTANT_ALLOCATION),
      .cache = CACHE_NEW_GS_PROG | CACHE_NEW_SAMPLER
   },
   .emit = upload_gs_state,
};
