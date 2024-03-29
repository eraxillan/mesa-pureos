/*
 * Copyright (C) 2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "compiler.h"

/* Creates pipeline registers. This is a prepass run before the main register
 * allocator but after scheduling, once bundles are created. It works by
 * iterating the scheduled IR, checking if a value is ever used after the end
 * of the current bundle. If it is not, it is promoted to a bundle-specific
 * pipeline register.
 *
 * Pipeline registers are only written from the first two stages of the
 * pipeline (vmul/sadd) lasting the duration of the bundle only. There are two
 * 128-bit pipeline registers available (r24/r25). The upshot is that no actual
 * register allocation is needed; we can _always_ promote a value to a pipeline
 * register, liveness permitting. This greatly simplifies the logic of this
 * passing, negating the need for a proper RA like work registers.
 */

static bool
mir_pipeline_ins(
                compiler_context *ctx,
                midgard_block *block,
                midgard_bundle *bundle, unsigned i,
                unsigned pipeline_count)
{
        midgard_instruction *ins = bundle->instructions[i];
        unsigned dest = ins->ssa_args.dest;

        /* Check to make sure we're legal */

        if (ins->compact_branch)
                return false;

        /* Don't allow non-SSA. Pipelining registers is theoretically possible,
         * but the analysis is much hairier, so don't bother quite yet */
        if ((dest < 0) || (dest >= ctx->func->impl->ssa_alloc))
                return false;

        /* We want to know if we live after this bundle, so check if
         * we're live after the last instruction of the bundle */

        midgard_instruction *end = bundle->instructions[
                bundle->instruction_count - 1];

        if (mir_is_live_after(ctx, block, end, ins->ssa_args.dest))
                return false;

        /* We're only live in this bundle -- pipeline! */

        mir_rewrite_index(ctx, dest, SSA_FIXED_REGISTER(24 + pipeline_count));

        return true;
}

void
mir_create_pipeline_registers(compiler_context *ctx)
{
        mir_foreach_block(ctx, block) {
                mir_foreach_bundle_in_block(block, bundle) {
                        if (!mir_is_alu_bundle(bundle)) continue;
                        if (bundle->instruction_count < 2) continue;

                        /* Only first 2 instructions could pipeline */
                        bool succ = mir_pipeline_ins(ctx, block, bundle, 0, 0);
                        mir_pipeline_ins(ctx, block, bundle, 1, succ);
                }
        }
}
