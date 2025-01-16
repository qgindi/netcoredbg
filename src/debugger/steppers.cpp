// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <unordered_set>
#include "debugger/stepper_simple.h"
#include "debugger/stepper_async.h"
#include "debugger/steppers.h"
#include "metadata/attributes.h"
#include "utils/utf.h"

#include "Au.h"

namespace netcoredbg
{

// From ECMA-335
static const std::unordered_set<WSTRING> g_operatorMethodNames
{
// Cast operators
    W("op_Implicit"),
    W("op_Explicit"),
// Unary operators
    W("op_Decrement"),                    // --
    W("op_Increment"),                    // ++
    W("op_UnaryNegation"),                // - (unary)
    W("op_UnaryPlus"),                    // + (unary)
    W("op_LogicalNot"),                   // !
    W("op_True"),                         // Not defined
    W("op_False"),                        // Not defined
    W("op_AddressOf"),                    // & (unary)
    W("op_OnesComplement"),               // ~
    W("op_PointerDereference"),           // * (unary)
// Binary operators
    W("op_Addition"),                     // + (binary)
    W("op_Subtraction"),                  // - (binary)
    W("op_Multiply"),                     // * (binary)
    W("op_Division"),                     // /
    W("op_Modulus"),                      // %
    W("op_ExclusiveOr"),                  // ^
    W("op_BitwiseAnd"),                   // & (binary)
    W("op_BitwiseOr"),                    // |
    W("op_LogicalAnd"),                   // &&
    W("op_LogicalOr"),                    // ||
    W("op_Assign"),                       // Not defined (= is not the same)
    W("op_LeftShift"),                    // <<
    W("op_RightShift"),                   // >>
    W("op_SignedRightShift"),             // Not defined
    W("op_UnsignedRightShift"),           // Not defined
    W("op_Equality"),                     // ==
    W("op_GreaterThan"),                  // >
    W("op_LessThan"),                     // <
    W("op_Inequality"),                   // !=
    W("op_GreaterThanOrEqual"),           // >=
    W("op_LessThanOrEqual"),              // <=
    W("op_UnsignedRightShiftAssignment"), // Not defined
    W("op_MemberSelection"),              // ->
    W("op_RightShiftAssignment"),         // >>=
    W("op_MultiplicationAssignment"),     // *=
    W("op_PointerToMemberSelection"),     // ->*
    W("op_SubtractionAssignment"),        // -=
    W("op_ExclusiveOrAssignment"),        // ^=
    W("op_LeftShiftAssignment"),          // <<=
    W("op_ModulusAssignment"),            // %=
    W("op_AdditionAssignment"),           // +=
    W("op_BitwiseAndAssignment"),         // &=
    W("op_BitwiseOrAssignment"),          // |=
    W("op_Comma"),                        // ,
    W("op_DivisionAssignment")            // /=
};

HRESULT Steppers::SetupStep(ICorDebugThread *pThread, IDebugger::StepType stepType)
{
    HRESULT Status;
    m_filteredPrevStep = false;
    m_initialStepType = stepType;

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pThread->GetProcess(&pProcess));
    DisableAllSteppers(pProcess);

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    ULONG32 ilOffset;
    //IfFailRet(m_sharedModules->GetFrameILAndSequencePoint(pFrame, ilOffset, m_StepStartSP));
    if (FAILED(m_sharedModules->GetFrameILAndSequencePoint(pFrame, ilOffset, m_StepStartSP))) m_StepStartSP.offset = -1; //Au: would fail to step when paused at exception thrown in non-user code

    IfFailRet(m_asyncStepper->SetupStep(pThread, stepType));
    if (Status == S_OK) // S_FALSE = setup simple stepper
        return S_OK;

    return m_simpleStepper->SetupStep(pThread, stepType);
}

HRESULT Steppers::ManagedCallbackBreakpoint(ICorDebugAppDomain *pAppDomain, ICorDebugThread *pThread)
{
    HRESULT Status;
    // Check async stepping related breakpoints first, since user can't setup breakpoints to await block yield or resume offsets manually,
    // so, async stepping related breakpoints not a part of any user breakpoints related data (that will be checked in separate thread. see code below).
    IfFailRet(m_asyncStepper->ManagedCallbackBreakpoint(pThread));
    if (Status == S_OK) // S_FALSE - no error, but steppers not affect on callback
        return S_OK;

    return m_simpleStepper->ManagedCallbackBreakpoint(pAppDomain, pThread);
}

HRESULT Steppers::ManagedCallbackStepComplete(ICorDebugThread *pThread, CorDebugStepReason reason)
{
    HRESULT Status;

    ToRelease<ICorDebugFrame> iCorFrame;
    IfFailRet(pThread->GetActiveFrame(&iCorFrame));
    if (iCorFrame == nullptr)
        return E_FAIL;

    ToRelease<ICorDebugFunction> iCorFunction;
    IfFailRet(iCorFrame->GetFunction(&iCorFunction));
    mdMethodDef methodDef;
    IfFailRet(iCorFunction->GetToken(&methodDef));
    ToRelease<ICorDebugClass> iCorClass;
    IfFailRet(iCorFunction->GetClass(&iCorClass));
    mdTypeDef typeDef;
    IfFailRet(iCorClass->GetToken(&typeDef));
    ToRelease<ICorDebugModule> iCorModule;
    IfFailRet(iCorFunction->GetModule(&iCorModule));
    ToRelease<IUnknown> iUnknown;
    IfFailRet(iCorModule->GetMetaDataInterface(IID_IMetaDataImport, &iUnknown));
    ToRelease<IMetaDataImport> iMD;
    IfFailRet(iUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &iMD));

    auto methodShouldBeFltered = [&]() -> bool
    {
        // In case stepping by method code lines or return to caller, don't check filtering (don't need to):
        // 1) filtering check for this method was already "passed" or 2) execution was stopped at breakpoint inside method or its callee.
        if (reason != CorDebugStepReason::STEP_CALL)
            return false;

        ULONG nameLen;
        WCHAR szFunctionName[mdNameLen] = {0};
        if (SUCCEEDED(iMD->GetMethodProps(methodDef, nullptr, szFunctionName, _countof(szFunctionName),
                                          &nameLen, nullptr, nullptr, nullptr, nullptr, nullptr)))
        {
            if (g_operatorMethodNames.find(szFunctionName) != g_operatorMethodNames.end())
                return true;
        }

        mdProperty propertyDef;
        ULONG numProperties = 0;
        HCORENUM propEnum = NULL;
        while(SUCCEEDED(iMD->EnumProperties(&propEnum, typeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
        {
            mdMethodDef mdSetter;
            mdMethodDef mdGetter;
            if (SUCCEEDED(iMD->GetPropertyProps(propertyDef, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr, &mdSetter, &mdGetter, nullptr, 0, nullptr)))
            {
                if (mdSetter != methodDef && mdGetter != methodDef)
                    continue;

                iMD->CloseEnum(propEnum);
                return true;
            }
        }
        iMD->CloseEnum(propEnum);

        return false;
    };

    // https://docs.microsoft.com/en-us/visualstudio/debugger/navigating-through-code-with-the-debugger?view=vs-2019#BKMK_Step_into_properties_and_operators_in_managed_code
    // The debugger steps over properties and operators in managed code by default. In most cases, this provides a better debugging experience.
    if (m_stepFiltering && methodShouldBeFltered())
    {
        IfFailRet(m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_OUT));
        m_filteredPrevStep = true;
        return S_OK;
    }

    bool filteredPrevStep = m_filteredPrevStep;
    m_filteredPrevStep = false;

    // Same behaviour as MS vsdbg and MSVS C# debugger have - step only for code with PDB loaded (no matter JMC enabled or not by user).
    ULONG32 ipOffset = 0;
    ULONG32 ilNextUserCodeOffset = 0;
    bool noUserCodeFound = false; // Must be initialized with `false`, since GetFrameILAndNextUserCodeILOffset call could be failed before delegate call.
    if (SUCCEEDED(Status = m_sharedModules->GetFrameILAndNextUserCodeILOffset(iCorFrame, ipOffset, ilNextUserCodeOffset, &noUserCodeFound)))
    {
        if (reason == CorDebugStepReason::STEP_NORMAL)
        {
            if (ipOffset != ilNextUserCodeOffset)
            {
                // Step completed on some compiler generated (not user) code inside user code (for example, `finally` block related code)
                IfFailRet(m_simpleStepper->SetupStep(pThread, m_initialStepType));
                return S_OK;
            }
            else
            {
                // Step completed on same location in source as it was started, this happens when some user code block have several
                // SequencePoints for same line (for example, `using` related code could mix user/compiler generated code for same line).
                ULONG32 ilOffset;
                Modules::SequencePoint sp;
                IfFailRet(m_sharedModules->GetFrameILAndSequencePoint(iCorFrame, ilOffset, sp));
                if (sp.startLine == m_StepStartSP.startLine &&
                    sp.startColumn == m_StepStartSP.startColumn &&
                    sp.endLine == m_StepStartSP.endLine &&
                    sp.endColumn == m_StepStartSP.endColumn &&
                    sp.document == m_StepStartSP.document)
                {
                    IfFailRet(m_simpleStepper->SetupStep(pThread, m_initialStepType));
                    return S_OK;
                }
            }
        }
        // Current IL offset less than IL offset of next close user code line (for example, step-in into async method)
        else if (reason == CorDebugStepReason::STEP_CALL && ipOffset < ilNextUserCodeOffset)
        {
            IfFailRet(m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_OVER));
            return S_OK;
        }
        // was return from filtered method
        else if (reason == CorDebugStepReason::STEP_RETURN && filteredPrevStep)
        {
            IfFailRet(m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_IN));
            return S_OK;
        }
    }
    else if (noUserCodeFound)
    {
        IfFailRet(m_simpleStepper->SetupStep(pThread, m_initialStepType));
        // In case step-in will return from method and no user code was called in user module, step-in again.
        m_filteredPrevStep = true;
        return S_OK;
    }
    else // Note, in case JMC enabled step, ManagedCallbackStepComplete() called only for user module code.
        return Status;

    // Care about attributes for "JMC disabled" case.
    if (!m_justMyCode)
    {
        static std::vector<std::string> attrNames{DebuggerAttribute::Hidden, DebuggerAttribute::StepThrough};

        if (HasAttribute(iMD, typeDef, DebuggerAttribute::StepThrough) || HasAttribute(iMD, methodDef, attrNames))
        {
            IfFailRet(m_simpleStepper->SetupStep(pThread, IDebugger::StepType::STEP_IN));
            // In case step-in will return from filtered method and no user code was called, step-in again.
            m_filteredPrevStep = true;

            return S_OK;
        }
    }

    // Note, reset steppers right before return only.
    m_simpleStepper->ManagedCallbackStepComplete();
    m_asyncStepper->ManagedCallbackStepComplete();

    return S_FALSE; // S_FALSE - no error, but steppers not affect on callback
}

HRESULT Steppers::DisableAllSteppers(ICorDebugProcess *pProcess)
{
    HRESULT Status;
    IfFailRet(m_simpleStepper->DisableAllSteppers(pProcess));
    return m_asyncStepper->DisableAllSteppers();
}

HRESULT Steppers::DisableAllSteppers(ICorDebugAppDomain *pAppDomain)
{
    HRESULT Status;
    ToRelease<ICorDebugProcess> iCorProcess;
    IfFailRet(pAppDomain->GetProcess(&iCorProcess));
    return DisableAllSteppers(iCorProcess);
}

HRESULT Steppers::DisableAllSimpleSteppers(ICorDebugProcess *pProcess)
{
    return m_simpleStepper->DisableAllSteppers(pProcess);
}

void Steppers::SetJustMyCode(bool enable)
{
    m_justMyCode = enable;
    m_simpleStepper->SetJustMyCode(enable);
}

void Steppers::SetStepFiltering(bool enable)
{
    m_stepFiltering = enable;
}

} // namespace netcoredbg
