// slang-ir-glsl-legalize.h
#pragma once

namespace Slang
{

class DiagnosticSink;
class Session;

class ExtensionUsageTracker;

struct IRFunc;
struct IRModule;

void legalizeEntryPointForGLSL(
    Session*                session,
    IRModule*               module,
    IRFunc*                 func,
    DiagnosticSink*         sink,
    ExtensionUsageTracker*  extensionUsageTracker);

}
