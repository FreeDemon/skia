/*
 * Copyright 2017 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "Skottie.h"

#include "SkCanvas.h"
#include "SkottieAnimator.h"
#include "SkottiePriv.h"
#include "SkottieProperties.h"
#include "SkData.h"
#include "SkImage.h"
#include "SkMakeUnique.h"
#include "SkOSPath.h"
#include "SkPaint.h"
#include "SkParse.h"
#include "SkPath.h"
#include "SkPoint.h"
#include "SkSGColor.h"
#include "SkSGDraw.h"
#include "SkSGGeometryTransform.h"
#include "SkSGGradient.h"
#include "SkSGGroup.h"
#include "SkSGImage.h"
#include "SkSGInvalidationController.h"
#include "SkSGMaskEffect.h"
#include "SkSGMerge.h"
#include "SkSGOpacityEffect.h"
#include "SkSGPath.h"
#include "SkSGRect.h"
#include "SkSGScene.h"
#include "SkSGTransform.h"
#include "SkSGTrimEffect.h"
#include "SkStream.h"
#include "SkTArray.h"
#include "SkTHash.h"

#include <cmath>
#include <unordered_map>
#include <vector>

#include "stdlib.h"

namespace skottie {

namespace {

using AssetMap = SkTHashMap<SkString, const Json::Value*>;

struct AttachContext {
    const ResourceProvider&    fResources;
    const AssetMap&            fAssets;
    sksg::Scene::AnimatorList& fAnimators;
};

bool LogFail(const Json::Value& json, const char* msg) {
    const auto dump = json.toStyledString();
    LOG("!! %s: %s", msg, dump.c_str());
    return false;
}

// This is the workhorse for binding properties: depending on whether the property is animated,
// it will either apply immediately or instantiate and attach a keyframe animator.
template <typename T>
bool BindProperty(const Json::Value& jprop, AttachContext* ctx,
                  typename Animator<T>::ApplyFuncT&& apply) {
    if (!jprop.isObject())
        return false;

    const auto& jpropA = jprop["a"];
    const auto& jpropK = jprop["k"];

    // Older Json versions don't have an "a" animation marker.
    // For those, we attempt to parse both ways.
    if (jpropA.isNull() || !ParseBool(jpropA, "false")) {
        T val;
        if (ValueTraits<T>::Parse(jpropK, &val)) {
            // Static property.
            apply(val);
            return true;
        }

        if (!jpropA.isNull()) {
            return LogFail(jprop, "Could not parse (explicit) static property");
        }
    }

    // Keyframe property.
    auto animator = Animator<T>::Make(jpropK, std::move(apply));

    if (!animator) {
        return LogFail(jprop, "Could not parse keyframed property");
    }

    ctx->fAnimators.push_back(std::move(animator));

    return true;
}

sk_sp<sksg::Matrix> AttachMatrix(const Json::Value& t, AttachContext* ctx,
                                        sk_sp<sksg::Matrix> parentMatrix) {
    if (!t.isObject())
        return nullptr;

    auto matrix = sksg::Matrix::Make(SkMatrix::I(), std::move(parentMatrix));
    auto composite = sk_make_sp<CompositeTransform>(matrix);
    auto anchor_attached = BindProperty<VectorValue>(t["a"], ctx,
            [composite](const VectorValue& a) {
                composite->setAnchorPoint(ValueTraits<VectorValue>::As<SkPoint>(a));
            });
    auto position_attached = BindProperty<VectorValue>(t["p"], ctx,
            [composite](const VectorValue& p) {
                composite->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
            });
    auto scale_attached = BindProperty<VectorValue>(t["s"], ctx,
            [composite](const VectorValue& s) {
                composite->setScale(ValueTraits<VectorValue>::As<SkVector>(s));
            });
    auto rotation_attached = BindProperty<ScalarValue>(t["r"], ctx,
            [composite](const ScalarValue& r) {
                composite->setRotation(r);
            });
    auto skew_attached = BindProperty<ScalarValue>(t["sk"], ctx,
            [composite](const ScalarValue& sk) {
                composite->setSkew(sk);
            });
    auto skewaxis_attached = BindProperty<ScalarValue>(t["sa"], ctx,
            [composite](const ScalarValue& sa) {
                composite->setSkewAxis(sa);
            });

    if (!anchor_attached &&
        !position_attached &&
        !scale_attached &&
        !rotation_attached &&
        !skew_attached &&
        !skewaxis_attached) {
        LogFail(t, "Could not parse transform");
        return nullptr;
    }

    return matrix;
}

sk_sp<sksg::RenderNode> AttachOpacity(const Json::Value& jtransform, AttachContext* ctx,
                                      sk_sp<sksg::RenderNode> childNode) {
    if (!jtransform.isObject() || !childNode)
        return childNode;

    // This is more peeky than other attachers, because we want to avoid redundant opacity
    // nodes for the extremely common case of static opaciy == 100.
    const auto& opacity = jtransform["o"];
    if (opacity.isObject() &&
        !ParseBool(opacity["a"], true) &&
        ParseScalar(opacity["k"], -1) == 100) {
        // Ignoring static full opacity.
        return childNode;
    }

    auto opacityNode = sksg::OpacityEffect::Make(childNode);
    BindProperty<ScalarValue>(opacity, ctx,
        [opacityNode](const ScalarValue& o) {
            // BM opacity is [0..100]
            opacityNode->setOpacity(o * 0.01f);
        });

    return opacityNode;
}

sk_sp<sksg::RenderNode> AttachComposition(const Json::Value&, AttachContext* ctx);

sk_sp<sksg::Path> AttachPath(const Json::Value& jpath, AttachContext* ctx) {
    auto path_node = sksg::Path::Make();
    return BindProperty<ShapeValue>(jpath, ctx,
                                    [path_node](const ShapeValue& p) { path_node->setPath(p); })
        ? path_node
        : nullptr;
}

sk_sp<sksg::GeometryNode> AttachPathGeometry(const Json::Value& jpath, AttachContext* ctx) {
    SkASSERT(jpath.isObject());

    return AttachPath(jpath["ks"], ctx);
}

sk_sp<sksg::GeometryNode> AttachRRectGeometry(const Json::Value& jrect, AttachContext* ctx) {
    SkASSERT(jrect.isObject());

    auto rect_node = sksg::RRect::Make();
    auto composite = sk_make_sp<CompositeRRect>(rect_node);

    auto p_attached = BindProperty<VectorValue>(jrect["p"], ctx,
        [composite](const VectorValue& p) {
                composite->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
        });
    auto s_attached = BindProperty<VectorValue>(jrect["s"], ctx,
        [composite](const VectorValue& s) {
            composite->setSize(ValueTraits<VectorValue>::As<SkSize>(s));
        });
    auto r_attached = BindProperty<ScalarValue>(jrect["r"], ctx,
        [composite](const ScalarValue& r) {
            composite->setRadius(SkSize::Make(r, r));
        });

    if (!p_attached && !s_attached && !r_attached) {
        return nullptr;
    }

    LOG("** Attached (r)rect geometry\n");

    return rect_node;
}

sk_sp<sksg::GeometryNode> AttachEllipseGeometry(const Json::Value& jellipse, AttachContext* ctx) {
    SkASSERT(jellipse.isObject());

    auto rect_node = sksg::RRect::Make();
    auto composite = sk_make_sp<CompositeRRect>(rect_node);

    auto p_attached = BindProperty<VectorValue>(jellipse["p"], ctx,
        [composite](const VectorValue& p) {
            composite->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
        });
    auto s_attached = BindProperty<VectorValue>(jellipse["s"], ctx,
        [composite](const VectorValue& s) {
            const auto sz = ValueTraits<VectorValue>::As<SkSize>(s);
            composite->setSize(sz);
            composite->setRadius(SkSize::Make(sz.width() / 2, sz.height() / 2));
        });

    if (!p_attached && !s_attached) {
        return nullptr;
    }

    LOG("** Attached ellipse geometry\n");

    return rect_node;
}

sk_sp<sksg::GeometryNode> AttachPolystarGeometry(const Json::Value& jstar, AttachContext* ctx) {
    SkASSERT(jstar.isObject());

    static constexpr CompositePolyStar::Type gTypes[] = {
        CompositePolyStar::Type::kStar, // "sy": 1
        CompositePolyStar::Type::kPoly, // "sy": 2
    };

    const auto type = ParseInt(jstar["sy"], 0) - 1;
    if (type < 0 || type >= SkTo<int>(SK_ARRAY_COUNT(gTypes))) {
        LogFail(jstar, "Unknown polystar type");
        return nullptr;
    }

    auto path_node = sksg::Path::Make();
    auto composite = sk_make_sp<CompositePolyStar>(path_node, gTypes[type]);

    BindProperty<VectorValue>(jstar["p"], ctx,
        [composite](const VectorValue& p) {
            composite->setPosition(ValueTraits<VectorValue>::As<SkPoint>(p));
        });
    BindProperty<ScalarValue>(jstar["pt"], ctx,
        [composite](const ScalarValue& pt) {
            composite->setPointCount(pt);
        });
    BindProperty<ScalarValue>(jstar["ir"], ctx,
        [composite](const ScalarValue& ir) {
            composite->setInnerRadius(ir);
        });
    BindProperty<ScalarValue>(jstar["or"], ctx,
        [composite](const ScalarValue& otr) {
            composite->setOuterRadius(otr);
        });
    BindProperty<ScalarValue>(jstar["is"], ctx,
        [composite](const ScalarValue& is) {
            composite->setInnerRoundness(is);
        });
    BindProperty<ScalarValue>(jstar["os"], ctx,
        [composite](const ScalarValue& os) {
            composite->setOuterRoundness(os);
        });
    BindProperty<ScalarValue>(jstar["r"], ctx,
        [composite](const ScalarValue& r) {
            composite->setRotation(r);
        });

    return path_node;
}

sk_sp<sksg::Color> AttachColor(const Json::Value& obj, AttachContext* ctx) {
    SkASSERT(obj.isObject());

    auto color_node = sksg::Color::Make(SK_ColorBLACK);
    auto color_attached = BindProperty<VectorValue>(obj["c"], ctx,
        [color_node](const VectorValue& c) {
            color_node->setColor(ValueTraits<VectorValue>::As<SkColor>(c));
        });

    return color_attached ? color_node : nullptr;
}

sk_sp<sksg::Gradient> AttachGradient(const Json::Value& obj, AttachContext* ctx) {
    SkASSERT(obj.isObject());

    const auto& stops = obj["g"];
    if (!stops.isObject())
        return nullptr;

    const auto stopCount = ParseInt(stops["p"], -1);
    if (stopCount < 0)
        return nullptr;

    sk_sp<sksg::Gradient> gradient_node;
    sk_sp<CompositeGradient> composite;

    if (ParseInt(obj["t"], 1) == 1) {
        auto linear_node = sksg::LinearGradient::Make();
        composite = sk_make_sp<CompositeLinearGradient>(linear_node, stopCount);
        gradient_node = std::move(linear_node);
    } else {
        auto radial_node = sksg::RadialGradient::Make();
        composite = sk_make_sp<CompositeRadialGradient>(radial_node, stopCount);

        // TODO: highlight, angle
        gradient_node = std::move(radial_node);
    }

    BindProperty<VectorValue>(stops["k"], ctx,
        [composite](const VectorValue& stops) {
            composite->setColorStops(stops);
        });
    BindProperty<VectorValue>(obj["s"], ctx,
        [composite](const VectorValue& s) {
            composite->setStartPoint(ValueTraits<VectorValue>::As<SkPoint>(s));
        });
    BindProperty<VectorValue>(obj["e"], ctx,
        [composite](const VectorValue& e) {
            composite->setEndPoint(ValueTraits<VectorValue>::As<SkPoint>(e));
        });

    return gradient_node;
}

sk_sp<sksg::PaintNode> AttachPaint(const Json::Value& jpaint, AttachContext* ctx,
                                   sk_sp<sksg::PaintNode> paint_node) {
    if (paint_node) {
        paint_node->setAntiAlias(true);

        BindProperty<ScalarValue>(jpaint["o"], ctx,
            [paint_node](const ScalarValue& o) {
                // BM opacity is [0..100]
                paint_node->setOpacity(o * 0.01f);
        });
    }

    return paint_node;
}

sk_sp<sksg::PaintNode> AttachStroke(const Json::Value& jstroke, AttachContext* ctx,
                                    sk_sp<sksg::PaintNode> stroke_node) {
    SkASSERT(jstroke.isObject());

    if (!stroke_node)
        return nullptr;

    stroke_node->setStyle(SkPaint::kStroke_Style);

    auto width_attached = BindProperty<ScalarValue>(jstroke["w"], ctx,
        [stroke_node](const ScalarValue& w) {
            stroke_node->setStrokeWidth(w);
        });
    if (!width_attached)
        return nullptr;

    stroke_node->setStrokeMiter(ParseScalar(jstroke["ml"], 4));

    static constexpr SkPaint::Join gJoins[] = {
        SkPaint::kMiter_Join,
        SkPaint::kRound_Join,
        SkPaint::kBevel_Join,
    };
    stroke_node->setStrokeJoin(gJoins[SkTPin<int>(ParseInt(jstroke["lj"], 1) - 1,
                                                  0, SK_ARRAY_COUNT(gJoins) - 1)]);

    static constexpr SkPaint::Cap gCaps[] = {
        SkPaint::kButt_Cap,
        SkPaint::kRound_Cap,
        SkPaint::kSquare_Cap,
    };
    stroke_node->setStrokeCap(gCaps[SkTPin<int>(ParseInt(jstroke["lc"], 1) - 1,
                                                0, SK_ARRAY_COUNT(gCaps) - 1)]);

    return stroke_node;
}

sk_sp<sksg::PaintNode> AttachColorFill(const Json::Value& jfill, AttachContext* ctx) {
    SkASSERT(jfill.isObject());

    return AttachPaint(jfill, ctx, AttachColor(jfill, ctx));
}

sk_sp<sksg::PaintNode> AttachGradientFill(const Json::Value& jfill, AttachContext* ctx) {
    SkASSERT(jfill.isObject());

    return AttachPaint(jfill, ctx, AttachGradient(jfill, ctx));
}

sk_sp<sksg::PaintNode> AttachColorStroke(const Json::Value& jstroke, AttachContext* ctx) {
    SkASSERT(jstroke.isObject());

    return AttachStroke(jstroke, ctx, AttachPaint(jstroke, ctx, AttachColor(jstroke, ctx)));
}

sk_sp<sksg::PaintNode> AttachGradientStroke(const Json::Value& jstroke, AttachContext* ctx) {
    SkASSERT(jstroke.isObject());

    return AttachStroke(jstroke, ctx, AttachPaint(jstroke, ctx, AttachGradient(jstroke, ctx)));
}

std::vector<sk_sp<sksg::GeometryNode>> AttachMergeGeometryEffect(
    const Json::Value& jmerge, AttachContext* ctx, std::vector<sk_sp<sksg::GeometryNode>>&& geos) {
    std::vector<sk_sp<sksg::GeometryNode>> merged;

    static constexpr sksg::Merge::Mode gModes[] = {
        sksg::Merge::Mode::kMerge,      // "mm": 1
        sksg::Merge::Mode::kUnion,      // "mm": 2
        sksg::Merge::Mode::kDifference, // "mm": 3
        sksg::Merge::Mode::kIntersect,  // "mm": 4
        sksg::Merge::Mode::kXOR      ,  // "mm": 5
    };

    const auto mode = gModes[SkTPin<int>(ParseInt(jmerge["mm"], 1) - 1,
                                         0, SK_ARRAY_COUNT(gModes) - 1)];
    merged.push_back(sksg::Merge::Make(std::move(geos), mode));

    LOG("** Attached merge path effect, mode: %d\n", mode);

    return merged;
}

std::vector<sk_sp<sksg::GeometryNode>> AttachTrimGeometryEffect(
    const Json::Value& jtrim, AttachContext* ctx, std::vector<sk_sp<sksg::GeometryNode>>&& geos) {

    enum class Mode {
        kMerged,   // "m": 1
        kSeparate, // "m": 2
    } gModes[] = { Mode::kMerged, Mode::kSeparate };

    const auto mode = gModes[SkTPin<int>(ParseInt(jtrim["m"], 1) - 1,
                                         0, SK_ARRAY_COUNT(gModes) - 1)];

    std::vector<sk_sp<sksg::GeometryNode>> inputs;
    if (mode == Mode::kMerged) {
        inputs.push_back(sksg::Merge::Make(std::move(geos), sksg::Merge::Mode::kMerge));
    } else {
        inputs = std::move(geos);
    }

    std::vector<sk_sp<sksg::GeometryNode>> trimmed;
    trimmed.reserve(inputs.size());
    for (const auto& i : inputs) {
        const auto trim = sksg::TrimEffect::Make(i);
        trimmed.push_back(trim);
        BindProperty<ScalarValue>(jtrim["s"], ctx,
            [trim](const ScalarValue& s) {
                trim->setStart(s * 0.01f);
            });
        BindProperty<ScalarValue>(jtrim["e"], ctx,
            [trim](const ScalarValue& e) {
                trim->setEnd(e * 0.01f);
            });
        BindProperty<ScalarValue>(jtrim["o"], ctx,
            [trim](const ScalarValue& o) {
                trim->setOffset(o / 360);
            });
    }

    return trimmed;
}

using GeometryAttacherT = sk_sp<sksg::GeometryNode> (*)(const Json::Value&, AttachContext*);
static constexpr GeometryAttacherT gGeometryAttachers[] = {
    AttachPathGeometry,
    AttachRRectGeometry,
    AttachEllipseGeometry,
    AttachPolystarGeometry,
};

using PaintAttacherT = sk_sp<sksg::PaintNode> (*)(const Json::Value&, AttachContext*);
static constexpr PaintAttacherT gPaintAttachers[] = {
    AttachColorFill,
    AttachColorStroke,
    AttachGradientFill,
    AttachGradientStroke,
};

using GeometryEffectAttacherT =
    std::vector<sk_sp<sksg::GeometryNode>> (*)(const Json::Value&,
                                               AttachContext*,
                                               std::vector<sk_sp<sksg::GeometryNode>>&&);
static constexpr GeometryEffectAttacherT gGeometryEffectAttachers[] = {
    AttachMergeGeometryEffect,
    AttachTrimGeometryEffect,
};

enum class ShapeType {
    kGeometry,
    kGeometryEffect,
    kPaint,
    kGroup,
    kTransform,
};

struct ShapeInfo {
    const char* fTypeString;
    ShapeType   fShapeType;
    uint32_t    fAttacherIndex; // index into respective attacher tables
};

const ShapeInfo* FindShapeInfo(const Json::Value& shape) {
    static constexpr ShapeInfo gShapeInfo[] = {
        { "el", ShapeType::kGeometry      , 2 }, // ellipse   -> AttachEllipseGeometry
        { "fl", ShapeType::kPaint         , 0 }, // fill      -> AttachColorFill
        { "gf", ShapeType::kPaint         , 2 }, // gfill     -> AttachGradientFill
        { "gr", ShapeType::kGroup         , 0 }, // group     -> Inline handler
        { "gs", ShapeType::kPaint         , 3 }, // gstroke   -> AttachGradientStroke
        { "mm", ShapeType::kGeometryEffect, 0 }, // merge     -> AttachMergeGeometryEffect
        { "rc", ShapeType::kGeometry      , 1 }, // rrect     -> AttachRRectGeometry
        { "sh", ShapeType::kGeometry      , 0 }, // shape     -> AttachPathGeometry
        { "sr", ShapeType::kGeometry      , 3 }, // polystar  -> AttachPolyStarGeometry
        { "st", ShapeType::kPaint         , 1 }, // stroke    -> AttachColorStroke
        { "tm", ShapeType::kGeometryEffect, 1 }, // trim      -> AttachTrimGeometryEffect
        { "tr", ShapeType::kTransform     , 0 }, // transform -> Inline handler
    };

    if (!shape.isObject())
        return nullptr;

    const auto& type = shape["ty"];
    if (!type.isString())
        return nullptr;

    const auto* info = bsearch(type.asCString(),
                               gShapeInfo,
                               SK_ARRAY_COUNT(gShapeInfo),
                               sizeof(ShapeInfo),
                               [](const void* key, const void* info) {
                                  return strcmp(static_cast<const char*>(key),
                                                static_cast<const ShapeInfo*>(info)->fTypeString);
                               });

    return static_cast<const ShapeInfo*>(info);
}

struct GeometryEffectRec {
    const Json::Value&      fJson;
    GeometryEffectAttacherT fAttach;
};

struct AttachShapeContext {
    AttachShapeContext(AttachContext* ctx,
                       std::vector<sk_sp<sksg::GeometryNode>>* geos,
                       std::vector<GeometryEffectRec>* effects,
                       size_t committedAnimators)
        : fCtx(ctx)
        , fGeometryStack(geos)
        , fGeometryEffectStack(effects)
        , fCommittedAnimators(committedAnimators) {}

    AttachContext*                          fCtx;
    std::vector<sk_sp<sksg::GeometryNode>>* fGeometryStack;
    std::vector<GeometryEffectRec>*         fGeometryEffectStack;
    size_t                                  fCommittedAnimators;
};

sk_sp<sksg::RenderNode> AttachShape(const Json::Value& jshape, AttachShapeContext* shapeCtx) {
    if (!jshape.isArray())
        return nullptr;

    SkDEBUGCODE(const auto initialGeometryEffects = shapeCtx->fGeometryEffectStack->size();)

    sk_sp<sksg::Group> shape_group = sksg::Group::Make();
    sk_sp<sksg::RenderNode> shape_wrapper = shape_group;
    sk_sp<sksg::Matrix> shape_matrix;

    struct ShapeRec {
        const Json::Value& fJson;
        const ShapeInfo&   fInfo;
    };

    // First pass (bottom->top):
    //
    //   * pick up the group transform and opacity
    //   * push local geometry effects onto the stack
    //   * store recs for next pass
    //
    std::vector<ShapeRec> recs;
    for (Json::ArrayIndex i = 0; i < jshape.size(); ++i) {
        const auto& s = jshape[jshape.size() - 1 - i];
        const auto* info = FindShapeInfo(s);
        if (!info) {
            LogFail(s.isObject() ? s["ty"] : s, "Unknown shape");
            continue;
        }

        recs.push_back({ s, *info });

        switch (info->fShapeType) {
        case ShapeType::kTransform:
            if ((shape_matrix = AttachMatrix(s, shapeCtx->fCtx, nullptr))) {
                shape_wrapper = sksg::Transform::Make(std::move(shape_wrapper), shape_matrix);
            }
            shape_wrapper = AttachOpacity(s, shapeCtx->fCtx, std::move(shape_wrapper));
            break;
        case ShapeType::kGeometryEffect:
            SkASSERT(info->fAttacherIndex < SK_ARRAY_COUNT(gGeometryEffectAttachers));
            shapeCtx->fGeometryEffectStack->push_back(
                { s, gGeometryEffectAttachers[info->fAttacherIndex] });
            break;
        default:
            break;
        }
    }

    // Second pass (top -> bottom, after 2x reverse):
    //
    //   * track local geometry
    //   * emit local paints
    //
    std::vector<sk_sp<sksg::GeometryNode>> geos;
    std::vector<sk_sp<sksg::RenderNode  >> draws;
    for (auto rec = recs.rbegin(); rec != recs.rend(); ++rec) {
        switch (rec->fInfo.fShapeType) {
        case ShapeType::kGeometry: {
            SkASSERT(rec->fInfo.fAttacherIndex < SK_ARRAY_COUNT(gGeometryAttachers));
            if (auto geo = gGeometryAttachers[rec->fInfo.fAttacherIndex](rec->fJson,
                                                                         shapeCtx->fCtx)) {
                geos.push_back(std::move(geo));
            }
        } break;
        case ShapeType::kGeometryEffect: {
            // Apply the current effect and pop from the stack.
            SkASSERT(rec->fInfo.fAttacherIndex < SK_ARRAY_COUNT(gGeometryEffectAttachers));
            if (!geos.empty()) {
                geos = gGeometryEffectAttachers[rec->fInfo.fAttacherIndex](rec->fJson,
                                                                           shapeCtx->fCtx,
                                                                           std::move(geos));
            }

            SkASSERT(shapeCtx->fGeometryEffectStack->back().fJson == rec->fJson);
            SkASSERT(shapeCtx->fGeometryEffectStack->back().fAttach ==
                     gGeometryEffectAttachers[rec->fInfo.fAttacherIndex]);
            shapeCtx->fGeometryEffectStack->pop_back();
        } break;
        case ShapeType::kGroup: {
            AttachShapeContext groupShapeCtx(shapeCtx->fCtx,
                                             &geos,
                                             shapeCtx->fGeometryEffectStack,
                                             shapeCtx->fCommittedAnimators);
            if (auto subgroup = AttachShape(rec->fJson["it"], &groupShapeCtx)) {
                draws.push_back(std::move(subgroup));
                SkASSERT(groupShapeCtx.fCommittedAnimators >= shapeCtx->fCommittedAnimators);
                shapeCtx->fCommittedAnimators = groupShapeCtx.fCommittedAnimators;
            }
        } break;
        case ShapeType::kPaint: {
            SkASSERT(rec->fInfo.fAttacherIndex < SK_ARRAY_COUNT(gPaintAttachers));
            auto paint = gPaintAttachers[rec->fInfo.fAttacherIndex](rec->fJson, shapeCtx->fCtx);
            if (!paint || geos.empty())
                break;

            auto drawGeos = geos;

            // Apply all pending effects from the stack.
            for (auto it = shapeCtx->fGeometryEffectStack->rbegin();
                 it != shapeCtx->fGeometryEffectStack->rend(); ++it) {
                drawGeos = it->fAttach(it->fJson, shapeCtx->fCtx, std::move(drawGeos));
            }

            // If we still have multiple geos, reduce using 'merge'.
            auto geo = drawGeos.size() > 1
                ? sksg::Merge::Make(std::move(drawGeos), sksg::Merge::Mode::kMerge)
                : drawGeos[0];

            SkASSERT(geo);
            draws.push_back(sksg::Draw::Make(std::move(geo), std::move(paint)));
            shapeCtx->fCommittedAnimators = shapeCtx->fCtx->fAnimators.size();
        } break;
        default:
            break;
        }
    }

    // By now we should have popped all local geometry effects.
    SkASSERT(shapeCtx->fGeometryEffectStack->size() == initialGeometryEffects);

    // Push transformed local geometries to parent list, for subsequent paints.
    for (const auto& geo : geos) {
        shapeCtx->fGeometryStack->push_back(shape_matrix
            ? sksg::GeometryTransform::Make(std::move(geo), shape_matrix)
            : std::move(geo));
    }

    // Emit local draws reversed (bottom->top, per spec).
    for (auto it = draws.rbegin(); it != draws.rend(); ++it) {
        shape_group->addChild(std::move(*it));
    }

    return draws.empty() ? nullptr : shape_wrapper;
}

sk_sp<sksg::RenderNode> AttachCompLayer(const Json::Value& layer, AttachContext* ctx) {
    SkASSERT(layer.isObject());

    auto refId = ParseString(layer["refId"], "");
    if (refId.isEmpty()) {
        LOG("!! Comp layer missing refId\n");
        return nullptr;
    }

    const auto* comp = ctx->fAssets.find(refId);
    if (!comp) {
        LOG("!! Pre-comp not found: '%s'\n", refId.c_str());
        return nullptr;
    }

    // TODO: cycle detection
    return AttachComposition(**comp, ctx);
}

sk_sp<sksg::RenderNode> AttachSolidLayer(const Json::Value& jlayer, AttachContext*) {
    SkASSERT(jlayer.isObject());

    const auto size = SkSize::Make(ParseScalar(jlayer["sw"], -1),
                                   ParseScalar(jlayer["sh"], -1));
    const auto hex = ParseString(jlayer["sc"], "");
    uint32_t c;
    if (size.isEmpty() ||
        !hex.startsWith("#") ||
        !SkParse::FindHex(hex.c_str() + 1, &c)) {
        LogFail(jlayer, "Could not parse solid layer");
        return nullptr;
    }

    const SkColor color = 0xff000000 | c;

    return sksg::Draw::Make(sksg::Rect::Make(SkRect::MakeSize(size)),
                            sksg::Color::Make(color));
}

sk_sp<sksg::RenderNode> AttachImageAsset(const Json::Value& jimage, AttachContext* ctx) {
    SkASSERT(jimage.isObject());

    const auto name = ParseString(jimage["p"], ""),
               path = ParseString(jimage["u"], "");
    if (name.isEmpty())
        return nullptr;

    // TODO: plumb resource paths explicitly to ResourceProvider?
    const auto resName    = path.isEmpty() ? name : SkOSPath::Join(path.c_str(), name.c_str());
    const auto resStream  = ctx->fResources.openStream(resName.c_str());
    if (!resStream || !resStream->hasLength()) {
        LOG("!! Could not load image resource: %s\n", resName.c_str());
        return nullptr;
    }

    // TODO: non-intrisic image sizing
    return sksg::Image::Make(
        SkImage::MakeFromEncoded(SkData::MakeFromStream(resStream.get(), resStream->getLength())));
}

sk_sp<sksg::RenderNode> AttachImageLayer(const Json::Value& layer, AttachContext* ctx) {
    SkASSERT(layer.isObject());

    auto refId = ParseString(layer["refId"], "");
    if (refId.isEmpty()) {
        LOG("!! Image layer missing refId\n");
        return nullptr;
    }

    const auto* jimage = ctx->fAssets.find(refId);
    if (!jimage) {
        LOG("!! Image asset not found: '%s'\n", refId.c_str());
        return nullptr;
    }

    return AttachImageAsset(**jimage, ctx);
}

sk_sp<sksg::RenderNode> AttachNullLayer(const Json::Value& layer, AttachContext*) {
    SkASSERT(layer.isObject());

    // Null layers are used solely to drive dependent transforms,
    // but we use free-floating sksg::Matrices for that purpose.
    return nullptr;
}

sk_sp<sksg::RenderNode> AttachShapeLayer(const Json::Value& layer, AttachContext* ctx) {
    SkASSERT(layer.isObject());

    LOG("** Attaching shape layer ind: %d\n", ParseInt(layer["ind"], 0));

    std::vector<sk_sp<sksg::GeometryNode>> geometryStack;
    std::vector<GeometryEffectRec> geometryEffectStack;
    AttachShapeContext shapeCtx(ctx, &geometryStack, &geometryEffectStack, ctx->fAnimators.size());
    auto shapeNode = AttachShape(layer["shapes"], &shapeCtx);

    // Trim uncommitted animators: AttachShape consumes effects on the fly, and greedily attaches
    // geometries => at the end, we can end up with unused geometries, which are nevertheless alive
    // due to attached animators.  To avoid this, we track committed animators and discard the
    // orphans here.
    SkASSERT(shapeCtx.fCommittedAnimators <= ctx->fAnimators.size());
    ctx->fAnimators.resize(shapeCtx.fCommittedAnimators);

    return shapeNode;
}

sk_sp<sksg::RenderNode> AttachTextLayer(const Json::Value& layer, AttachContext*) {
    SkASSERT(layer.isObject());

    LOG("?? Text layer stub\n");
    return nullptr;
}

struct AttachLayerContext {
    AttachLayerContext(const Json::Value& jlayers, AttachContext* ctx)
        : fLayerList(jlayers), fCtx(ctx) {}

    const Json::Value&                                          fLayerList;
    AttachContext*                                              fCtx;
    std::unordered_map<const Json::Value*, sk_sp<sksg::Matrix>> fLayerMatrixCache;
    std::unordered_map<int, const Json::Value*>                 fLayerIndexCache;
    sk_sp<sksg::RenderNode>                                     fCurrentMatte;

    const Json::Value* findLayer(int index) {
        SkASSERT(fLayerList.isArray());

        if (index < 0) {
            return nullptr;
        }

        const auto cached = fLayerIndexCache.find(index);
        if (cached != fLayerIndexCache.end()) {
            return cached->second;
        }

        for (const auto& l : fLayerList) {
            if (!l.isObject()) {
                continue;
            }

            if (ParseInt(l["ind"], -1) == index) {
                fLayerIndexCache.insert(std::make_pair(index, &l));
                return &l;
            }
        }

        return nullptr;
    }

    sk_sp<sksg::Matrix> AttachLayerMatrix(const Json::Value& jlayer) {
        SkASSERT(jlayer.isObject());

        const auto cached = fLayerMatrixCache.find(&jlayer);
        if (cached != fLayerMatrixCache.end()) {
            return cached->second;
        }

        const auto* parentLayer = this->findLayer(ParseInt(jlayer["parent"], -1));

        // TODO: cycle detection?
        auto parentMatrix = (parentLayer && parentLayer != &jlayer)
            ? this->AttachLayerMatrix(*parentLayer) : nullptr;

        auto layerMatrix = AttachMatrix(jlayer["ks"], fCtx, std::move(parentMatrix));
        fLayerMatrixCache.insert(std::make_pair(&jlayer, layerMatrix));

        return layerMatrix;
    }
};

SkBlendMode MaskBlendMode(char mode) {
    switch (mode) {
    case 'a': return SkBlendMode::kSrcOver;    // Additive
    case 's': return SkBlendMode::kExclusion;  // Subtract
    case 'i': return SkBlendMode::kDstIn;      // Intersect
    case 'l': return SkBlendMode::kLighten;    // Lighten
    case 'd': return SkBlendMode::kDarken;     // Darken
    case 'f': return SkBlendMode::kDifference; // Difference
    default: break;
    }

    return SkBlendMode::kSrcOver;
}

sk_sp<sksg::RenderNode> AttachMask(const Json::Value& jmask,
                                   AttachContext* ctx,
                                   sk_sp<sksg::RenderNode> childNode) {
    if (!jmask.isArray())
        return childNode;

    auto mask_group = sksg::Group::Make();

    for (const auto& m : jmask) {
        if (!m.isObject())
            continue;

        const auto inverted = ParseBool(m["inv"], false);
        // TODO
        if (inverted) {
            LogFail(m, "Unsupported inverse mask");
            continue;
        }

        auto mask_path = AttachPath(m["pt"], ctx);
        if (!mask_path) {
            LogFail(m, "Could not parse mask path");
            continue;
        }

        auto mode = ParseString(m["mode"], "");
        if (mode.size() != 1 || !strcmp(mode.c_str(), "n")) // "None" masks have no effect.
            continue;

        auto mask_paint = sksg::Color::Make(SK_ColorBLACK);
        mask_paint->setBlendMode(MaskBlendMode(mode.c_str()[0]));
        BindProperty<ScalarValue>(m["o"], ctx,
            [mask_paint](const ScalarValue& o) { mask_paint->setOpacity(o * 0.01f); });

        mask_group->addChild(sksg::Draw::Make(std::move(mask_path), std::move(mask_paint)));
    }

    return mask_group->empty()
        ? childNode
        : sksg::MaskEffect::Make(std::move(childNode), std::move(mask_group));
}

sk_sp<sksg::RenderNode> AttachLayer(const Json::Value& jlayer,
                                    AttachLayerContext* layerCtx) {
    if (!jlayer.isObject())
        return nullptr;

    using LayerAttacher = sk_sp<sksg::RenderNode> (*)(const Json::Value&, AttachContext*);
    static constexpr LayerAttacher gLayerAttachers[] = {
        AttachCompLayer,  // 'ty': 0
        AttachSolidLayer, // 'ty': 1
        AttachImageLayer, // 'ty': 2
        AttachNullLayer,  // 'ty': 3
        AttachShapeLayer, // 'ty': 4
        AttachTextLayer,  // 'ty': 5
    };

    int type = ParseInt(jlayer["ty"], -1);
    if (type < 0 || type >= SkTo<int>(SK_ARRAY_COUNT(gLayerAttachers))) {
        return nullptr;
    }

    // Layer content.
    auto layer = gLayerAttachers[type](jlayer, layerCtx->fCtx);
    // Optional layer mask.
    layer = AttachMask(jlayer["masksProperties"], layerCtx->fCtx, std::move(layer));
    // Optional layer transform.
    if (auto layerMatrix = layerCtx->AttachLayerMatrix(jlayer)) {
        layer = sksg::Transform::Make(std::move(layer), std::move(layerMatrix));
    }
    // Optional layer opacity.
    layer = AttachOpacity(jlayer["ks"], layerCtx->fCtx, std::move(layer));

    // TODO: we should also disable related/inactive animators.
    class Activator final : public sksg::Animator {
    public:
        Activator(sk_sp<sksg::OpacityEffect> controlNode, float in, float out)
            : fControlNode(std::move(controlNode))
            , fIn(in)
            , fOut(out) {}

        void onTick(float t) override {
            // Keep the layer fully transparent except for its [in..out] lifespan.
            // (note: opacity == 0 disables rendering, while opacity == 1 is a noop)
            fControlNode->setOpacity(t >= fIn && t <= fOut ? 1 : 0);
        }

    private:
        const sk_sp<sksg::OpacityEffect> fControlNode;
        const float                      fIn,
                                         fOut;
    };

    auto layerControl = sksg::OpacityEffect::Make(std::move(layer));
    const auto  in = ParseScalar(jlayer["ip"], 0),
               out = ParseScalar(jlayer["op"], in);

    if (in >= out || ! layerControl)
        return nullptr;

    layerCtx->fCtx->fAnimators.push_back(skstd::make_unique<Activator>(layerControl, in, out));

    if (ParseBool(jlayer["td"], false)) {
        // This layer is a matte.  We apply it as a mask to the next layer.
        layerCtx->fCurrentMatte = std::move(layerControl);
        return nullptr;
    }

    if (layerCtx->fCurrentMatte) {
        // There is a pending matte. Apply and reset.
        return sksg::MaskEffect::Make(std::move(layerControl), std::move(layerCtx->fCurrentMatte));
    }

    return layerControl;
}

sk_sp<sksg::RenderNode> AttachComposition(const Json::Value& comp, AttachContext* ctx) {
    if (!comp.isObject())
        return nullptr;

    const auto& jlayers = comp["layers"];
    if (!jlayers.isArray())
        return nullptr;

    SkSTArray<16, sk_sp<sksg::RenderNode>, true> layers;
    AttachLayerContext                           layerCtx(jlayers, ctx);

    for (const auto& l : jlayers) {
        if (auto layer_fragment = AttachLayer(l, &layerCtx)) {
            layers.push_back(std::move(layer_fragment));
        }
    }

    if (layers.empty()) {
        return nullptr;
    }

    // Layers are painted in bottom->top order.
    auto comp_group = sksg::Group::Make();
    for (int i = layers.count() - 1; i >= 0; --i) {
        comp_group->addChild(std::move(layers[i]));
    }

    LOG("** Attached composition '%s': %d layers.\n",
        ParseString(comp["id"], "").c_str(), layers.count());

    return comp_group;
}

} // namespace

std::unique_ptr<Animation> Animation::Make(SkStream* stream, const ResourceProvider& res) {
    if (!stream->hasLength()) {
        // TODO: handle explicit buffering?
        LOG("!! cannot parse streaming content\n");
        return nullptr;
    }

    Json::Value json;
    {
        auto data = SkData::MakeFromStream(stream, stream->getLength());
        if (!data) {
            LOG("!! could not read stream\n");
            return nullptr;
        }

        Json::Reader reader;

        auto dataStart = static_cast<const char*>(data->data());
        if (!reader.parse(dataStart, dataStart + data->size(), json, false) || !json.isObject()) {
            LOG("!! failed to parse json: %s\n", reader.getFormattedErrorMessages().c_str());
            return nullptr;
        }
    }

    const auto version = ParseString(json["v"], "");
    const auto size    = SkSize::Make(ParseScalar(json["w"], -1), ParseScalar(json["h"], -1));
    const auto fps     = ParseScalar(json["fr"], -1);

    if (size.isEmpty() || version.isEmpty() || fps < 0) {
        LOG("!! invalid animation params (version: %s, size: [%f %f], frame rate: %f)",
            version.c_str(), size.width(), size.height(), fps);
        return nullptr;
    }

    return std::unique_ptr<Animation>(new Animation(res, std::move(version), size, fps, json));
}

std::unique_ptr<Animation> Animation::MakeFromFile(const char path[], const ResourceProvider* res) {
    class DirectoryResourceProvider final : public ResourceProvider {
    public:
        explicit DirectoryResourceProvider(SkString dir) : fDir(std::move(dir)) {}

        std::unique_ptr<SkStream> openStream(const char resource[]) const override {
            const auto resPath = SkOSPath::Join(fDir.c_str(), resource);
            return SkStream::MakeFromFile(resPath.c_str());
        }

    private:
        const SkString fDir;
    };

    const auto jsonStream =  SkStream::MakeFromFile(path);
    if (!jsonStream)
        return nullptr;

    std::unique_ptr<ResourceProvider> defaultProvider;
    if (!res) {
        defaultProvider = skstd::make_unique<DirectoryResourceProvider>(SkOSPath::Dirname(path));
    }

    return Make(jsonStream.get(), res ? *res : *defaultProvider);
}

Animation::Animation(const ResourceProvider& resources,
                     SkString version, const SkSize& size, SkScalar fps, const Json::Value& json)
    : fVersion(std::move(version))
    , fSize(size)
    , fFrameRate(fps)
    , fInPoint(ParseScalar(json["ip"], 0))
    , fOutPoint(SkTMax(ParseScalar(json["op"], SK_ScalarMax), fInPoint)) {

    AssetMap assets;
    for (const auto& asset : json["assets"]) {
        if (!asset.isObject()) {
            continue;
        }

        assets.set(ParseString(asset["id"], ""), &asset);
    }

    sksg::Scene::AnimatorList animators;
    AttachContext ctx = { resources, assets, animators };
    auto root = AttachComposition(json, &ctx);

    LOG("** Attached %d animators\n", animators.size());

    fScene = sksg::Scene::Make(std::move(root), std::move(animators));

    // In case the client calls render before the first tick.
    this->animationTick(0);
}

Animation::~Animation() = default;

void Animation::setShowInval(bool show) {
    if (fScene) {
        fScene->setShowInval(show);
    }
}

void Animation::render(SkCanvas* canvas, const SkRect* dstR) const {
    if (!fScene)
        return;

    SkAutoCanvasRestore restore(canvas, true);
    const SkRect srcR = SkRect::MakeSize(this->size());
    if (dstR) {
        canvas->concat(SkMatrix::MakeRectToRect(srcR, *dstR, SkMatrix::kCenter_ScaleToFit));
    }
    canvas->clipRect(srcR);
    fScene->render(canvas);
}

void Animation::animationTick(SkMSec ms) {
    if (!fScene)
        return;

    // 't' in the BM model really means 'frame #'
    auto t = static_cast<float>(ms) * fFrameRate / 1000;

    t = fInPoint + std::fmod(t, fOutPoint - fInPoint);

    fScene->animate(t);
}

} // namespace skottie
