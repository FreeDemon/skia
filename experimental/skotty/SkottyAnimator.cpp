/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkottyAnimator.h"

namespace skotty {

namespace {

SkScalar lerp_scalar(SkScalar v0, SkScalar v1, float t) {
    SkASSERT(t >= 0 && t <= 1);
    return v0 * (1 - t) + v1 * t;
}

} // namespace

template <>
void KeyframeInterval<ScalarValue>::lerp(float t, ScalarValue* v) const {
    *v = lerp_scalar(fV0, fV1, t);
}

template <>
void KeyframeInterval<VectorValue>::lerp(float t, VectorValue* v) const {
    SkASSERT(fV0.cardinality() == fV1.cardinality());
    SkASSERT(v->cardinality() == 0);

    v->fVals.reserve(fV0.cardinality());
    for (int i = 0; i < fV0.fVals.count(); ++i) {
        v->fVals.emplace_back(lerp_scalar(fV0.fVals[i], fV1.fVals[i], t));
    }
}

template <>
void KeyframeInterval<ShapeValue>::lerp(float t, ShapeValue* v) const {
    SkASSERT(fV0.cardinality() == fV1.cardinality());
    SkASSERT(v->cardinality() == 0);

    SkAssertResult(fV1.fPath.interpolate(fV0.fPath, t, &v->fPath));
}

float AnimatorBase::ComputeLocalT(float t, float t0, float t1,
                                  const SkPoint& c0, const SkPoint& c1) {
    SkASSERT(t1 > t0);
    auto lt = (t - t0) / (t1 - t0);

    if (c0 != SkPoint({0, 0}) || c1 != SkPoint({1, 1})) {
        // TODO: lt = CubicBezier(lt, c0, c1);
    }

    return SkTPin<float>(lt, 0, 1);
}

} // namespace skotty