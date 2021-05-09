// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include <memory>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include "debugger/evaluator.h"
#include "debugger/evalwaiter.h"
#include "debugger/frames.h"
#include "utils/utf.h"
#include "metadata/modules.h"
#include "metadata/typeprinter.h"
#include "valueprint.h"

namespace netcoredbg
{

static HRESULT ParseIndices(const std::string &s, std::vector<ULONG32> &indices)
{
    indices.clear();
    ULONG32 currentVal = 0;
    bool digit_detected = false;

    static const std::string digits("0123456789");

    for (char c : s)
    {
        std::size_t digit = digits.find(c);
        if (digit == std::string::npos)
        {
            switch(c)
            {
                case ' ': continue;
                case ',':
                case ']':
                    if (!digit_detected)
                        return E_FAIL;
                    indices.push_back(currentVal);
                    if (c == ']')
                        return S_OK;
                    currentVal = 0;
                    digit_detected = false;
                    continue;
                default:
                    return E_FAIL;
            }
        }
        currentVal *= 10;
        currentVal += (ULONG32)digit;
        digit_detected = true;
    }

    // Something wrong, we should never arrived this point.
    return E_FAIL;
}

HRESULT Evaluator::GetFieldOrPropertyWithName(ICorDebugThread *pThread,
                                              FrameLevel frameLevel,
                                              ICorDebugValue *pInputValue,
                                              ValueKind valueKind,
                                              const std::string &name,
                                              ICorDebugValue **ppResultValue,
                                              int evalFlags)
{
    HRESULT Status;

    ToRelease<ICorDebugValue> pResult;

    if (name.empty())
        return E_FAIL;

    if (name.back() == ']')
    {
        if (valueKind == ValueIsClass)
            return E_FAIL;

        BOOL isNull = FALSE;
        ToRelease<ICorDebugValue> pValue;

        IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

        if (isNull)
            return E_FAIL;

        ToRelease<ICorDebugArrayValue> pArrayVal;
        IfFailRet(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayVal));

        ULONG32 nRank;
        IfFailRet(pArrayVal->GetRank(&nRank));

        std::vector<ULONG32> indices;
        IfFailRet(ParseIndices(name, indices));

        if (indices.size() != nRank)
            return E_FAIL;

        ToRelease<ICorDebugValue> pArrayElement;
        return pArrayVal->GetElement(static_cast<uint32_t>(indices.size()), indices.data(), ppResultValue);
    }

    IfFailRet(WalkMembers(pInputValue, pThread, frameLevel, [&](
        mdMethodDef mdGetter,
        ICorDebugModule *pModule,
        ICorDebugType *pType,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &memberName)
    {
        if (is_static && valueKind == ValueIsVariable)
            return S_OK;
        if (!is_static && valueKind == ValueIsClass)
            return S_OK;

        if (pResult)
            return S_OK;
        if (memberName != name)
            return S_OK;

        if (mdGetter != mdMethodDefNil)
        {
            ToRelease<ICorDebugFunction> pFunc;
            if (SUCCEEDED(pModule->GetFunctionFromToken(mdGetter, &pFunc)))
            {
                if (is_static)
                    EvalFunction(pThread, pFunc, &pType, 1, nullptr, 0, &pResult, evalFlags);
                else
                    EvalFunction(pThread, pFunc, &pType, 1, &pInputValue, 1, &pResult, evalFlags);
            }
        }
        else
        {
            if (pValue)
                pValue->AddRef();

            pResult = pValue;
        }

        return S_OK;
    }));

    if (!pResult)
        return E_FAIL;

    *ppResultValue = pResult.Detach();
    return S_OK;
}


static mdTypeDef GetTypeTokenForName(IMetaDataImport *pMD, mdTypeDef tkEnclosingClass, const std::string &name)
{
    mdTypeDef typeToken = mdTypeDefNil;
    pMD->FindTypeDefByName(reinterpret_cast<LPCWSTR>(to_utf16(name).c_str()), tkEnclosingClass, &typeToken);
    return typeToken;
}

static HRESULT GetMethodToken(IMetaDataImport *pMD, mdTypeDef cl, const WCHAR *methodName)
{
    ULONG numMethods = 0;
    HCORENUM mEnum = NULL;
    mdMethodDef methodDef = mdTypeDefNil;
    pMD->EnumMethodsWithName(&mEnum, cl, methodName, &methodDef, 1, &numMethods);
    pMD->CloseEnum(mEnum);
    return methodDef;
}

HRESULT Evaluator::FindFunction(ICorDebugModule *pModule,
                            const WCHAR *typeName,
                            const WCHAR *methodName,
                            ICorDebugFunction **ppFunction)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdTypeDef typeDef = mdTypeDefNil;

    IfFailRet(pMD->FindTypeDefByName(typeName, mdTypeDefNil, &typeDef));

    mdMethodDef methodDef = GetMethodToken(pMD, typeDef, methodName);

    if (methodDef == mdMethodDefNil)
        return E_FAIL;

    return pModule->GetFunctionFromToken(methodDef, ppFunction);
}

void Evaluator::Cleanup()
{
    m_pSuppressFinalizeMutex.lock();
    if (m_pSuppressFinalize)
        m_pSuppressFinalize.Free();
    m_pSuppressFinalizeMutex.unlock();

    m_typeObjectCacheMutex.lock();
    m_typeObjectCache.clear();
    m_typeObjectCacheMutex.unlock();
}

HRESULT Evaluator::FollowFields(ICorDebugThread *pThread,
                                FrameLevel frameLevel,
                                ICorDebugValue *pValue,
                                ValueKind valueKind,
                                const std::vector<std::string> &parts,
                                int nextPart,
                                ICorDebugValue **ppResult,
                                int evalFlags)
{
    HRESULT Status;

    // Note, in case of (nextPart == parts.size()) result is pValue itself, so, we ok here.
    if (nextPart > (int)parts.size())
        return E_FAIL;

    pValue->AddRef();
    ToRelease<ICorDebugValue> pResultValue(pValue);
    for (int i = nextPart; i < (int)parts.size(); i++)
    {
        ToRelease<ICorDebugValue> pClassValue(std::move(pResultValue));
        IfFailRet(GetFieldOrPropertyWithName(
            pThread, frameLevel, pClassValue, valueKind, parts[i], &pResultValue, evalFlags));  // NOLINT(clang-analyzer-cplusplus.Move)
        valueKind = ValueIsVariable; // we can only follow through instance fields
    }

    *ppResult = pResultValue.Detach();
    return S_OK;
}

static std::vector<std::string> ParseExpression(const std::string &expression)
{
    std::vector<std::string> result;
    int paramDepth = 0;

    result.push_back("");

    for (char c : expression)
    {
        switch(c)
        {
            case '.':
            case '[':
                if (paramDepth == 0)
                {
                    result.push_back("");
                    continue;
                }
                break;
            case '<':
                paramDepth++;
                break;
            case '>':
                paramDepth--;
                break;
            case ' ':
                continue;
            default:
                break;
        }
        result.back() += c;
    }
    return result;
}

static std::vector<std::string> ParseType(const std::string &expression, std::vector<int> &ranks)
{
    std::vector<std::string> result;
    int paramDepth = 0;

    result.push_back("");

    for (char c : expression)
    {
        switch(c)
        {
            case '.':
                if (paramDepth == 0)
                {
                    result.push_back("");
                    continue;
                }
                break;
            case '[':
                if (paramDepth == 0)
                {
                    ranks.push_back(1);
                    continue;
                }
                break;
            case ']':
                if (paramDepth == 0)
                    continue;
                break;
            case ',':
                if (paramDepth == 0)
                {
                    if (!ranks.empty())
                        ranks.back()++;
                    continue;
                }
                break;
            case '<':
                paramDepth++;
                break;
            case '>':
                paramDepth--;
                break;
            case ' ':
                continue;
            default:
                break;
        }
        result.back() += c;
    }
    return result;
}

static std::vector<std::string> ParseGenericParams(const std::string &part, std::string &typeName)
{
    std::vector<std::string> result;

    std::size_t start = part.find('<');
    if (start == std::string::npos)
    {
        typeName = part;
        return result;
    }

    int paramDepth = 0;
    bool inArray = false;

    result.push_back("");

    for (std::size_t i = start; i < part.size(); i++)
    {
        char c = part.at(i);
        switch(c)
        {
            case ',':
                if (paramDepth == 1 && !inArray)
                {
                    result.push_back("");
                    continue;
                }
                break;
            case '[':
                inArray = true;
                break;
            case ']':
                inArray = false;
                break;
            case '<':
                paramDepth++;
                if (paramDepth == 1) continue;
                break;
            case '>':
                paramDepth--;
                if (paramDepth == 0) continue;
                break;
            default:
                break;
        }
        result.back() += c;
    }
    typeName = part.substr(0, start) + '`' + std::to_string(result.size());
    return result;
}

static std::vector<std::string> GatherParameters(const std::vector<std::string> &parts, int indexEnd)
{
    std::vector<std::string> result;
    for (int i = 0; i < indexEnd; i++)
    {
        std::string typeName;
        std::vector<std::string> params = ParseGenericParams(parts[i], typeName);
        result.insert(result.end(), params.begin(), params.end());
    }
    return result;
}

static HRESULT FindTypeInModule(ICorDebugModule *pModule, const std::vector<std::string> &parts, int &nextPart, mdTypeDef &typeToken)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    std::string currentTypeName;

    // Search for type in module
    for (int i = nextPart; i < (int)parts.size(); i++)
    {
        std::string name;
        ParseGenericParams(parts[i], name);
        currentTypeName += (currentTypeName.empty() ? "" : ".") + name;

        typeToken = GetTypeTokenForName(pMD, mdTypeDefNil, currentTypeName);
        if (typeToken != mdTypeDefNil)
        {
            nextPart = i + 1;
            break;
        }
    }

    if (typeToken == mdTypeDefNil) // type not found, continue search in next module
        return E_FAIL;

    // Resolve nested class
    for (int j = nextPart; j < (int)parts.size(); j++)
    {
        std::string name;
        ParseGenericParams(parts[j], name);
        mdTypeDef classToken = GetTypeTokenForName(pMD, typeToken, name);
        if (classToken == mdTypeDefNil)
            break;
        typeToken = classToken;
        nextPart = j + 1;
    }

    return S_OK;
}

HRESULT Evaluator::GetType(
    const std::string &typeName,
    ICorDebugThread *pThread,
    ICorDebugType **ppType)
{
    HRESULT Status;
    std::vector<int> ranks;
    std::vector<std::string> classParts = ParseType(typeName, ranks);
    if (classParts.size() == 1)
        classParts[0] = TypePrinter::RenameToSystem(classParts[0]);

    ToRelease<ICorDebugType> pType;
    int nextClassPart = 0;
    IfFailRet(FindType(classParts, nextClassPart, pThread, nullptr, &pType));

    if (!ranks.empty())
    {
        ToRelease<ICorDebugAppDomain2> pAppDomain2;
        ToRelease<ICorDebugAppDomain> pAppDomain;
        IfFailRet(pThread->GetAppDomain(&pAppDomain));
        IfFailRet(pAppDomain->QueryInterface(IID_ICorDebugAppDomain2, (LPVOID*) &pAppDomain2));

        for (auto irank = ranks.rbegin(); irank != ranks.rend(); ++irank)
        {
            ToRelease<ICorDebugType> pElementType(std::move(pType));
            IfFailRet(pAppDomain2->GetArrayOrPointerType(
                *irank > 1 ? ELEMENT_TYPE_ARRAY : ELEMENT_TYPE_SZARRAY,
                *irank,
                pElementType,
                &pType));        // NOLINT(clang-analyzer-cplusplus.Move)
        }
    }

    *ppType = pType.Detach();
    return S_OK;
}

HRESULT Evaluator::ResolveParameters(
    const std::vector<std::string> &params,
    ICorDebugThread *pThread,
    std::vector< ToRelease<ICorDebugType> > &types)
{
    HRESULT Status;
    for (auto &p : params)
    {
        ICorDebugType *tmpType;
        IfFailRet(GetType(p, pThread, &tmpType));
        types.emplace_back(tmpType);
    }
    return S_OK;
}

HRESULT Evaluator::FindType(
    const std::vector<std::string> &parts,
    int &nextPart,
    ICorDebugThread *pThread,
    ICorDebugModule *pModule,
    ICorDebugType **ppType,
    ICorDebugModule **ppModule)
{
    HRESULT Status;

    if (pModule)
        pModule->AddRef();
    ToRelease<ICorDebugModule> pTypeModule(pModule);

    mdTypeDef typeToken = mdTypeDefNil;

    if (!pTypeModule)
    {
        m_sharedModules->ForEachModule([&](ICorDebugModule *pModule)->HRESULT {
            if (typeToken != mdTypeDefNil) // already found
                return S_OK;

            if (SUCCEEDED(FindTypeInModule(pModule, parts, nextPart, typeToken)))
            {
                pModule->AddRef();
                pTypeModule = pModule;
            }
            return S_OK;
        });
    }
    else
    {
        FindTypeInModule(pTypeModule, parts, nextPart, typeToken);
    }

    if (typeToken == mdTypeDefNil)
        return E_FAIL;

    if (ppType)
    {
        std::vector<std::string> params = GatherParameters(parts, nextPart);
        std::vector< ToRelease<ICorDebugType> > types;
        IfFailRet(ResolveParameters(params, pThread, types));

        ToRelease<ICorDebugClass> pClass;
        IfFailRet(pTypeModule->GetClassFromToken(typeToken, &pClass));

        ToRelease<ICorDebugClass2> pClass2;
        IfFailRet(pClass->QueryInterface(IID_ICorDebugClass2, (LPVOID*) &pClass2));

        ToRelease<IUnknown> pMDUnknown;
        IfFailRet(pTypeModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
        ToRelease<IMetaDataImport> pMD;
        IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

        DWORD flags;
        ULONG nameLen;
        mdToken tkExtends;
        IfFailRet(pMD->GetTypeDefProps(typeToken, nullptr, 0, &nameLen, &flags, &tkExtends));

        std::string eTypeName;
        IfFailRet(TypePrinter::NameForToken(tkExtends, pMD, eTypeName, true, nullptr));

        bool isValueType = eTypeName == "System.ValueType" || eTypeName == "System.Enum";
        CorElementType et = isValueType ? ELEMENT_TYPE_VALUETYPE : ELEMENT_TYPE_CLASS;

        ToRelease<ICorDebugType> pType;
        IfFailRet(pClass2->GetParameterizedType(et, static_cast<uint32_t>(types.size()), (ICorDebugType **)types.data(), &pType));

        *ppType = pType.Detach();
    }
    if (ppModule)
        *ppModule = pTypeModule.Detach();

    return S_OK;
}

HRESULT Evaluator::FollowNested(ICorDebugThread *pThread,
                                FrameLevel frameLevel,
                                const std::string &methodClass,
                                const std::vector<std::string> &parts,
                                ICorDebugValue **ppResult,
                                int evalFlags)
{
    HRESULT Status;

    std::vector<int> ranks;
    std::vector<std::string> classParts = ParseType(methodClass, ranks);
    int nextClassPart = 0;
    int partsNum = (int)parts.size() -1;
    std::vector<std::string> fieldName {parts.back()};
    std::vector<std::string> fullpath;

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(FindType(classParts, nextClassPart, pThread, nullptr, nullptr, &pModule));

    bool trim = false;
    while (!classParts.empty())
    {
        ToRelease<ICorDebugType> pType;
        nextClassPart = 0;
        if (trim)
            classParts.pop_back();

        fullpath = classParts;
        for (int i = 0; i < partsNum; i++)
            fullpath.push_back(parts[i]);

        if (FAILED(FindType(fullpath, nextClassPart, pThread, pModule, &pType)))  // NOLINT(clang-analyzer-cplusplus.Move)
            break;

        if(nextClassPart < (int)fullpath.size())
        {
            // try to check non-static fields inside a static member
            std::vector<std::string> staticName;
            for (int i = nextClassPart; i < (int)fullpath.size(); i++)
            {
                staticName.push_back(fullpath[i]);
            }
            staticName.push_back(fieldName[0]);
            ToRelease<ICorDebugValue> pTypeObject;
            if (S_OK == CreatTypeObjectStaticConstructor(pThread, pType, &pTypeObject))
            {
                if (SUCCEEDED(FollowFields(pThread, frameLevel, pTypeObject, ValueIsClass, staticName, 0, ppResult, evalFlags)))
                    return S_OK;
            }
            trim = true;
            continue;
        }

        ToRelease<ICorDebugValue> pTypeObject;
        IfFailRet(CreatTypeObjectStaticConstructor(pThread, pType, &pTypeObject));
        if (Status == S_OK && // type have static members (S_FALSE if type don't have static members)
            SUCCEEDED(FollowFields(pThread, frameLevel, pTypeObject, ValueIsClass, fieldName, 0, ppResult, evalFlags)))
            return S_OK;

        trim = true;
    }

    return E_FAIL;
}

HRESULT Evaluator::EvalExpr(ICorDebugThread *pThread,
                            FrameLevel frameLevel,
                            const std::string &expression,
                            ICorDebugValue **ppResult,
                            int evalFlags)
{
    HRESULT Status;

    // TODO: support generics
    std::vector<std::string> parts = ParseExpression(expression);

    if (parts.empty())
        return E_FAIL;

    int nextPart = 0;

    ToRelease<ICorDebugValue> pResultValue;
    ToRelease<ICorDebugValue> pThisValue;

    if (parts.at(nextPart) == "$exception")
    {
        pThread->GetCurrentException(&pResultValue);
    }
    else
    {
        // Note, we use E_ABORT error code as fast way to exit from stack vars walk routine here.
        if (FAILED(Status = WalkStackVars(pThread, frameLevel, [&](ICorDebugValue *pValue,
                                                                   const std::string &name) -> HRESULT
        {
            if (name == "this" && pValue)
            {
                pValue->AddRef();
                pThisValue = pValue;
                if (name == parts.at(nextPart))
                    return E_ABORT; // Fast way to exit from stack vars walk routine.
            }
            else if (name == parts.at(nextPart) && pValue)
            {
                pValue->AddRef();
                pResultValue = pValue;
                return E_ABORT; // Fast way to exit from stack vars walk routine.
            }

            return S_OK;
        })) && !pThisValue && !pResultValue) // Check, that we have fast exit instead of real error.
        {
            return Status;
        }
    }

    if (!pResultValue && pThisValue) // check this/this.*
    {
        if (parts[nextPart] == "this")
            nextPart++; // skip first part with "this" (we have it in pThisValue), check rest

        if (SUCCEEDED(FollowFields(pThread, frameLevel, pThisValue, ValueIsVariable, parts, nextPart, &pResultValue, evalFlags)))
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
    }

    if (!pResultValue) // check statics in nested classes
    {
        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
        if (pFrame == nullptr)
            return E_FAIL;

        std::string methodClass;
        std::string methodName;
        TypePrinter::GetTypeAndMethod(pFrame, methodClass, methodName);

        if (SUCCEEDED(FollowNested(pThread, frameLevel, methodClass, parts, &pResultValue, evalFlags)))
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
    }

    ValueKind valueKind;
    if (pResultValue)
    {
        nextPart++;
        if (nextPart == (int)parts.size())
        {
            *ppResult = pResultValue.Detach();
            return S_OK;
        }
        valueKind = ValueIsVariable;
    }
    else
    {
        ToRelease<ICorDebugType> pType;
        IfFailRet(FindType(parts, nextPart, pThread, nullptr, &pType));
        IfFailRet(CreatTypeObjectStaticConstructor(pThread, pType, &pResultValue));
        if (Status == S_FALSE) // type don't have static members, nothing explore here
            return E_INVALIDARG;
        valueKind = ValueIsClass;
    }

    ToRelease<ICorDebugValue> pValue(std::move(pResultValue));
    IfFailRet(FollowFields(pThread, frameLevel, pValue, valueKind, parts, nextPart, &pResultValue, evalFlags));

    *ppResult = pResultValue.Detach();

    return S_OK;
}

HRESULT Evaluator::CreateString(ICorDebugThread *pThread, const std::string &value, ICorDebugValue **ppNewString)
{
    auto value16t = to_utf16(value);
    return m_sharedEvalWaiter->WaitEvalResult(pThread, ppNewString,
        [&](ICorDebugEval *pEval) -> HRESULT
        {
            // Note, this code execution protected by EvalWaiter mutex.
            HRESULT Status;
            IfFailRet(pEval->NewString(value16t.c_str()));
            return S_OK;
        });
}

// Call managed function in debuggee process.
// [in] pThread - managed thread for evaluation;
// [in] pFunc - function to call;
// [in] ppArgsType - pointer to args Type array, could be nullptr;
// [in] ArgsTypeCount - size of args Type array;
// [in] ppArgsValue - pointer to args Value array, could be nullptr;
// [in] ArgsValueCount - size of args Value array;
// [out] ppEvalResult - return value;
// [in] evalFlags - evaluation flags.
HRESULT Evaluator::EvalFunction(
    ICorDebugThread *pThread,
    ICorDebugFunction *pFunc,
    ICorDebugType **ppArgsType,
    ULONG32 ArgsTypeCount,
    ICorDebugValue **ppArgsValue,
    ULONG32 ArgsValueCount,
    ICorDebugValue **ppEvalResult,
    int evalFlags)
{
    assert((!ppArgsType && ArgsTypeCount == 0) || (ppArgsType && ArgsTypeCount > 0));
    assert((!ppArgsValue && ArgsValueCount == 0) || (ppArgsValue && ArgsValueCount > 0));

    if (evalFlags & EVAL_NOFUNCEVAL)
        return S_OK;

    std::vector< ToRelease<ICorDebugType> > typeParams;
    // Reserve memory from the beginning, since typeParams will have ArgsTypeCount or more count of elements for sure.
    typeParams.reserve(ArgsTypeCount);

    for (ULONG32 i = 0; i < ArgsTypeCount; i++)
    {
        ToRelease<ICorDebugTypeEnum> pTypeEnum;
        if (SUCCEEDED(ppArgsType[i]->EnumerateTypeParameters(&pTypeEnum)))
        {
            ICorDebugType *curType;
            ULONG fetched = 0;
            while (SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
            {
                typeParams.emplace_back(curType);
            }
        }
    }

    return m_sharedEvalWaiter->WaitEvalResult(pThread, ppEvalResult,
        [&](ICorDebugEval *pEval) -> HRESULT
        {
            // Note, this code execution protected by EvalWaiter mutex.
            HRESULT Status;
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(Status = pEval2->CallParameterizedFunction(
                pFunc,
                static_cast<uint32_t>(typeParams.size()),
                (ICorDebugType **)typeParams.data(),
                ArgsValueCount,
                ppArgsValue));
            return S_OK;
        });
}

static bool TypeHaveStaticMembers(ICorDebugType *pType)
{
    HRESULT Status;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    mdTypeDef typeDef;
    IfFailRet(pClass->GetToken(&typeDef));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numFields = 0;
    HCORENUM hEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&hEnum, typeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        DWORD fieldAttr = 0;
        if (FAILED(pMD->GetFieldProps(fieldDef, nullptr, nullptr, 0, nullptr, &fieldAttr,
                                           nullptr, nullptr, nullptr, nullptr, nullptr)))
        {
            continue;
        }

        if (fieldAttr & fdStatic)
        {
            pMD->CloseEnum(hEnum);
            return true;
        }
    }
    pMD->CloseEnum(hEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, typeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdMethodDef mdGetter;
        if (FAILED(pMD->GetPropertyProps(propertyDef, nullptr, nullptr, 0, nullptr, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, &mdGetter, nullptr, 0, nullptr)))
        {
            continue;
        }

        DWORD getterAttr = 0;
        if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
        {
            continue;
        }

        if (getterAttr & mdStatic)
        {
            pMD->CloseEnum(propEnum);
            return true;
        }
    }
    pMD->CloseEnum(propEnum);

    return false;
}

HRESULT Evaluator::TryReuseTypeObjectFromCache(ICorDebugType *pType, ICorDebugValue **ppTypeObjectResult)
{
    std::lock_guard<std::mutex> lock(m_typeObjectCacheMutex);

    HRESULT Status;
    ToRelease<ICorDebugType2> iCorType2;
    IfFailRet(pType->QueryInterface(IID_ICorDebugType2, (LPVOID*) &iCorType2));

    COR_TYPEID typeID;
    IfFailRet(iCorType2->GetTypeID(&typeID));

    auto is_same = [&typeID](type_object_t &typeObject){ return typeObject.id.token1 == typeID.token1 && typeObject.id.token2 == typeID.token2; };
    auto it = std::find_if(m_typeObjectCache.begin(), m_typeObjectCache.end(), is_same);
    if (it == m_typeObjectCache.end())
        return E_FAIL;

    // Move data to begin, so, last used will be on front.
    if (it != m_typeObjectCache.begin())
        m_typeObjectCache.splice(m_typeObjectCache.begin(), m_typeObjectCache, it);

    if (ppTypeObjectResult)
    {
        // We don't check handle's status here, since we store only strong handles.
        // https://docs.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/cordebughandletype-enumeration
        // The handle is strong, which prevents an object from being reclaimed by garbage collection.
        return m_typeObjectCache.front().typeObject->QueryInterface(IID_ICorDebugValue, (LPVOID *)ppTypeObjectResult);
    }

    return S_OK;
}

HRESULT Evaluator::AddTypeObjectToCache(ICorDebugType *pType, ICorDebugValue *pTypeObject)
{
    std::lock_guard<std::mutex> lock(m_typeObjectCacheMutex);

    HRESULT Status;
    ToRelease<ICorDebugType2> iCorType2;
    IfFailRet(pType->QueryInterface(IID_ICorDebugType2, (LPVOID*) &iCorType2));

    COR_TYPEID typeID;
    IfFailRet(iCorType2->GetTypeID(&typeID));

    auto is_same = [&typeID](type_object_t &typeObject){ return typeObject.id.token1 == typeID.token1 && typeObject.id.token2 == typeID.token2; };
    auto it = std::find_if(m_typeObjectCache.begin(), m_typeObjectCache.end(), is_same);
    if (it != m_typeObjectCache.end())
        return S_OK;

    ToRelease<ICorDebugHandleValue> iCorHandleValue;
    IfFailRet(pTypeObject->QueryInterface(IID_ICorDebugHandleValue, (LPVOID *) &iCorHandleValue));

    CorDebugHandleType handleType;
    if (FAILED(iCorHandleValue->GetHandleType(&handleType)) ||
        handleType != HANDLE_STRONG)
        return E_FAIL;

    if (m_typeObjectCache.size() == m_typeObjectCacheSize)
    {
        // Re-use last list entry.
        m_typeObjectCache.back().id = typeID;
        m_typeObjectCache.back().typeObject.Free();
        m_typeObjectCache.back().typeObject = iCorHandleValue.Detach();
        assert(m_typeObjectCacheSize >= 2);
        m_typeObjectCache.splice(m_typeObjectCache.begin(), m_typeObjectCache, std::prev(m_typeObjectCache.end()));
    }
    else
        m_typeObjectCache.emplace_front(type_object_t{typeID, iCorHandleValue.Detach()});

    return S_OK;
}

HRESULT Evaluator::CreatTypeObjectStaticConstructor(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugValue **ppTypeObjectResult,
    bool DetectStaticMembers)
{
    HRESULT Status;

    CorElementType et;
    IfFailRet(pType->GetType(&et));

    if (et != ELEMENT_TYPE_CLASS && et != ELEMENT_TYPE_VALUETYPE)
        return S_OK;

    // Check cache first, before check type for static members.
    if (SUCCEEDED(TryReuseTypeObjectFromCache(pType, ppTypeObjectResult)))
        return S_OK;

    // Create type object only in case type have static members.
    // Note, for some cases we have static members check outside this method.
    if (DetectStaticMembers && !TypeHaveStaticMembers(pType))
        return S_FALSE;

    std::vector< ToRelease<ICorDebugType> > typeParams;
    ToRelease<ICorDebugTypeEnum> pTypeEnum;
    if (SUCCEEDED(pType->EnumerateTypeParameters(&pTypeEnum)))
    {
        ICorDebugType *curType;
        ULONG fetched = 0;
        while (SUCCEEDED(pTypeEnum->Next(1, &curType, &fetched)) && fetched == 1)
        {
            typeParams.emplace_back(curType);
        }
    }

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));

    ToRelease<ICorDebugValue> pTypeObject;
    IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, &pTypeObject,
        [&](ICorDebugEval *pEval) -> HRESULT
        {
            // Note, this code execution protected by EvalWaiter mutex.
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->NewParameterizedObjectNoConstructor(
                pClass,
                static_cast<uint32_t>(typeParams.size()),
                (ICorDebugType **)typeParams.data()
            ));
            return S_OK;
        }));

    if (et == ELEMENT_TYPE_CLASS)
    {
        std::lock_guard<std::mutex> lock(m_pSuppressFinalizeMutex);

        if (!m_pSuppressFinalize)
        {
            ToRelease<ICorDebugModule> pModule;
            IfFailRet(m_sharedModules->GetModuleWithName("System.Private.CoreLib.dll", &pModule));

            static const WCHAR gcName[] = W("System.GC");
            static const WCHAR suppressFinalizeMethodName[] = W("SuppressFinalize");
            IfFailRet(FindFunction(pModule, gcName, suppressFinalizeMethodName, &m_pSuppressFinalize));
        }

        if (!m_pSuppressFinalize)
            return E_FAIL;

        // Note, this call must ignore any eval flags.
        IfFailRet(EvalFunction(pThread, m_pSuppressFinalize, &pType, 1, pTypeObject.GetRef(), 1, nullptr, defaultEvalFlags));
    }

    AddTypeObjectToCache(pType, pTypeObject);

    if (ppTypeObjectResult)
        *ppTypeObjectResult = pTypeObject.Detach();

    return S_OK;
}

static HRESULT FindMethod(ICorDebugType *pType, const WCHAR *methodName, ICorDebugFunction **ppFunc)
{
    HRESULT Status;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));

    mdTypeDef currentTypeDef;
    IfFailRet(pClass->GetToken(&currentTypeDef));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    ULONG numMethods = 0;
    HCORENUM hEnum = NULL;
    mdMethodDef methodDef = mdMethodDefNil;

    pMD->EnumMethodsWithName(&hEnum, currentTypeDef, methodName, &methodDef, 1, &numMethods);
    pMD->CloseEnum(hEnum);

    if (numMethods == 1)
        return pModule->GetFunctionFromToken(methodDef, ppFunc);

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;

    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return E_FAIL;
        else if (baseTypeName != "object" && baseTypeName != "System.Object" && baseTypeName != "System.ValueType")
        {
            return FindMethod(pBaseType, methodName, ppFunc);
        }
    }

    return E_FAIL;
}

HRESULT Evaluator::getObjectByFunction(
    const std::string &func,
    ICorDebugThread *pThread,
    ICorDebugValue *pInValue,
    ICorDebugValue **ppOutValue,
    int evalFlags)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugValue2> pValue2;
    IfFailRet(pInValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *)&pValue2));
    ToRelease<ICorDebugType> pType;
    IfFailRet(pValue2->GetExactType(&pType));

    auto methodName = to_utf16(func);
    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(FindMethod(pType, methodName.c_str(), &pFunc));

    return EvalFunction(pThread, pFunc, pType.GetRef(), 1, &pInValue, 1, ppOutValue, evalFlags);
}

static void IncIndicies(std::vector<ULONG32> &ind, const std::vector<ULONG32> &dims)
{
    int i = static_cast<int32_t>(ind.size()) - 1;

    while (i >= 0)
    {
        ind[i] += 1;
        if (ind[i] < dims[i])
            return;
        ind[i] = 0;
        --i;
    }
}

static std::string IndiciesToStr(const std::vector<ULONG32> &ind, const std::vector<ULONG32> &base)
{
    const size_t ind_size = ind.size();
    if (ind_size < 1 || base.size() != ind_size)
        return std::string();

    std::ostringstream ss;
    const char *sep = "";
    for (size_t i = 0; i < ind_size; ++i)
    {
        ss << sep;
        sep = ", ";
        ss << (base[i] + ind[i]);
    }
    return ss.str();
}

HRESULT Evaluator::GetLiteralValue(
    ICorDebugThread *pThread,
    ICorDebugType *pType,
    ICorDebugModule *pModule,
    PCCOR_SIGNATURE pSignatureBlob,
    ULONG sigBlobLength,
    UVCP_CONSTANT pRawValue,
    ULONG rawValueLength,
    ICorDebugValue **ppLiteralValue)
{
    HRESULT Status = S_OK;

    if (!pRawValue || !pThread)
        return S_FALSE;

    CorSigUncompressCallingConv(pSignatureBlob);
    CorElementType underlyingType;
    CorSigUncompressElementType(pSignatureBlob, &underlyingType);

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    switch(underlyingType)
    {
        case ELEMENT_TYPE_OBJECT:
        {
            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
            IfFailRet(pEval->CreateValue(ELEMENT_TYPE_CLASS, nullptr, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_CLASS:
        {
            // Get token and create null reference
            mdTypeDef tk;
            CorSigUncompressElementType(pSignatureBlob);
            CorSigUncompressToken(pSignatureBlob, &tk);

            ToRelease<ICorDebugClass> pValueClass;
            IfFailRet(pModule->GetClassFromToken(tk, &pValueClass));

            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
            IfFailRet(pEval->CreateValue(ELEMENT_TYPE_CLASS, pValueClass, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_ARRAY:
        case ELEMENT_TYPE_SZARRAY:
        {
            // Get type name from signature and get its ICorDebugType
            std::string typeName;
            TypePrinter::NameForTypeSig(pSignatureBlob, pType, pMD, typeName);
            ToRelease<ICorDebugType> pElementType;
            IfFailRet(GetType(typeName, pThread, &pElementType));

            ToRelease<ICorDebugAppDomain> pAppDomain;
            IfFailRet(pThread->GetAppDomain(&pAppDomain));
            ToRelease<ICorDebugAppDomain2> pAppDomain2;
            IfFailRet(pAppDomain->QueryInterface(IID_ICorDebugAppDomain2, (LPVOID*) &pAppDomain2));

            // We can not directly create null value of specific array type.
            // Instead, we create one element array with element type set to our specific array type.
            // Since array elements are initialized to null, we get our null value from the first array item.

            ULONG32 dims = 1;
            ULONG32 bounds = 0;
            ToRelease<ICorDebugValue> pTmpArrayValue;
            IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, &pTmpArrayValue,
                [&](ICorDebugEval *pEval) -> HRESULT
                {
                    // Note, this code execution protected by EvalWaiter mutex.
                    ToRelease<ICorDebugEval2> pEval2;
                    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                    IfFailRet(pEval2->NewParameterizedArray(pElementType, 1, &dims, &bounds));
                    return S_OK;
                }));

            BOOL isNull = FALSE;
            ToRelease<ICorDebugValue> pUnboxedResult;
            IfFailRet(DereferenceAndUnboxValue(pTmpArrayValue, &pUnboxedResult, &isNull));

            ToRelease<ICorDebugArrayValue> pArray;
            IfFailRet(pUnboxedResult->QueryInterface(IID_ICorDebugArrayValue, (LPVOID*) &pArray));
            IfFailRet(pArray->GetElementAtPosition(0, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_GENERICINST:
        {
            // Get type name from signature and get its ICorDebugType
            std::string typeName;
            TypePrinter::NameForTypeSig(pSignatureBlob, pType, pMD, typeName);
            ToRelease<ICorDebugType> pValueType;
            IfFailRet(GetType(typeName, pThread, &pValueType));

            // Create value from ICorDebugType
            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
            ToRelease<ICorDebugEval2> pEval2;
            IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
            IfFailRet(pEval2->CreateValueForType(pValueType, ppLiteralValue));
            break;
        }
        case ELEMENT_TYPE_VALUETYPE:
        {
            // Get type token
            mdTypeDef tk;
            CorSigUncompressElementType(pSignatureBlob);
            CorSigUncompressToken(pSignatureBlob, &tk);

            ToRelease<ICorDebugClass> pValueClass;
            IfFailRet(pModule->GetClassFromToken(tk, &pValueClass));

            // Create value (without calling a constructor)
            ToRelease<ICorDebugValue> pValue;
            IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, &pValue,
                [&](ICorDebugEval *pEval) -> HRESULT
                {
                    // Note, this code execution protected by EvalWaiter mutex.
                    ToRelease<ICorDebugEval2> pEval2;
                    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                    IfFailRet(pEval2->NewParameterizedObjectNoConstructor(pValueClass, 0, nullptr));
                    return S_OK;
                }));

            // Set value
            BOOL isNull = FALSE;
            ToRelease<ICorDebugValue> pEditableValue;
            IfFailRet(DereferenceAndUnboxValue(pValue, &pEditableValue, &isNull));

            ToRelease<ICorDebugGenericValue> pGenericValue;
            IfFailRet(pEditableValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
            IfFailRet(pGenericValue->SetValue((LPVOID)pRawValue));
            *ppLiteralValue = pValue.Detach();
            break;
        }
        case ELEMENT_TYPE_STRING:
        {
            IfFailRet(m_sharedEvalWaiter->WaitEvalResult(pThread, ppLiteralValue,
                [&](ICorDebugEval *pEval) -> HRESULT
                {
                    // Note, this code execution protected by EvalWaiter mutex.
                    ToRelease<ICorDebugEval2> pEval2;
                    IfFailRet(pEval->QueryInterface(IID_ICorDebugEval2, (LPVOID*) &pEval2));
                    IfFailRet(pEval2->NewStringWithLength((LPCWSTR)pRawValue, rawValueLength));
                    return S_OK;
                }));

            break;
        }
        case ELEMENT_TYPE_BOOLEAN:
        case ELEMENT_TYPE_CHAR:
        case ELEMENT_TYPE_I1:
        case ELEMENT_TYPE_U1:
        case ELEMENT_TYPE_I2:
        case ELEMENT_TYPE_U2:
        case ELEMENT_TYPE_I4:
        case ELEMENT_TYPE_U4:
        case ELEMENT_TYPE_I8:
        case ELEMENT_TYPE_U8:
        case ELEMENT_TYPE_R4:
        case ELEMENT_TYPE_R8:
        {
            ToRelease<ICorDebugEval> pEval;
            IfFailRet(pThread->CreateEval(&pEval));
            ToRelease<ICorDebugValue> pValue;
            IfFailRet(pEval->CreateValue(underlyingType, nullptr, &pValue));
            ToRelease<ICorDebugGenericValue> pGenericValue;
            IfFailRet(pValue->QueryInterface(IID_ICorDebugGenericValue, (LPVOID*) &pGenericValue));
            IfFailRet(pGenericValue->SetValue((LPVOID)pRawValue));
            *ppLiteralValue = pValue.Detach();
            break;
        }
        default:
            return E_FAIL;
    }
    return S_OK;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pInputValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    ICorDebugType *pTypeCast,
    WalkMembersCallback cb)
{
    HRESULT Status = S_OK;

    BOOL isNull = FALSE;
    ToRelease<ICorDebugValue> pValue;

    IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

    if (isNull && !pValue.GetPtr()) return S_OK;
    else if (!pValue.GetPtr()) return E_FAIL;

    CorElementType inputCorType;
    IfFailRet(pInputValue->GetType(&inputCorType));
    if (inputCorType == ELEMENT_TYPE_PTR)
    {
        return cb(mdMethodDefNil, nullptr, nullptr, pValue, false, "");
    }

    ToRelease<ICorDebugArrayValue> pArrayValue;
    if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugArrayValue, (LPVOID *) &pArrayValue)))
    {
        ULONG32 nRank;
        IfFailRet(pArrayValue->GetRank(&nRank));

        ULONG32 cElements;
        IfFailRet(pArrayValue->GetCount(&cElements));

        std::vector<ULONG32> dims(nRank, 0);
        IfFailRet(pArrayValue->GetDimensions(nRank, &dims[0]));

        std::vector<ULONG32> base(nRank, 0);
        BOOL hasBaseIndicies = FALSE;
        if (SUCCEEDED(pArrayValue->HasBaseIndicies(&hasBaseIndicies)) && hasBaseIndicies)
            IfFailRet(pArrayValue->GetBaseIndicies(nRank, &base[0]));

        std::vector<ULONG32> ind(nRank, 0);

        for (ULONG32 i = 0; i < cElements; ++i)
        {
            ToRelease<ICorDebugValue> pElementValue;
            pArrayValue->GetElementAtPosition(i, &pElementValue);
            IfFailRet(cb(mdMethodDefNil, nullptr, nullptr, pElementValue, false, "[" + IndiciesToStr(ind, base) + "]"));
            IncIndicies(ind, dims);
        }

        return S_OK;
    }

    ToRelease<ICorDebugValue2> pValue2;
    IfFailRet(pValue->QueryInterface(IID_ICorDebugValue2, (LPVOID *) &pValue2));
    ToRelease<ICorDebugType> pType;
    if(pTypeCast == nullptr)
    {
        IfFailRet(pValue2->GetExactType(&pType));
        if (!pType) return E_FAIL;
    }
    else
    {
        pTypeCast->AddRef();
        pType = pTypeCast;
    }

    std::string className;
    TypePrinter::GetTypeOfValue(pType, className);
    if (className == "decimal") // TODO: implement mechanism for walking over custom type fields
        return S_OK;

    CorElementType corElemType;
    IfFailRet(pType->GetType(&corElemType));
    if (corElemType == ELEMENT_TYPE_STRING)
        return S_OK;

    ToRelease<ICorDebugClass> pClass;
    IfFailRet(pType->GetClass(&pClass));
    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pClass->GetModule(&pModule));
    mdTypeDef currentTypeDef;
    IfFailRet(pClass->GetToken(&currentTypeDef));
    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    std::unordered_set<std::string> backedProperties;

    ULONG numFields = 0;
    HCORENUM hEnum = NULL;
    mdFieldDef fieldDef;
    while(SUCCEEDED(pMD->EnumFields(&hEnum, currentTypeDef, &fieldDef, 1, &numFields)) && numFields != 0)
    {
        ULONG nameLen = 0;
        DWORD fieldAttr = 0;
        WCHAR mdName[mdNameLen] = {0};
        PCCOR_SIGNATURE pSignatureBlob = nullptr;
        ULONG sigBlobLength = 0;
        UVCP_CONSTANT pRawValue = nullptr;
        ULONG rawValueLength = 0;
        if (SUCCEEDED(pMD->GetFieldProps(fieldDef, nullptr, mdName, _countof(mdName), &nameLen, &fieldAttr,
                                         &pSignatureBlob, &sigBlobLength, nullptr, &pRawValue, &rawValueLength)))
        {
            std::string name = to_utf8(mdName /*, nameLen*/);

            // Prevent access to internal compiler added fields (without visible name).
            // Should be accessed by debugger routine only and hidden from user/ide.
            // Note, uncontrolled access to internal compiler added field or its properties may break debugger work.
            if (name.size() > 1 && name[0] == '<' && name[1] == '>')
                continue;

            bool is_static = (fieldAttr & fdStatic);

            ToRelease<ICorDebugValue> pFieldVal;

            if(fieldAttr & fdLiteral)
            {
                Status = GetLiteralValue(pThread, pType, pModule, pSignatureBlob, sigBlobLength, pRawValue, rawValueLength, &pFieldVal);
                if (FAILED(Status))
                {
                    pMD->CloseEnum(hEnum);
                    return Status;
                }
            }
            else if (fieldAttr & fdStatic)
            {
                if (pThread)
                {
                    ToRelease<ICorDebugFrame> pFrame;
                    if (SUCCEEDED(GetFrameAt(pThread, frameLevel, &pFrame)) && pFrame != nullptr)
                        pType->GetStaticFieldValue(fieldDef, pFrame, &pFieldVal);
                }
            }
            else
            {
                // Get pValue again, since it could be neutered at eval call in `cb` on previous cycle.
                pValue.Free();
                IfFailRet(DereferenceAndUnboxValue(pInputValue, &pValue, &isNull));

                ToRelease<ICorDebugObjectValue> pObjValue;
                if (SUCCEEDED(pValue->QueryInterface(IID_ICorDebugObjectValue, (LPVOID*) &pObjValue)))
                    pObjValue->GetFieldValue(pClass, fieldDef, &pFieldVal);
            }

            if (pFieldVal != NULL)
            {
                if (name[0] == '<')
                {
                    size_t endOffset = name.rfind('>');
                    name = name.substr(1, endOffset - 1);
                    backedProperties.insert(name);
                }
            }
            else
            {
                // no need for backing field when we can not get its value
                if (name[0] == '<')
                    continue;
            }

            if (isNull && !is_static)
                continue;
            Status = cb(mdMethodDefNil, pModule, pType, pFieldVal, is_static, name);
            if (FAILED(Status))
            {
                pMD->CloseEnum(hEnum);
                return Status;
            }
        }
    }
    pMD->CloseEnum(hEnum);

    mdProperty propertyDef;
    ULONG numProperties = 0;
    HCORENUM propEnum = NULL;
    while(SUCCEEDED(pMD->EnumProperties(&propEnum, currentTypeDef, &propertyDef, 1, &numProperties)) && numProperties != 0)
    {
        mdTypeDef  propertyClass;

        ULONG propertyNameLen = 0;
        UVCP_CONSTANT pDefaultValue;
        ULONG cchDefaultValue;
        mdMethodDef mdGetter;
        WCHAR propertyName[mdNameLen] = W("\0");
        if (SUCCEEDED(pMD->GetPropertyProps(propertyDef, &propertyClass, propertyName, _countof(propertyName),
                                            &propertyNameLen, nullptr, nullptr, nullptr, nullptr, &pDefaultValue,
                                            &cchDefaultValue, nullptr, &mdGetter, nullptr, 0, nullptr)))
        {
            DWORD getterAttr = 0;
            if (FAILED(pMD->GetMethodProps(mdGetter, NULL, NULL, 0, NULL, &getterAttr, NULL, NULL, NULL, NULL)))
                continue;

            std::string name = to_utf8(propertyName/*, propertyNameLen*/);

            if (backedProperties.find(name) != backedProperties.end())
                continue;

            bool is_static = (getterAttr & mdStatic);

            if (isNull && !is_static)
                continue;

            // https://github.sec.samsung.net/dotnet/coreclr/blob/9df87a133b0f29f4932f38b7307c87d09ab80d5d/src/System.Private.CoreLib/shared/System/Diagnostics/DebuggerBrowsableAttribute.cs#L17
            // Since we check only first byte, no reason store it as int (default enum type in c#)
            enum DebuggerBrowsableState : char
            {
                Never = 0,
                Expanded = 1, 
                Collapsed = 2,
                RootHidden = 3
            };

            const char *g_DebuggerBrowsable = "System.Diagnostics.DebuggerBrowsableAttribute..ctor";
            bool debuggerBrowsableState_Never = false;

            ULONG numAttributes = 0;
            hEnum = NULL;
            mdCustomAttribute attr;
            while(SUCCEEDED(pMD->EnumCustomAttributes(&hEnum, propertyDef, 0, &attr, 1, &numAttributes)) && numAttributes != 0)
            {
                mdToken ptkObj = mdTokenNil;
                mdToken ptkType = mdTokenNil;
                void const *ppBlob = 0;
                ULONG pcbSize = 0;
                if (FAILED(pMD->GetCustomAttributeProps(attr, &ptkObj, &ptkType, &ppBlob, &pcbSize)))
                    continue;

                std::string mdName;
                if (FAILED(TypePrinter::NameForToken(ptkType, pMD, mdName, true, nullptr)))
                    continue;

                if (mdName == g_DebuggerBrowsable
                    // In case of DebuggerBrowsableAttribute blob is 8 bytes:
                    // 2 bytes - blob prolog 0x0001
                    // 4 bytes - data (DebuggerBrowsableAttribute::State), default enum type (int)
                    // 2 bytes - alignment
                    // We check only one byte (first data byte), no reason check 4 bytes in our case.
                    && pcbSize > 2
                    && ((char const *)ppBlob)[2] == DebuggerBrowsableState::Never)
                {
                    debuggerBrowsableState_Never = true;
                    break;
                }
            }
            pMD->CloseEnum(hEnum);

            if (debuggerBrowsableState_Never)
              continue;

            IfFailRet(cb(mdGetter, pModule, pType, nullptr, is_static, name));
        }
    }
    pMD->CloseEnum(propEnum);

    std::string baseTypeName;
    ToRelease<ICorDebugType> pBaseType;
    if(SUCCEEDED(pType->GetBase(&pBaseType)) && pBaseType != NULL && SUCCEEDED(TypePrinter::GetTypeOfValue(pBaseType, baseTypeName)))
    {
        if(baseTypeName == "System.Enum")
            return S_OK;
        else if (baseTypeName != "object" && baseTypeName != "System.Object" && baseTypeName != "System.ValueType")
        {
            if (pThread)
            {
                // Note, this call could return S_FALSE without ICorDebugValue creation in case type don't have static members.
                IfFailRet(CreatTypeObjectStaticConstructor(pThread, pBaseType));
            }
            // Add fields of base class
            IfFailRet(WalkMembers(pInputValue, pThread, frameLevel, pBaseType, cb));
        }
    }

    return S_OK;
}

HRESULT Evaluator::WalkMembers(
    ICorDebugValue *pValue,
    ICorDebugThread *pThread,
    FrameLevel frameLevel,
    WalkMembersCallback cb)
{
    return WalkMembers(pValue, pThread, frameLevel, nullptr, cb);
}

static bool has_prefix(const std::string &s, const std::string &prefix)
{
    return prefix.length() <= s.length() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

HRESULT Evaluator::HandleSpecialLocalVar(
    const std::string &localName,
    ICorDebugValue *pLocalValue,
    std::unordered_set<std::string> &locals,
    WalkStackVarsCallback cb)
{
    static const std::string captureName = "CS$<>";

    HRESULT Status;

    if (!has_prefix(localName, captureName))
        return S_FALSE;

    // Substitute local value with its fields
    IfFailRet(WalkMembers(pLocalValue, nullptr, FrameLevel{0}, [&](
        mdMethodDef,
        ICorDebugModule *,
        ICorDebugType *,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &name)
    {
        if (is_static)
            return S_OK;
        if (has_prefix(name, captureName))
            return S_OK;
        if (!locals.insert(name).second)
            return S_OK; // already in the list
        return cb(pValue, name.empty() ? "this" : name);
    }));

    return S_OK;
}

HRESULT Evaluator::HandleSpecialThisParam(
    ICorDebugValue *pThisValue,
    std::unordered_set<std::string> &locals,
    WalkStackVarsCallback cb)
{
    static const std::string displayClass = "<>c__DisplayClass";

    HRESULT Status;

    std::string typeName;
    TypePrinter::GetTypeOfValue(pThisValue, typeName);

    std::size_t start = typeName.find_last_of('.');
    if (start == std::string::npos)
        return S_FALSE;

    typeName = typeName.substr(start + 1);

    if (!has_prefix(typeName, displayClass))
        return S_FALSE;

    // Substitute this with its fields
    IfFailRet(WalkMembers(pThisValue, nullptr, FrameLevel{0}, [&](
        mdMethodDef,
        ICorDebugModule *,
        ICorDebugType *,
        ICorDebugValue *pValue,
        bool is_static,
        const std::string &name)
    {
        HRESULT Status;
        if (is_static)
            return S_OK;
        IfFailRet(HandleSpecialLocalVar(name, pValue, locals, cb));
        if (Status == S_OK)
            return S_OK;
        locals.insert(name);
        return cb(pValue, name.empty() ? "this" : name);
    }));
    return S_OK;
}

HRESULT Evaluator::WalkStackVars(ICorDebugThread *pThread, FrameLevel frameLevel, WalkStackVarsCallback cb)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    ToRelease<ICorDebugFunction> pFunction;
    IfFailRet(pFrame->GetFunction(&pFunction));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunction->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    ToRelease<IMetaDataImport> pMD;
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMD));

    mdMethodDef methodDef;
    IfFailRet(pFunction->GetToken(&methodDef));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 currentIlOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&currentIlOffset, &mappingResult));

    ToRelease<ICorDebugValueEnum> pLocalsEnum;
    IfFailRet(pILFrame->EnumerateLocalVariables(&pLocalsEnum));

    ULONG cLocals = 0;
    IfFailRet(pLocalsEnum->GetCount(&cLocals));

    ULONG cParams = 0;
    ToRelease<ICorDebugValueEnum> pParamEnum;
    IfFailRet(pILFrame->EnumerateArguments(&pParamEnum));
    IfFailRet(pParamEnum->GetCount(&cParams));

    std::unordered_set<std::string> locals;

    if (cParams > 0)
    {
        DWORD methodAttr = 0;
        IfFailRet(pMD->GetMethodProps(methodDef, NULL, NULL, 0, NULL, &methodAttr, NULL, NULL, NULL, NULL));

        for (ULONG i = 0; i < cParams; i++)
        {
            ULONG paramNameLen = 0;
            mdParamDef paramDef;
            std::string paramName;

            bool thisParam = i == 0 && (methodAttr & mdStatic) == 0;
            if (thisParam)
                paramName = "this";
            else
            {
                int idx = ((methodAttr & mdStatic) == 0)? i : (i + 1);
                if(SUCCEEDED(pMD->GetParamForMethodIndex(methodDef, idx, &paramDef)))
                {
                    WCHAR wParamName[mdNameLen] = W("\0");
                    pMD->GetParamProps(paramDef, NULL, NULL, wParamName, mdNameLen, &paramNameLen, NULL, NULL, NULL, NULL);
                    paramName = to_utf8(wParamName);
                }
            }
            if(paramName.empty())
            {
                std::ostringstream ss;
                ss << "param_" << i;
                paramName = ss.str();
            }

            ToRelease<ICorDebugValue> pValue;
            ULONG cArgsFetched;
            if (FAILED(pParamEnum->Next(1, &pValue, &cArgsFetched)))
                continue;

            if (thisParam)
            {
                // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
                pFrame.Free();
                pILFrame.Free();

                IfFailRet(HandleSpecialThisParam(pValue, locals, cb));
                if (Status == S_OK)
                    continue;
            }

            locals.insert(paramName);

            // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
            pFrame.Free();
            pILFrame.Free();

            IfFailRet(cb(pValue, paramName));
        }
    }

    if (cLocals > 0)
    {
        for (ULONG i = 0; i < cLocals; i++)
        {
            std::string paramName;
            ULONG32 ilStart;
            ULONG32 ilEnd;
            if (FAILED(m_sharedModules->GetFrameNamedLocalVariable(pModule, methodDef, i, paramName, &ilStart, &ilEnd)))
                continue;

            if (currentIlOffset < ilStart || currentIlOffset >= ilEnd)
                continue;

            if (!pFrame) // Forced to update pFrame/pILFrame.
            {
                IfFailRet(GetFrameAt(pThread, frameLevel, &pFrame));
                if (pFrame == nullptr)
                    return E_FAIL;
                IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));
            }
            ToRelease<ICorDebugValue> pValue;
            if (FAILED(pILFrame->GetLocalVariable(i, &pValue)) || !pValue)
                continue;

            // Reset pFrame/pILFrame, since it could be neutered at `cb` call, we need track this case.
            pFrame.Free();
            pILFrame.Free();

            IfFailRet(HandleSpecialLocalVar(paramName, pValue, locals, cb));
            if (Status == S_OK)
                continue;

            locals.insert(paramName);
            IfFailRet(cb(pValue, paramName));
        }
    }

    return S_OK;
}

} // namespace netcoredbg
