// slang-ir-glsl-legalize.cpp
#include "slang-ir-glsl-legalize.h"

#include "slang-ir.h"
#include "slang-ir-insts.h"

#include "slang-extension-usage-tracker.h"

namespace Slang
{

//
// Legalization of entry points for GLSL:
//

IRGlobalParam* addGlobalParam(
    IRModule*   module,
    IRType*     valueType)
{
    auto session = module->session;

    SharedIRBuilder shared;
    shared.module = module;
    shared.session = session;

    IRBuilder builder;
    builder.sharedBuilder = &shared;
    return builder.createGlobalParam(valueType);
}

void moveValueBefore(
    IRInst*  valueToMove,
    IRInst*  placeBefore)
{
    valueToMove->removeFromParent();
    valueToMove->insertBefore(placeBefore);
}

IRType* getFieldType(
    IRType*         baseType,
    IRStructKey*    fieldKey)
{
    if(auto structType = as<IRStructType>(baseType))
    {
        for(auto ff : structType->getFields())
        {
            if(ff->getKey() == fieldKey)
                return ff->getFieldType();
        }
    }

    SLANG_UNEXPECTED("no such field");
    UNREACHABLE_RETURN(nullptr);
}



// When scalarizing shader inputs/outputs for GLSL, we need a way
// to refer to a conceptual "value" that might comprise multiple
// IR-level values. We could in principle introduce tuple types
// into the IR so that everything stays at the IR level, but
// it seems easier to just layer it over the top for now.
//
// The `ScalarizedVal` type deals with the "tuple or single value?"
// question, and also the "l-value or r-value?" question.
struct ScalarizedValImpl : RefObject
{};
struct ScalarizedTupleValImpl;
struct ScalarizedTypeAdapterValImpl;
struct ScalarizedVal
{
    enum class Flavor
    {
        // no value (null pointer)
        none,

        // A simple `IRInst*` that represents the actual value
        value,

        // An `IRInst*` that represents the address of the actual value
        address,

        // A `TupleValImpl` that represents zero or more `ScalarizedVal`s
        tuple,

        // A `TypeAdapterValImpl` that wraps a single `ScalarizedVal` and
        // represents an implicit type conversion applied to it on read
        // or write.
        typeAdapter,
    };

    // Create a value representing a simple value
    static ScalarizedVal value(IRInst* irValue)
    {
        ScalarizedVal result;
        result.flavor = Flavor::value;
        result.irValue = irValue;
        return result;
    }


    // Create a value representing an address
    static ScalarizedVal address(IRInst* irValue)
    {
        ScalarizedVal result;
        result.flavor = Flavor::address;
        result.irValue = irValue;
        return result;
    }

    static ScalarizedVal tuple(ScalarizedTupleValImpl* impl)
    {
        ScalarizedVal result;
        result.flavor = Flavor::tuple;
        result.impl = (ScalarizedValImpl*)impl;
        return result;
    }

    static ScalarizedVal typeAdapter(ScalarizedTypeAdapterValImpl* impl)
    {
        ScalarizedVal result;
        result.flavor = Flavor::typeAdapter;
        result.impl = (ScalarizedValImpl*)impl;
        return result;
    }

    Flavor                      flavor = Flavor::none;
    IRInst*                     irValue = nullptr;
    RefPtr<ScalarizedValImpl>   impl;
};

// This is the case for a value that is a "tuple" of other values
struct ScalarizedTupleValImpl : ScalarizedValImpl
{
    struct Element
    {
        IRStructKey*    key;
        ScalarizedVal   val;
    };

    IRType*         type;
    List<Element>   elements;
};

// This is the case for a value that is stored with one type,
// but needs to present itself as having a different type
struct ScalarizedTypeAdapterValImpl : ScalarizedValImpl
{
    ScalarizedVal   val;
    IRType*         actualType;   // the actual type of `val`
    IRType*         pretendType;     // the type this value pretends to have
};

struct GlobalVaryingDeclarator
{
    enum class Flavor
    {
        array,
    };

    Flavor                      flavor;
    IRInst*                     elementCount;
    GlobalVaryingDeclarator*    next;
};

struct GLSLSystemValueInfo
{
    // The name of the built-in GLSL variable
    char const* name;

    // The name of an outer array that wraps
    // the variable, in the case of a GS input
    char const*     outerArrayName;

    // The required type of the built-in variable
    IRType*     requiredType;
};

struct GLSLLegalizationContext
{
    Session*                session;
    ExtensionUsageTracker*  extensionUsageTracker;
    DiagnosticSink*         sink;
    Stage                   stage;

    void requireGLSLExtension(String const& name)
    {
        extensionUsageTracker->requireGLSLExtension(name);
    }

    void requireGLSLVersion(ProfileVersion version)
    {
        extensionUsageTracker->requireGLSLVersion(version);
    }

    Stage getStage()
    {
        return stage;
    }

    DiagnosticSink* getSink()
    {
        return sink;
    }

    IRBuilder* builder;
    IRBuilder* getBuilder() { return builder; }
};

GLSLSystemValueInfo* getGLSLSystemValueInfo(
    GLSLLegalizationContext*    context,
    VarLayout*                  varLayout,
    LayoutResourceKind          kind,
    Stage                       stage,
    GLSLSystemValueInfo*        inStorage)
{
    char const* name = nullptr;
    char const* outerArrayName = nullptr;

    auto semanticNameSpelling = varLayout->systemValueSemantic;
    if(semanticNameSpelling.getLength() == 0)
        return nullptr;

    auto semanticName = semanticNameSpelling.toLower();

    // HLSL semantic types can be found here
    // https://docs.microsoft.com/en-us/windows/desktop/direct3dhlsl/dx-graphics-hlsl-semantics
    /// NOTE! While there might be an "official" type for most of these in HLSL, in practice the user is allowed to declare almost anything
    /// that the HLSL compiler can implicitly convert to/from the correct type

    auto builder = context->getBuilder();
    IRType* requiredType = nullptr;

    if(semanticName == "sv_position")
    {
        // float4 in hlsl & glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_FragCoord.xhtml
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_Position.xhtml

        // This semantic can either work like `gl_FragCoord`
        // when it is used as a fragment shader input, or
        // like `gl_Position` when used in other stages.
        //
        // Note: This isn't as simple as testing input-vs-output,
        // because a user might have a VS output `SV_Position`,
        // and then pass it along to a GS that reads it as input.
        //
        if( stage == Stage::Fragment
            && kind == LayoutResourceKind::VaryingInput )
        {
            name = "gl_FragCoord";
        }
        else if( stage == Stage::Geometry
            && kind == LayoutResourceKind::VaryingInput )
        {
            // As a GS input, the correct syntax is `gl_in[...].gl_Position`,
            // but that is not compatible with picking the array dimension later,
            // of course.
            outerArrayName = "gl_in";
            name = "gl_Position";
        }
        else
        {
            name = "gl_Position";
        }

        requiredType = builder->getVectorType(builder->getBasicType(BaseType::Float), builder->getIntValue(builder->getIntType(), 4));
    }
    else if(semanticName == "sv_target")
    {
        // Note: we do *not* need to generate some kind of `gl_`
        // builtin for fragment-shader outputs: they are just
        // ordinary `out` variables, with ordinary `location`s,
        // as far as GLSL is concerned.
        return nullptr;
    }
    else if(semanticName == "sv_clipdistance")
    {
        // TODO: type conversion is required here.

        // float in hlsl & glsl.
        // "Clip distance data. SV_ClipDistance values are each assumed to be a float32 signed distance to a plane."
        // In glsl clipping value meaning is probably different
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_ClipDistance.xhtml

        name = "gl_ClipDistance";
        requiredType = builder->getBasicType(BaseType::Float);
    }
    else if(semanticName == "sv_culldistance")
    {
        // float in hlsl & glsl.
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_CullDistance.xhtml

        context->requireGLSLExtension("ARB_cull_distance");

        // TODO: type conversion is required here.
        name = "gl_CullDistance";
        requiredType = builder->getBasicType(BaseType::Float);
    }
    else if(semanticName == "sv_coverage")
    {
        // TODO: deal with `gl_SampleMaskIn` when used as an input.

        // TODO: type conversion is required here.

        // uint in hlsl, int in glsl
        // https://www.opengl.org/sdk/docs/manglsl/docbook4/xhtml/gl_SampleMask.xml

        requiredType = builder->getBasicType(BaseType::Int);

        name = "gl_SampleMask";
    }
    else if(semanticName == "sv_depth")
    {
        // Float in hlsl & glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_FragDepth.xhtml
        name = "gl_FragDepth";
        requiredType = builder->getBasicType(BaseType::Float);
    }
    else if(semanticName == "sv_depthgreaterequal")
    {
        // TODO: layout(depth_greater) out float gl_FragDepth;

        // Type is 'unknown' in hlsl 
        name = "gl_FragDepth";
        requiredType = builder->getBasicType(BaseType::Float);
    }
    else if(semanticName == "sv_depthlessequal")
    {
        // TODO: layout(depth_greater) out float gl_FragDepth;

        // 'unknown' in hlsl, float in glsl 
        name = "gl_FragDepth";
        requiredType = builder->getBasicType(BaseType::Float);
    }
    else if(semanticName == "sv_dispatchthreadid")
    {
        // uint3 in hlsl, uvec3 in glsl
        // https://www.opengl.org/sdk/docs/manglsl/docbook4/xhtml/gl_GlobalInvocationID.xml
        name = "gl_GlobalInvocationID";

        requiredType = builder->getVectorType(builder->getBasicType(BaseType::UInt), builder->getIntValue(builder->getIntType(), 3));
    }
    else if(semanticName == "sv_domainlocation")
    {
        // float2|3 in hlsl, vec3 in glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_TessCoord.xhtml

        requiredType = builder->getVectorType(builder->getBasicType(BaseType::Float), builder->getIntValue(builder->getIntType(), 3));

        name = "gl_TessCoord";
    }
    else if(semanticName == "sv_groupid")
    {
        // uint3 in hlsl, uvec3 in glsl
        // https://www.opengl.org/sdk/docs/manglsl/docbook4/xhtml/gl_WorkGroupID.xml
        name = "gl_WorkGroupID";

        requiredType = builder->getVectorType(builder->getBasicType(BaseType::UInt), builder->getIntValue(builder->getIntType(), 3));
    }
    else if(semanticName == "sv_groupindex")
    {
        // uint in hlsl & in glsl
        name = "gl_LocalInvocationIndex";
        requiredType = builder->getBasicType(BaseType::UInt);
    }
    else if(semanticName == "sv_groupthreadid")
    {
        // uint3 in hlsl, uvec3 in glsl
        name = "gl_LocalInvocationID";

        requiredType = builder->getVectorType(builder->getBasicType(BaseType::UInt), builder->getIntValue(builder->getIntType(), 3));
    }
    else if(semanticName == "sv_gsinstanceid")
    {
        // uint in hlsl, int in glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_InvocationID.xhtml

        requiredType = builder->getBasicType(BaseType::Int);
        name = "gl_InvocationID";
    }
    else if(semanticName == "sv_instanceid")
    {
        // https://docs.microsoft.com/en-us/windows/desktop/direct3d11/d3d10-graphics-programming-guide-input-assembler-stage-using#instanceid
        // uint in hlsl, int in glsl 

        requiredType = builder->getBasicType(BaseType::Int);
        name = "gl_InstanceIndex";
    }
    else if(semanticName == "sv_isfrontface")
    {
        // bool in hlsl & glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_FrontFacing.xhtml
        name = "gl_FrontFacing";
        requiredType = builder->getBasicType(BaseType::Bool);
    }
    else if(semanticName == "sv_outputcontrolpointid")
    {
        // uint in hlsl, int in glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_InvocationID.xhtml

        name = "gl_InvocationID";

        requiredType = builder->getBasicType(BaseType::Int);
    }
    else if (semanticName == "sv_pointsize")
    {
        // float in hlsl & glsl
        name = "gl_PointSize";
        requiredType = builder->getBasicType(BaseType::Float);
    }
    else if(semanticName == "sv_primitiveid")
    {
        // uint in hlsl, int in glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_PrimitiveID.xhtml
        name = "gl_PrimitiveID";

        requiredType = builder->getBasicType(BaseType::Int);
    }
    else if (semanticName == "sv_rendertargetarrayindex")
    {
        // uint on hlsl, int on glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_Layer.xhtml

        switch (context->getStage())
        {
        case Stage::Geometry:
            context->requireGLSLVersion(ProfileVersion::GLSL_150);
            break;

        case Stage::Fragment:
            context->requireGLSLVersion(ProfileVersion::GLSL_430);
            break;

        default:
            context->requireGLSLVersion(ProfileVersion::GLSL_450);
            context->requireGLSLExtension("GL_ARB_shader_viewport_layer_array");
            break;
        }

        name = "gl_Layer";
        requiredType = builder->getBasicType(BaseType::Int);
    }
    else if (semanticName == "sv_sampleindex")
    {
        // uint in hlsl, int in glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_SampleID.xhtml

        requiredType = builder->getBasicType(BaseType::Int);
        name = "gl_SampleID";
    }
    else if (semanticName == "sv_stencilref")
    {
        // uint in hlsl, int in glsl
        // https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_shader_stencil_export.txt

        requiredType = builder->getBasicType(BaseType::Int);

        context->requireGLSLExtension("ARB_shader_stencil_export");
        name = "gl_FragStencilRef";
    }
    else if (semanticName == "sv_tessfactor")
    {
        // TODO(JS): Adjust type does *not* handle the conversion correctly. More specifically a float array hlsl
        // parameter goes through code to make SOA in createGLSLGlobalVaryingsImpl.  
        // 
        // Can be input and output.
        // 
        // https://docs.microsoft.com/en-us/windows/desktop/direct3dhlsl/sv-tessfactor
        // "Tessellation factors must be declared as an array; they cannot be packed into a single vector."
        //
        // float[2|3|4] in hlsl, float[4] on glsl (ie both are arrays but might be different size)
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_TessLevelOuter.xhtml

        name = "gl_TessLevelOuter";

        // float[4] on glsl
        requiredType = builder->getArrayType(builder->getBasicType(BaseType::Float), builder->getIntValue(builder->getIntType(), 4));
    }
    else if (semanticName == "sv_vertexid")
    {
        // uint in hlsl, int in glsl (https://www.khronos.org/opengl/wiki/Built-in_Variable_(GLSL))
        requiredType = builder->getBasicType(BaseType::Int);
        name = "gl_VertexIndex";
    }
    else if (semanticName == "sv_viewportarrayindex")
    {
        // uint on hlsl, int on glsl
        // https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/gl_ViewportIndex.xhtml

        requiredType = builder->getBasicType(BaseType::Int);
        name = "gl_ViewportIndex";
    }
    else if (semanticName == "nv_x_right")
    {
        context->requireGLSLVersion(ProfileVersion::GLSL_450);
        context->requireGLSLExtension("GL_NVX_multiview_per_view_attributes");

        // The actual output in GLSL is:
        //
        //    vec4 gl_PositionPerViewNV[];
        //
        // and is meant to support an arbitrary number of views,
        // while the HLSL case just defines a second position
        // output.
        //
        // For now we will hack this by:
        //   1. Mapping an `NV_X_Right` output to `gl_PositionPerViewNV[1]`
        //      (that is, just one element of the output array)
        //   2. Adding logic to copy the traditional `gl_Position` output
        //      over to `gl_PositionPerViewNV[0]`
        //

        name = "gl_PositionPerViewNV[1]";

//            shared->requiresCopyGLPositionToPositionPerView = true;
    }
    else if (semanticName == "nv_viewport_mask")
    {
        // TODO: This doesn't seem to work correctly on it's own between hlsl/glsl

        // Indeed on slang issue 109 claims this remains a problem  
        // https://github.com/shader-slang/slang/issues/109

        // On hlsl it's UINT related. "higher 16 bits for the right view, lower 16 bits for the left view."
        // There is use in hlsl shader code as uint4 - not clear if that varies 
        // https://github.com/KhronosGroup/GLSL/blob/master/extensions/nvx/GL_NVX_multiview_per_view_attributes.txt
        // On glsl its highp int gl_ViewportMaskPerViewNV[];

        context->requireGLSLVersion(ProfileVersion::GLSL_450);
        context->requireGLSLExtension("GL_NVX_multiview_per_view_attributes");

        name = "gl_ViewportMaskPerViewNV";
//            globalVarExpr = createGLSLBuiltinRef("gl_ViewportMaskPerViewNV",
//                getUnsizedArrayType(getIntType()));
    }

    if( name )
    {
        inStorage->name = name;
        inStorage->outerArrayName = outerArrayName;
        inStorage->requiredType = requiredType;
        return inStorage;
    }

    context->getSink()->diagnose(varLayout->varDecl.getDecl()->loc, Diagnostics::unknownSystemValueSemantic, semanticNameSpelling);
    return nullptr;
}

ScalarizedVal createSimpleGLSLGlobalVarying(
    GLSLLegalizationContext*    context,
    IRBuilder*                  builder,
    IRType*                     inType,
    VarLayout*                  inVarLayout,
    TypeLayout*                 inTypeLayout,
    LayoutResourceKind          kind,
    Stage                       stage,
    UInt                        bindingIndex,
    GlobalVaryingDeclarator*    declarator)
{
    // Check if we have a system value on our hands.
    GLSLSystemValueInfo systemValueInfoStorage;
    auto systemValueInfo = getGLSLSystemValueInfo(
        context,
        inVarLayout,
        kind,
        stage,
        &systemValueInfoStorage);

    IRType* type = inType;

    // A system-value semantic might end up needing to override the type
    // that the user specified.
    if( systemValueInfo && systemValueInfo->requiredType )
    {
        type = systemValueInfo->requiredType;
    }

    // Construct the actual type and type-layout for the global variable
    //
    RefPtr<TypeLayout> typeLayout = inTypeLayout;
    for( auto dd = declarator; dd; dd = dd->next )
    {
        // We only have one declarator case right now...
        SLANG_ASSERT(dd->flavor == GlobalVaryingDeclarator::Flavor::array);

        auto arrayType = builder->getArrayType(
            type,
            dd->elementCount);

        RefPtr<ArrayTypeLayout> arrayTypeLayout = new ArrayTypeLayout();
//            arrayTypeLayout->type = arrayType;
        arrayTypeLayout->rules = typeLayout->rules;
        arrayTypeLayout->originalElementTypeLayout =  typeLayout;
        arrayTypeLayout->elementTypeLayout = typeLayout;
        arrayTypeLayout->uniformStride = 0;

        if( auto resInfo = inTypeLayout->FindResourceInfo(kind) )
        {
            // TODO: it is kind of gross to be re-running some
            // of the type layout logic here.

            UInt elementCount = (UInt) GetIntVal(dd->elementCount);
            arrayTypeLayout->addResourceUsage(
                kind,
                resInfo->count * elementCount);
        }

        type = arrayType;
        typeLayout = arrayTypeLayout;
    }

    // We need to construct a fresh layout for the variable, even
    // if the original had its own layout, because it might be
    // an `inout` parameter, and we only want to deal with the case
    // described by our `kind` parameter.
    RefPtr<VarLayout> varLayout = new VarLayout();
    varLayout->varDecl = inVarLayout->varDecl;
    varLayout->typeLayout = typeLayout;
    varLayout->flags = inVarLayout->flags;
    varLayout->systemValueSemantic = inVarLayout->systemValueSemantic;
    varLayout->systemValueSemanticIndex = inVarLayout->systemValueSemanticIndex;
    varLayout->semanticName = inVarLayout->semanticName;
    varLayout->semanticIndex = inVarLayout->semanticIndex;
    varLayout->stage = inVarLayout->stage;
    varLayout->AddResourceInfo(kind)->index = bindingIndex;

    // We are going to be creating a global parameter to replace
    // the function parameter, but we need to handle the case
    // where the parameter represents a varying *output* and not
    // just an input.
    //
    // Our IR global shader parameters are read-only, just
    // like our IR function parameters, and need a wrapper
    // `Out<...>` type to represent outputs.
    //
    bool isOutput = kind == LayoutResourceKind::VaryingOutput;
    IRType* paramType = isOutput ? builder->getOutType(type) : type;

    auto globalParam = addGlobalParam(builder->getModule(), paramType);
    moveValueBefore(globalParam, builder->getFunc());

    ScalarizedVal val = isOutput ? ScalarizedVal::address(globalParam) : ScalarizedVal::value(globalParam);

    if( systemValueInfo )
    {
        builder->addImportDecoration(globalParam, UnownedTerminatedStringSlice(systemValueInfo->name));

        if( auto fromType = systemValueInfo->requiredType )
        {
            // We may need to adapt from the declared type to/from
            // the actual type of the GLSL global.
            auto toType = inType;

            if( !isTypeEqual(fromType, toType ))
            {
                RefPtr<ScalarizedTypeAdapterValImpl> typeAdapter = new ScalarizedTypeAdapterValImpl;
                typeAdapter->actualType = systemValueInfo->requiredType;
                typeAdapter->pretendType = inType;
                typeAdapter->val = val;

                val = ScalarizedVal::typeAdapter(typeAdapter);
            }
        }

        if(auto outerArrayName = systemValueInfo->outerArrayName)
        {
            builder->addGLSLOuterArrayDecoration(globalParam, UnownedTerminatedStringSlice(outerArrayName));
        }
    }

    builder->addLayoutDecoration(globalParam, varLayout);

    return val;
}

ScalarizedVal createGLSLGlobalVaryingsImpl(
    GLSLLegalizationContext*    context,
    IRBuilder*                  builder,
    IRType*                     type,
    VarLayout*                  varLayout,
    TypeLayout*                 typeLayout,
    LayoutResourceKind          kind,
    Stage                       stage,
    UInt                        bindingIndex,
    GlobalVaryingDeclarator*    declarator)
{
    if (as<IRVoidType>(type))
    {
        return ScalarizedVal();
    }
    else if( as<IRBasicType>(type) )
    {
        return createSimpleGLSLGlobalVarying(
            context,
            builder, type, varLayout, typeLayout, kind, stage, bindingIndex, declarator);
    }
    else if( as<IRVectorType>(type) )
    {
        return createSimpleGLSLGlobalVarying(
            context,
            builder, type, varLayout, typeLayout, kind, stage, bindingIndex, declarator);
    }
    else if( as<IRMatrixType>(type) )
    {
        // TODO: a matrix-type varying should probably be handled like an array of rows
        return createSimpleGLSLGlobalVarying(
            context,
            builder, type, varLayout, typeLayout, kind, stage, bindingIndex, declarator);
    }
    else if( auto arrayType = as<IRArrayType>(type) )
    {
        // We will need to SOA-ize any nested types.

        auto elementType = arrayType->getElementType();
        auto elementCount = arrayType->getElementCount();
        auto arrayLayout = as<ArrayTypeLayout>(typeLayout);
        SLANG_ASSERT(arrayLayout);
        auto elementTypeLayout = arrayLayout->elementTypeLayout;

        GlobalVaryingDeclarator arrayDeclarator;
        arrayDeclarator.flavor = GlobalVaryingDeclarator::Flavor::array;
        arrayDeclarator.elementCount = elementCount;
        arrayDeclarator.next = declarator;

        return createGLSLGlobalVaryingsImpl(
            context,
            builder,
            elementType,
            varLayout,
            elementTypeLayout,
            kind,
            stage,
            bindingIndex,
            &arrayDeclarator);
    }
    else if( auto streamType = as<IRHLSLStreamOutputType>(type))
    {
        auto elementType = streamType->getElementType();
        auto streamLayout = as<StreamOutputTypeLayout>(typeLayout);
        SLANG_ASSERT(streamLayout);
        auto elementTypeLayout = streamLayout->elementTypeLayout;

        return createGLSLGlobalVaryingsImpl(
            context,
            builder,
            elementType,
            varLayout,
            elementTypeLayout,
            kind,
            stage,
            bindingIndex,
            declarator);
    }
    else if(auto structType = as<IRStructType>(type))
    {
        // We need to recurse down into the individual fields,
        // and generate a variable for each of them.

        auto structTypeLayout = as<StructTypeLayout>(typeLayout);
        SLANG_ASSERT(structTypeLayout);
        RefPtr<ScalarizedTupleValImpl> tupleValImpl = new ScalarizedTupleValImpl();


        // Construct the actual type for the tuple (including any outer arrays)
        IRType* fullType = type;
        for( auto dd = declarator; dd; dd = dd->next )
        {
            SLANG_ASSERT(dd->flavor == GlobalVaryingDeclarator::Flavor::array);
            fullType = builder->getArrayType(
                fullType,
                dd->elementCount);
        }

        tupleValImpl->type = fullType;

        // Okay, we want to walk through the fields here, and
        // generate one variable for each.
        UInt fieldCounter = 0;
        for(auto field : structType->getFields())
        {
            UInt fieldIndex = fieldCounter++;

            auto fieldLayout = structTypeLayout->fields[fieldIndex];

            UInt fieldBindingIndex = bindingIndex;
            if(auto fieldResInfo = fieldLayout->FindResourceInfo(kind))
                fieldBindingIndex += fieldResInfo->index;

            auto fieldVal = createGLSLGlobalVaryingsImpl(
                context,
                builder,
                field->getFieldType(),
                fieldLayout,
                fieldLayout->typeLayout,
                kind,
                stage,
                fieldBindingIndex,
                declarator);
            if (fieldVal.flavor != ScalarizedVal::Flavor::none)
            {
                ScalarizedTupleValImpl::Element element;
                element.val = fieldVal;
                element.key = field->getKey();

                tupleValImpl->elements.add(element);
            }
        }

        return ScalarizedVal::tuple(tupleValImpl);
    }

    // Default case is to fall back on the simple behavior
    return createSimpleGLSLGlobalVarying(
        context,
        builder, type, varLayout, typeLayout, kind, stage, bindingIndex, declarator);
}

ScalarizedVal createGLSLGlobalVaryings(
    GLSLLegalizationContext*    context,
    IRBuilder*                  builder,
    IRType*                     type,
    VarLayout*                  layout,
    LayoutResourceKind          kind,
    Stage                       stage)
{
    UInt bindingIndex = 0;
    if(auto rr = layout->FindResourceInfo(kind))
        bindingIndex = rr->index;
    return createGLSLGlobalVaryingsImpl(
        context,
        builder, type, layout, layout->typeLayout, kind, stage, bindingIndex, nullptr);
}

ScalarizedVal extractField(
    IRBuilder*              builder,
    ScalarizedVal const&    val,
    UInt                    fieldIndex,
    IRStructKey*            fieldKey)
{
    switch( val.flavor )
    {
    case ScalarizedVal::Flavor::value:
        return ScalarizedVal::value(
            builder->emitFieldExtract(
                getFieldType(val.irValue->getDataType(), fieldKey),
                val.irValue,
                fieldKey));

    case ScalarizedVal::Flavor::address:
        {
            auto ptrType = as<IRPtrTypeBase>(val.irValue->getDataType());
            auto valType = ptrType->getValueType();
            auto fieldType = getFieldType(valType, fieldKey);
            auto fieldPtrType = builder->getPtrType(ptrType->op, fieldType);
            return ScalarizedVal::address(
                builder->emitFieldAddress(
                    fieldPtrType,
                    val.irValue,
                    fieldKey));
        }

    case ScalarizedVal::Flavor::tuple:
        {
            auto tupleVal = as<ScalarizedTupleValImpl>(val.impl);
            return tupleVal->elements[fieldIndex].val;
        }

    default:
        SLANG_UNEXPECTED("unimplemented");
        UNREACHABLE_RETURN(ScalarizedVal());
    }

}

ScalarizedVal adaptType(
    IRBuilder*              builder,
    IRInst*                 val,
    IRType*                 toType,
    IRType*                 /*fromType*/)
{
    // TODO: actually consider what needs to go on here...
    return ScalarizedVal::value(builder->emitConstructorInst(
        toType,
        1,
        &val));
}

ScalarizedVal adaptType(
    IRBuilder*              builder,
    ScalarizedVal const&    val,
    IRType*                 toType,
    IRType*                 fromType)
{
    switch( val.flavor )
    {
    case ScalarizedVal::Flavor::value:
        return adaptType(builder, val.irValue, toType, fromType);
        break;

    case ScalarizedVal::Flavor::address:
        {
            auto loaded = builder->emitLoad(val.irValue);
            return adaptType(builder, loaded, toType, fromType);
        }
        break;

    default:
        SLANG_UNEXPECTED("unimplemented");
        UNREACHABLE_RETURN(ScalarizedVal());
    }
}

void assign(
    IRBuilder*              builder,
    ScalarizedVal const&    left,
    ScalarizedVal const&    right)
{
    switch( left.flavor )
    {
    case ScalarizedVal::Flavor::address:
        switch( right.flavor )
        {
        case ScalarizedVal::Flavor::value:
            {
                builder->emitStore(left.irValue, right.irValue);
            }
            break;

        case ScalarizedVal::Flavor::address:
            {
                auto val = builder->emitLoad(right.irValue);
                builder->emitStore(left.irValue, val);
            }
            break;

        case ScalarizedVal::Flavor::tuple:
            {
                // We are assigning from a tuple to a destination
                // that is not a tuple. We will perform assignment
                // element-by-element.
                auto rightTupleVal = as<ScalarizedTupleValImpl>(right.impl);
                Index elementCount = rightTupleVal->elements.getCount();

                for( Index ee = 0; ee < elementCount; ++ee )
                {
                    auto rightElement = rightTupleVal->elements[ee];
                    auto leftElementVal = extractField(
                        builder,
                        left,
                        ee,
                        rightElement.key);
                    assign(builder, leftElementVal, rightElement.val);
                }
            }
            break;

        default:
            SLANG_UNEXPECTED("unimplemented");
            break;
        }
        break;

    case ScalarizedVal::Flavor::tuple:
        {
            // We have a tuple, so we are going to need to try and assign
            // to each of its constituent fields.
            auto leftTupleVal = as<ScalarizedTupleValImpl>(left.impl);
            Index elementCount = leftTupleVal->elements.getCount();

            for( Index ee = 0; ee < elementCount; ++ee )
            {
                auto rightElementVal = extractField(
                    builder,
                    right,
                    ee,
                    leftTupleVal->elements[ee].key);
                assign(builder, leftTupleVal->elements[ee].val, rightElementVal);
            }
        }
        break;

    case ScalarizedVal::Flavor::typeAdapter:
        {
            // We are trying to assign to something that had its type adjusted,
            // so we will need to adjust the type of the right-hand side first.
            //
            // In this case we are converting to the actual type of the GLSL variable,
            // from the "pretend" type that it had in the IR before.
            auto typeAdapter = as<ScalarizedTypeAdapterValImpl>(left.impl);
            auto adaptedRight = adaptType(builder, right, typeAdapter->actualType, typeAdapter->pretendType);
            assign(builder, typeAdapter->val, adaptedRight);
        }
        break;

    default:
        SLANG_UNEXPECTED("unimplemented");
        break;
    }
}

ScalarizedVal getSubscriptVal(
    IRBuilder*      builder,
    IRType*         elementType,
    ScalarizedVal   val,
    IRInst*         indexVal)
{
    switch( val.flavor )
    {
    case ScalarizedVal::Flavor::value:
        return ScalarizedVal::value(
            builder->emitElementExtract(
                elementType,
                val.irValue,
                indexVal));

    case ScalarizedVal::Flavor::address:
        return ScalarizedVal::address(
            builder->emitElementAddress(
                builder->getPtrType(elementType),
                val.irValue,
                indexVal));

    case ScalarizedVal::Flavor::tuple:
        {
            auto inputTuple = val.impl.as<ScalarizedTupleValImpl>();

            RefPtr<ScalarizedTupleValImpl> resultTuple = new ScalarizedTupleValImpl();
            resultTuple->type = elementType;

            Index elementCount = inputTuple->elements.getCount();
            Index elementCounter = 0;

            auto structType = as<IRStructType>(elementType);
            for(auto field : structType->getFields())
            {
                auto tupleElementType = field->getFieldType();

                Index elementIndex = elementCounter++;

                SLANG_RELEASE_ASSERT(elementIndex < elementCount);
                auto inputElement = inputTuple->elements[elementIndex];

                ScalarizedTupleValImpl::Element resultElement;
                resultElement.key = inputElement.key;
                resultElement.val = getSubscriptVal(
                    builder,
                    tupleElementType,
                    inputElement.val,
                    indexVal);

                resultTuple->elements.add(resultElement);
            }
            SLANG_RELEASE_ASSERT(elementCounter == elementCount);

            return ScalarizedVal::tuple(resultTuple);
        }

    default:
        SLANG_UNEXPECTED("unimplemented");
        UNREACHABLE_RETURN(ScalarizedVal());
    }
}

ScalarizedVal getSubscriptVal(
    IRBuilder*      builder,
    IRType*         elementType,
    ScalarizedVal   val,
    UInt            index)
{
    return getSubscriptVal(
        builder,
        elementType,
        val,
        builder->getIntValue(
            builder->getIntType(),
            index));
}

IRInst* materializeValue(
    IRBuilder*              builder,
    ScalarizedVal const&    val);

IRInst* materializeTupleValue(
    IRBuilder*      builder,
    ScalarizedVal   val)
{
    auto tupleVal = val.impl.as<ScalarizedTupleValImpl>();
    SLANG_ASSERT(tupleVal);

    Index elementCount = tupleVal->elements.getCount();
    auto type = tupleVal->type;

    if( auto arrayType = as<IRArrayType>(type))
    {
        // The tuple represent an array, which means that the
        // individual elements are expected to yield arrays as well.
        //
        // We will extract a value for each array element, and
        // then use these to construct our result.

        List<IRInst*> arrayElementVals;
        UInt arrayElementCount = (UInt) GetIntVal(arrayType->getElementCount());

        for( UInt ii = 0; ii < arrayElementCount; ++ii )
        {
            auto arrayElementPseudoVal = getSubscriptVal(
                builder,
                arrayType->getElementType(),
                val,
                ii);

            auto arrayElementVal = materializeValue(
                builder,
                arrayElementPseudoVal);

            arrayElementVals.add(arrayElementVal);
        }

        return builder->emitMakeArray(
            arrayType,
            arrayElementVals.getCount(),
            arrayElementVals.getBuffer());
    }
    else
    {
        // The tuple represents a value of some aggregate type,
        // so we can simply materialize the elements and then
        // construct a value of that type.
        //
        // TODO: this should be using a `makeStruct` instruction.

        List<IRInst*> elementVals;
        for( Index ee = 0; ee < elementCount; ++ee )
        {
            auto elementVal = materializeValue(builder, tupleVal->elements[ee].val);
            elementVals.add(elementVal);
        }

        return builder->emitConstructorInst(
            tupleVal->type,
            elementVals.getCount(),
            elementVals.getBuffer());
    }
}

IRInst* materializeValue(
    IRBuilder*              builder,
    ScalarizedVal const&    val)
{
    switch( val.flavor )
    {
    case ScalarizedVal::Flavor::value:
        return val.irValue;

    case ScalarizedVal::Flavor::address:
        {
            auto loadInst = builder->emitLoad(val.irValue);
            return loadInst;
        }
        break;

    case ScalarizedVal::Flavor::tuple:
        {
            //auto tupleVal = as<ScalarizedTupleValImpl>(val.impl);
            return materializeTupleValue(builder, val);
        }
        break;

    case ScalarizedVal::Flavor::typeAdapter:
        {
            // Somebody is trying to use a value where its actual type
            // doesn't match the type it pretends to have. To make this
            // work we need to adapt the type from its actual type over
            // to its pretend type.
            auto typeAdapter = as<ScalarizedTypeAdapterValImpl>(val.impl);
            auto adapted = adaptType(builder, typeAdapter->val, typeAdapter->pretendType, typeAdapter->actualType);
            return materializeValue(builder, adapted);
        }
        break;

    default:
        SLANG_UNEXPECTED("unimplemented");
        break;
    }
}

void legalizeRayTracingEntryPointParameterForGLSL(
    GLSLLegalizationContext*    context,
    IRFunc*                     func,
    IRParam*                    pp,
    VarLayout*                  paramLayout)
{
    auto builder = context->getBuilder();
    auto paramType = pp->getDataType();

    // The parameter might be either an `in` parameter,
    // or an `out` or `in out` parameter, and in those
    // latter cases its IR-level type will include a
    // wrapping "pointer-like" type (e.g., `Out<Float>`
    // instead of just `Float`).
    //
    // Because global shader parameters are read-only
    // in the same way function types are, we can take
    // care of that detail here just by allocating a
    // global shader parameter with exactly the type
    // of the original function parameter.
    //
    auto globalParam = addGlobalParam(builder->getModule(), paramType);
    builder->addLayoutDecoration(globalParam, paramLayout);
    moveValueBefore(globalParam, builder->getFunc());
    pp->replaceUsesWith(globalParam);

    // Because linkage between ray-tracing shaders is
    // based on the type of incoming/outgoing payload
    // and attribute parameters, it would be an error to
    // eliminate the global parameter *even if* it is
    // not actually used inside the entry point.
    //
    // We attach a decoration to the entry point that
    // makes note of the dependency, so that steps
    // like dead code elimination cannot get rid of
    // the parameter.
    //
    // TODO: We could consider using a structure like
    // this for *all* of the entry point parameters
    // that get moved to the global scope, since SPIR-V
    // ends up requiring such information on an `OpEntryPoint`.
    //
    // As a further alternative, we could decide to
    // keep entry point varying input/outtput attached
    // to the parameter list through all of the Slang IR
    // steps, and only declare it as global variables at
    // the last minute when emitting a GLSL `main` or
    // SPIR-V for an entry point.
    //
    builder->addDependsOnDecoration(func, globalParam);
}

void legalizeEntryPointParameterForGLSL(
    GLSLLegalizationContext*    context,
    IRFunc*                     func,
    IRParam*                    pp,
    VarLayout*                  paramLayout)
{
    auto builder = context->getBuilder();
    auto stage = context->getStage();

    // We need to create a global variable that will replace the parameter.
    // It seems superficially obvious that the variable should have
    // the same type as the parameter.
    // However, if the parameter was a pointer, in order to
    // support `out` or `in out` parameter passing, we need
    // to be sure to allocate a variable of the pointed-to
    // type instead.
    //
    // We also need to replace uses of the parameter with
    // uses of the variable, and the exact logic there
    // will differ a bit between the pointer and non-pointer
    // cases.
    auto paramType = pp->getDataType();

    // First we will special-case stage input/outputs that
    // don't fit into the standard varying model.
    // For right now we are only doing special-case handling
    // of geometry shader output streams.
    if( auto paramPtrType = as<IROutTypeBase>(paramType) )
    {
        auto valueType = paramPtrType->getValueType();
        if( auto gsStreamType = as<IRHLSLStreamOutputType>(valueType) )
        {
            // An output stream type like `TriangleStream<Foo>` should
            // more or less translate into `out Foo` (plus scalarization).

            auto globalOutputVal = createGLSLGlobalVaryings(
                context,
                builder,
                valueType,
                paramLayout,
                LayoutResourceKind::VaryingOutput,
                stage);

            // TODO: a GS output stream might be passed into other
            // functions, so that we should really be modifying
            // any function that has one of these in its parameter
            // list (and in the limit we should be leagalizing any
            // type that nests these...).
            //
            // For now we will just try to deal with `Append` calls
            // directly in this function.



            for( auto bb = func->getFirstBlock(); bb; bb = bb->getNextBlock() )
            {
                for( auto ii = bb->getFirstInst(); ii; ii = ii->getNextInst() )
                {
                    // Is it a call?
                    if(ii->op != kIROp_Call)
                        continue;

                    // Is it calling the append operation?
                    auto callee = ii->getOperand(0);
                    for(;;)
                    {
                        // If the instruction is `specialize(X,...)` then
                        // we want to look at `X`, and if it is `generic { ... return R; }`
                        // then we want to look at `R`. We handle this
                        // iteratively here.
                        //
                        // TODO: This idiom seems to come up enough that we
                        // should probably have a dedicated convenience routine
                        // for this.
                        //
                        // Alternatively, we could switch the IR encoding so
                        // that decorations are added to the generic instead of the
                        // value it returns.
                        //
                        switch(callee->op)
                        {
                        case kIROp_Specialize:
                            {
                                callee = cast<IRSpecialize>(callee)->getOperand(0);
                                continue;
                            }

                        case kIROp_Generic:
                            {
                                auto genericResult = findGenericReturnVal(cast<IRGeneric>(callee));
                                if(genericResult)
                                {
                                    callee = genericResult;
                                    continue;
                                }
                            }

                        default:
                            break;
                        }
                        break;
                    }
                    if(callee->op != kIROp_Func)
                        continue;

                    // HACK: we will identify the operation based
                    // on the target-intrinsic definition that was
                    // given to it.
                    auto decoration = findTargetIntrinsicDecoration(callee, "glsl");
                    if(!decoration)
                        continue;

                    if(decoration->getDefinition() != UnownedStringSlice::fromLiteral("EmitVertex()"))
                    {
                        continue;
                    }

                    // Okay, we have a declaration, and we want to modify it!

                    builder->setInsertBefore(ii);

                    assign(builder, globalOutputVal, ScalarizedVal::value(ii->getOperand(2)));
                }
            }

            // We will still have references to the parameter coming
            // from the `EmitVertex` calls, so we need to replace it
            // with something. There isn't anything reasonable to
            // replace it with that would have the right type, so
            // we will replace it with an undefined value, knowing
            // that the emitted code will not actually reference it.
            //
            // TODO: This approach to generating geometry shader code
            // is not ideal, and we should strive to find a better
            // approach that involes coding the `EmitVertex` operation
            // directly in the stdlib, similar to how ray-tracing
            // operations like `TraceRay` are handled.
            //
            builder->setInsertBefore(func->getFirstBlock()->getFirstOrdinaryInst());
            auto undefinedVal = builder->emitUndefined(pp->getFullType());
            pp->replaceUsesWith(undefinedVal);

            return;
        }
    }

    // When we have an HLSL ray tracing shader entry point,
    // we don't want to translate the inputs/outputs for GLSL/SPIR-V
    // according to our default rules, for two reasons:
    //
    // 1. The input and output for these stages are expected to
    // be packaged into `struct` types rather than be scalarized,
    // so the usual scalarization approach we take here should
    // not be applied.
    //
    // 2. An `in out` parameter isn't just sugar for a combination
    // of an `in` and an `out` parameter, and instead represents the
    // read/write "payload" that was passed in. It should legalize
    // to a single variable, and we can lower reads/writes of it
    // directly, rather than introduce an intermediate temporary.
    //
    switch( stage )
    {
    default:
        break;

    case Stage::AnyHit:
    case Stage::Callable:
    case Stage::ClosestHit:
    case Stage::Intersection:
    case Stage::Miss:
    case Stage::RayGeneration:
        legalizeRayTracingEntryPointParameterForGLSL(context, func, pp, paramLayout);
        return;
    }

    // Is the parameter type a special pointer type
    // that indicates the parameter is used for `out`
    // or `inout` access?
    if(auto paramPtrType = as<IROutTypeBase>(paramType) )
    {
        // Okay, we have the more interesting case here,
        // where the parameter was being passed by reference.
        // We are going to create a local variable of the appropriate
        // type, which will replace the parameter, along with
        // one or more global variables for the actual input/output.

        auto valueType = paramPtrType->getValueType();

        auto localVariable = builder->emitVar(valueType);
        auto localVal = ScalarizedVal::address(localVariable);

        if( auto inOutType = as<IRInOutType>(paramPtrType) )
        {
            // In the `in out` case we need to declare two
            // sets of global variables: one for the `in`
            // side and one for the `out` side.
            auto globalInputVal = createGLSLGlobalVaryings(
                context,
                builder, valueType, paramLayout, LayoutResourceKind::VaryingInput, stage);

            assign(builder, localVal, globalInputVal);
        }

        // Any places where the original parameter was used inside
        // the function body should instead use the new local variable.
        // Since the parameter was a pointer, we use the variable instruction
        // itself (which is an `alloca`d pointer) directly:
        pp->replaceUsesWith(localVariable);

        // We also need one or more global variables to write the output to
        // when the function is done. We create them here.
        auto globalOutputVal = createGLSLGlobalVaryings(
                context,
                builder, valueType, paramLayout, LayoutResourceKind::VaryingOutput, stage);

        // Now we need to iterate over all the blocks in the function looking
        // for any `return*` instructions, so that we can write to the output variable
        for( auto bb = func->getFirstBlock(); bb; bb = bb->getNextBlock() )
        {
            auto terminatorInst = bb->getLastInst();
            if(!terminatorInst)
                continue;

            switch( terminatorInst->op )
            {
            default:
                continue;

            case kIROp_ReturnVal:
            case kIROp_ReturnVoid:
                break;
            }

            // We dont' re-use `builder` here because we don't want to
            // disrupt the source location it is using for inserting
            // temporary variables at the top of the function.
            //
            IRBuilder terminatorBuilder;
            terminatorBuilder.sharedBuilder = builder->sharedBuilder;
            terminatorBuilder.setInsertBefore(terminatorInst);

            // Assign from the local variabel to the global output
            // variable before the actual `return` takes place.
            assign(&terminatorBuilder, globalOutputVal, localVal);
        }
    }
    else
    {
        // This is the "easy" case where the parameter wasn't
        // being passed by reference. We start by just creating
        // one or more global variables to represent the parameter,
        // and attach the required layout information to it along
        // the way.

        auto globalValue = createGLSLGlobalVaryings(
            context,
            builder, paramType, paramLayout, LayoutResourceKind::VaryingInput, stage);

        // Next we need to replace uses of the parameter with
        // references to the variable(s). We are going to do that
        // somewhat naively, by simply materializing the
        // variables at the start.
        IRInst* materialized = materializeValue(builder, globalValue);

        pp->replaceUsesWith(materialized);
    }
}

void legalizeEntryPointForGLSL(
    Session*                session,
    IRModule*               module,
    IRFunc*                 func,
    DiagnosticSink*         sink,
    ExtensionUsageTracker*  extensionUsageTracker)
{
    auto layoutDecoration = func->findDecoration<IRLayoutDecoration>();
    SLANG_ASSERT(layoutDecoration);

    auto entryPointLayout = as<EntryPointLayout>(layoutDecoration->getLayout());
    SLANG_ASSERT(entryPointLayout);

    GLSLLegalizationContext context;
    context.session = session;
    context.stage = entryPointLayout->profile.GetStage();
    context.sink = sink;
    context.extensionUsageTracker = extensionUsageTracker;

    Stage stage = entryPointLayout->profile.GetStage();

    // We require that the entry-point function has no uses,
    // because otherwise we'd invalidate the signature
    // at all existing call sites.
    //
    // TODO: the right thing to do here is to split any
    // function that both gets called as an entry point
    // and as an ordinary function.
    SLANG_ASSERT(!func->firstUse);

    // We create a dummy IR builder, since some of
    // the functions require it.
    //
    // TODO: make some of these free functions...
    //
    SharedIRBuilder shared;
    shared.module = module;
    shared.session = session;
    IRBuilder builder;
    builder.sharedBuilder = &shared;
    builder.setInsertInto(func);

    context.builder = &builder;

    // We will start by looking at the return type of the
    // function, because that will enable us to do an
    // early-out check to avoid more work.
    //
    // Specifically, we need to check if the function has
    // a `void` return type, because there is no work
    // to be done on its return value in that case.
    auto resultType = func->getResultType();
    if(as<IRVoidType>(resultType))
    {
        // In this case, the function doesn't return a value
        // so we don't need to transform its `return` sites.
        //
        // We can also use this opportunity to quickly
        // check if the function has any parameters, and if
        // it doesn't use the chance to bail out immediately.
        if( func->getParamCount() == 0 )
        {
            // This function is already legal for GLSL
            // (at least in terms of parameter/result signature),
            // so we won't bother doing anything at all.
            return;
        }

        // If the function does have parameters, then we need
        // to let the logic later in this function handle them.
    }
    else
    {
        // Function returns a value, so we need
        // to introduce a new global variable
        // to hold that value, and then replace
        // any `returnVal` instructions with
        // code to write to that variable.

        auto resultGlobal = createGLSLGlobalVaryings(
            &context,
            &builder,
            resultType,
            entryPointLayout->resultLayout,
            LayoutResourceKind::VaryingOutput,
            stage);

        for( auto bb = func->getFirstBlock(); bb; bb = bb->getNextBlock() )
        {
            // TODO: This is silly, because we are looking at every instruction,
            // when we know that a `returnVal` should only ever appear as a
            // terminator...
            for( auto ii = bb->getFirstInst(); ii; ii = ii->getNextInst() )
            {
                if(ii->op != kIROp_ReturnVal)
                    continue;

                IRReturnVal* returnInst = (IRReturnVal*) ii;
                IRInst* returnValue = returnInst->getVal();

                // Make sure we add these instructions to the right block
                builder.setInsertInto(bb);

                // Write to our global variable(s) from the value being returned.
                assign(&builder, resultGlobal, ScalarizedVal::value(returnValue));

                // Emit a `returnVoid` to end the block
                auto returnVoid = builder.emitReturn();

                // Remove the old `returnVal` instruction.
                returnInst->removeAndDeallocate();

                // Make sure to resume our iteration at an
                // appropriate instruciton, since we deleted
                // the one we had been using.
                ii = returnVoid;
            }
        }
    }

    // Next we will walk through any parameters of the entry-point function,
    // and turn them into global variables.
    if( auto firstBlock = func->getFirstBlock() )
    {
        // Any initialization code we insert for parameters needs
        // to be at the start of the "ordinary" instructions in the block:
        builder.setInsertBefore(firstBlock->getFirstOrdinaryInst());

        for( auto pp = firstBlock->getFirstParam(); pp; pp = pp->getNextParam() )
        {
            // We assume that the entry-point parameters will all have
            // layout information attached to them, which is kept up-to-date
            // by any transformations affecting the parameter list.
            //
            auto paramLayoutDecoration = pp->findDecoration<IRLayoutDecoration>();
            SLANG_ASSERT(paramLayoutDecoration);
            auto paramLayout = as<VarLayout>(paramLayoutDecoration->getLayout());
            SLANG_ASSERT(paramLayout);

            legalizeEntryPointParameterForGLSL(
                &context,
                func,
                pp,
                paramLayout);
        }

        // At this point we should have eliminated all uses of the
        // parameters of the entry block. Also, our control-flow
        // rules mean that the entry block cannot be the target
        // of any branches in the code, so there can't be
        // any control-flow ops that try to match the parameter
        // list.
        //
        // We can safely go through and destroy the parameters
        // themselves, and then clear out the parameter list.

        for( auto pp = firstBlock->getFirstParam(); pp; )
        {
            auto next = pp->getNextParam();
            pp->removeAndDeallocate();
            pp = next;
        }
    }

    // Finally, we need to patch up the type of the entry point,
    // because it is no longer accurate.

    IRFuncType* voidFuncType = builder.getFuncType(
        0,
        nullptr,
        builder.getVoidType());
    func->setFullType(voidFuncType);

    // TODO: we should technically be constructing
    // a new `EntryPointLayout` here to reflect
    // the way that things have been moved around.
}

} // namespace Slang
