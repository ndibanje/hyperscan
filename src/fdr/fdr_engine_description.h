/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FDR_ENGINE_DESCRIPTION_H
#define FDR_ENGINE_DESCRIPTION_H

#include "engine_description.h"
#include "util/ue2_containers.h"

#include <map>
#include <memory>
#include <vector>

namespace ue2 {

struct FDREngineDef {
    u32 id;
    u32 schemeWidth;
    u32 numBuckets;
    u32 stride;
    u64a cpu_features;
    u32 confirmPullBackDistance;
    u32 confirmTopLevelSplit;
};

class FDREngineDescription : public EngineDescription {
public:
    u32 schemeWidth;
    u32 stride;
    u32 bits;

    u32 getSchemeWidth() const { return schemeWidth; }
    u32 getBucketWidth(BucketIndex b) const;
    SchemeBitIndex getSchemeBit(BucketIndex b, PositionInBucket p) const;
    u32 getNumTableEntries() const { return 1 << bits; }
    u32 getTabSizeBytes() const {
        return schemeWidth / 8 * getNumTableEntries();
    }

    explicit FDREngineDescription(const FDREngineDef &def);

    u32 getDefaultFloodSuffixLength() const override;
    bool typicallyHoldsOneCharLits() const override { return stride == 1; }
};

std::unique_ptr<FDREngineDescription>
chooseEngine(const target_t &target, const std::vector<hwlmLiteral> &vl,
             bool make_small);
std::unique_ptr<FDREngineDescription> getFdrDescription(u32 engineID);
void getFdrDescriptions(std::vector<FDREngineDescription> *out);

} // namespace ue2

#endif
