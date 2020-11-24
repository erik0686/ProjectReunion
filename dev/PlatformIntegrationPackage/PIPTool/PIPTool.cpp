﻿// Copyright (C) Microsoft. All rights reserved.

#include <pch.h>

#include <wrl.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <stdio.h>
#include <stdlib.h>
#include <dbghelp.h>

// It appears that this resolves to msxml 3.0, but explicitly specifying that version does not compile.
#include <msxml.h>

#define FLAGS_OVERWRITE                         0x1         // Overwrite target file regardless
#define FLAGS_VERBOSE                           0x2         // Enable verbose messages
#define FLAGS_NOCHANGES                         0x4         // Show findngs, but don't actually make changes
#define FLAGS_KEEPCLSID                         0x8         // Keep default CLSID for Lifetime Manager

#define VERBOSE(printStatement) \
if (WI_IsFlagSet(flags, FLAGS_VERBOSE)) \
{ \
    printStatement; \
}

#define RETURN_IF_NOT_FOUND_OR_ERROR(hr, ptr) \
    RETURN_HR_IF_EXPECTED(S_OK, (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) || (SUCCEEDED(hr) && (ptr == nullptr))); \
    RETURN_IF_FAILED(hr);

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace Windows::Foundation;

static const PCWSTR DefaultOutputManifestFileName = L"AppxManifest.xml";
static const PCSTR DefaultLifetimeManagerClsid = "32E7CF70-038C-429a-BD49-88850F1B4A11";
static DWORD flags = 0;
static LONG propertiesNodesRemoved = 0;
static LONG propertyNodesRemoved = 0;
static LONG clsidsReplaced = 0;
static PWSTR cSourceFileName = nullptr;
static PWSTR alternateClsid = nullptr;
static wil::unique_rpc_wstr tempGuidRpcString;

HRESULT DoWork(_In_ PCWSTR sourceFolder, _In_ PCWSTR sourceFile, _In_ PCWSTR targetFile);
HRESULT ProcessManifest(_In_ PCWSTR sourceFolder, _In_ PCWSTR sourceFile, _In_ PCWSTR targetFile);
HRESULT GetDomFromManifestPath(_In_ PCWSTR sourceManifest, _Inout_ ComPtr<IXMLDOMDocument>& xmlDoc);
HRESULT IsTargetNewerThanSource(_In_ PCWSTR sidecarManifestPath, _In_ PCWSTR targetManifestPath, _Inout_ bool& isNewer);
HRESULT GetFileTimeForExistingFile(_In_ PCWSTR filePath, _Inout_ FILETIME& ft);
HRESULT ProcessDom(_Inout_ ComPtr<IXMLDOMDocument>& xmlDoc);
HRESULT ParseApplicationNode(_In_ ComPtr<IXMLDOMNode> applicationNode);
HRESULT ParseExtensionNode(_In_ ComPtr<IXMLDOMNode> extensionNode);

HRESULT ProcessNodeAndGetGrandChildrenNotes(
    _In_ ComPtr<IXMLDOMNode> currentNode,
    _In_ PCWSTR childNodeXPath,
    _In_ PCWSTR grandChildrenNodesXPath,
    _Inout_ ComPtr<IXMLDOMNodeList>& nodesList);

HRESULT GetNodesList(
    _In_ ComPtr<IXMLDOMNode> targetNode,
    _In_ PCWSTR xPathQuery,
    _Inout_ ComPtr<IXMLDOMNodeList>& nodesList);

HRESULT GetNonEmptyNodesList(
    _In_ ComPtr<IXMLDOMNode> targetNode,
    _In_ PCWSTR xPathQuery,
    _Inout_ ComPtr<IXMLDOMNodeList>& nodesList,
    _Inout_ long& nodesCount);

HRESULT ProcessComServerNode(_Inout_ ComPtr<IXMLDOMNode> comServerNode);
HRESULT ProcessComServerNodes(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _Inout_ bool& isComServe);
HRESULT ProcessExeServerNode(_Inout_ ComPtr<IXMLDOMNode> exeServerNode);
HRESULT ProcessComInterfaceNode(_Inout_ ComPtr<IXMLDOMNode> comInterfaceNode);
HRESULT ProcessComInterfaceNodes(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _Inout_ bool& isComInterface);
HRESULT ProcessComServerOrInterfaceCategory(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _Inout_ bool& isComServerOrInterface);
HRESULT FindCategoryInNode(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _In_ PCWSTR categoryValue, _Inout_ bool& valueFound);
HRESULT ProcessInterfaceNode(_Inout_ ComPtr<IXMLDOMNode> interfaceNode);
HRESULT UpdateAttributeValueIfNeeded(_Inout_ ComPtr<IXMLDOMNode> targetNode, _In_ PCWSTR attributeName);
HRESULT ProcessSourceCodeFileIfNeeded(_In_ PCWSTR sourceFolder);

int ShowUsage()
{
    wprintf(L"Usage: PIPTool <SourceFolder> <SourceManifestName> [<TargetManifestName>] [-v] [-o] [-n] [-c <CLSID>] [-fp <FilePath>]\n");
    wprintf(L"Or:    PIPTool <SourceFolder> <SourceManifestName> [<TargetManifestName>] [-v] [-o] [-n] [-k]\n");
    wprintf(L"Or:    PIPTool [-h]\n");
    wprintf(L"\t <SourceFolder>       Root folder that contains <SourceManifestName> and all other source files for the package\n");
    wprintf(L"\t                      Surround the folder path by double quotes if it contains spaces\n");
    wprintf(L"\t <SourceManifestName> Name of the input manifest for the Sidecar package\n");
    wprintf(L"\t <TargetManifestName> Name of the output manifest to be put under <SourceFolder>, default: AppxManifest.xml\n");
    wprintf(L"\t -c <CLSID>           Search and replace the prescribed CLSID instead of the internal default CLSID: %S\n",
        DefaultLifetimeManagerClsid);
    wprintf(L"\t -fp <FilePath>       Full path to a source code file that contains the default CLSID to be refreshed\n");
    wprintf(L"\t -h                   Show this page, trumps other flags\n");
    wprintf(L"\t -k                   Keep default CLSID for Lifetime Mananger (for testing purpose), trumps the -c and -fp flags\n");
    wprintf(L"\t -n                   Show findings and changes to perform, but don't actually save the changes\n");
    wprintf(L"\t -o                   Overwrite target manifest file regardless, Default: Auto, which means if \n");
    wprintf(L"\t                      <SourceManifestName> looks newer than <TargetManifestName> then the latter is silently\n");
    wprintf(L"\t                      overwritten; if <SourceManifestName> looks older than <TargetManifestName> then abort\n");
    wprintf(L"\t -v                   Show verbose diagnostic messages, Default: show brief messages\n");
    wprintf(L"Examples:\n");
    wprintf(L"\t PIPTool\n");
    wprintf(L"\t PIPTool -h\n");
    wprintf(L"\t PIPTool c:\\temp\\test\\SidecarSource SidecarAppxManifest.xml -k\n");
    wprintf(L"\t PIPTool c:\\temp\\test\\SidecarSource SidecarAppxManifest.xml -c 32E7CF70-038C-429a-BD49-88850F1B4A11\n");
    wprintf(L"\t PIPTool c:\\temp\\test\\SidecarSource SidecarAppxManifest.xml -fp c:\\temp\\test\\Header.hpp\n");
    wprintf(L"\t PIPTool \"c:\\temp\\test\\Sidecar Source\" mySidecarAppxManifest.xml AppxManifest.xml -v -o -n\n");
    wprintf(L"\n\n == The PIPTool performs these tasks: ==\n");
    wprintf(L"\t 1) Removes the Properties elements in the Extension elements in the manifest for the Platform Integration\n");
    wprintf(L"\t    Package (PIP), specified by <SourceFolder>\\<SourceManifestName>, and saves the result to the output\n");
    wprintf(L"\t    file AppxManifest.xml or the prescribed <TargetManifestName>, under <SourceFolder>.\n");
    wprintf(L"\t 2) Also searches for the default CLSID for the Lifetime Manager in the PIP's manifest and replaces it with\n");
    wprintf(L"\t    a newly generated CLSID. The default CLSID to look for can specified by the \"-c\" option.\n");
    wprintf(L"\t    Because the input and output manifest files for the PIP are both to be included in the PIP package, they.\n");
    wprintf(L"\t    are expected to be named differently, with the latter (output) likely named Appxmanifest.xml. The source\n");
    wprintf(L"\t    file is not modified by this tool.\n");
    wprintf(L"\t 3) If the \"-fp\" option is specified, then this tool searches for the same (default) CLSID in the prescribed\n");
    wprintf(L"\t    file (expectedly a .h|.hpp|.c|.cpp source file), as for the PIP's manifest file above. If the target CLSID\n");
    wprintf(L"\t    is found then it is being replaced by the same newly generated CLSID as for the PIP manifest. Note thatd\n");
    wprintf(L"\t    while CLSID-search-and-replace is always performed in case of the PIP manifest file, in case of the C source\n");
    wprintf(L"\t    file there are notable differences in behavior: \n");
    wprintf(L"\t    - The specified C source file is overwritten, because the PIP is meant to be rebuilt with the updated source\n");
    wprintf(L"\t      bearing the same file name.\n");
    wprintf(L"\t    - If the target CLSID was NOT found in the PIP manifest file, and\n");
    wprintf(L"\t        - If the target CLSID WAS found in the C source file: NO CLSID replacement, show warning, to help prevent\n");
    wprintf(L"\t          having inconsistent CLSIDs in the PIP manifest and the C source file\n");
    wprintf(L"\t        - If the target CLSID was NOT found in the C source file: NO CLSID replacement can happen, but things\n");
    wprintf(L"\t          look consistent, replacement might have consistently happened previously\n");
    wprintf(L"\t    - If the target CLSID WAS found in the PIP manifest file (and thus replacement happened there), and\n");
    wprintf(L"\t        - If the target CLSID WAS found in the C source file: CLSID replacement happens, this is the happy case\n");
    wprintf(L"\t        - If the target CLSID was NOT found in the C source file: NO CLSID replacement can happen, show warning\n");
    return 1;
}

HRESULT FileExists(_In_ PCWSTR filePath, _Out_ bool* exists)
{
    const DWORD attributes = ::GetFileAttributesW(filePath);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD lastError = GetLastError();
        RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(lastError), (lastError != ERROR_FILE_NOT_FOUND) &&
            (lastError != ERROR_PATH_NOT_FOUND) && (lastError != ERROR_INVALID_DRIVE) &&
            (lastError != ERROR_BAD_NETPATH) && (lastError != ERROR_BAD_NET_NAME) &&
            (lastError != ERROR_NO_NET_OR_BAD_PATH), "GetFileAttributesW %ws", filePath);

        *exists = false;
        return S_OK;
    }

    RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        "GetFileAttributesW %ws", filePath);

    *exists = true;
    return S_OK;
}

HRESULT ProcessSourceCodeFileIfNeeded(_In_ PCWSTR sourceFolder)
{
    if (cSourceFileName == nullptr)
    {
        VERBOSE(wprintf(L"-fp argument unspecified.\n"));
        return S_OK;
    }

    if (WI_IsFlagSet(flags, FLAGS_KEEPCLSID))
    {
        VERBOSE(wprintf(L"ProcessSourceCodeFileIfNeeded SKIPPED due to the -k option.\n"));
        return S_OK;
    }

    WCHAR pathBuffer[MAX_PATH];
    PWSTR localSourceilePath = cSourceFileName;

    bool exists = false;
    HRESULT hrExists = FileExists(localSourceilePath, &exists);
    if (FAILED(hrExists) || !exists)
    {
        wprintf(L"INFO: C source file %ws does not exist, 0x%0x.\n", localSourceilePath, hrExists);
        RETURN_IF_FAILED_MSG(StringCchPrintf(pathBuffer, ARRAYSIZE(pathBuffer), L"%ls\\%ls", sourceFolder, localSourceilePath),
            "StringCchPrintf %ws", sourceFolder);

        localSourceilePath = pathBuffer;
        hrExists = FileExists(localSourceilePath, &exists);
    }

    if (FAILED(hrExists))
    {
        wprintf(L"ERROR: C source file %ws existence check FAILED, 0x%0x.\n", localSourceilePath, hrExists);
        RETURN_HR_MSG(hrExists, "FileExists %ls", localSourceilePath);
    }

    if (!exists)
    {
        wprintf(L"ERROR: C source file %ws NOT FOUND.\n", localSourceilePath);
        RETURN_HR_MSG(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "FileExists %ls", localSourceilePath);
    }

    PSTR localClsidString = const_cast<PSTR>(DefaultLifetimeManagerClsid);
    char clsidBuffer[MAX_PATH];
    const size_t LengthOfDefaultLifetimeManagerClsid = strlen(DefaultLifetimeManagerClsid);
    if (alternateClsid != nullptr)
    {
        // Convert the Unicode alternate CLSID into ASCII.
        RETURN_IF_FAILED_MSG(StringCchPrintfA(clsidBuffer, ARRAYSIZE(clsidBuffer), "%s", alternateClsid),
            "StringCchPrintfA %s", alternateClsid);

        localClsidString = clsidBuffer;
        RETURN_HR_IF_MSG(E_INVALIDARG, strlen(localClsidString) != LengthOfDefaultLifetimeManagerClsid,
            "strlen(%s) is %u", localClsidString, strlen(localClsidString));
    }

    wil::unique_handle fileHandle(CreateFileW(
        localSourceilePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL));
    if (fileHandle.get() == nullptr)
    {
        const HRESULT hrCreate = HRESULT_FROM_WIN32(GetLastError());
        wprintf(L"ERROR: CreateFileW %ls FAILED, 0x%0x.\n", localSourceilePath, hrCreate);
        RETURN_HR_MSG(hrCreate, "CreateFileW %ws", localSourceilePath);
    }

    // We expect to be dealing with a small file, check this.
    const DWORD fileSize = GetFileSize(fileHandle.get(), nullptr);
    wprintf(L"INFO: Size of %s is %u.\n", localSourceilePath, fileSize);
    if (fileSize > 30 * MAX_PATH)
    {
        wprintf(L"ERROR: File %ls is unexpectedly large.\n", localSourceilePath);
        RETURN_HR_MSG(E_INVALIDARG, "GetFileSize %ws", localSourceilePath);
    }
    else if (fileSize <= LengthOfDefaultLifetimeManagerClsid)
    {
        wprintf(L"WARNING: File size of %ls is less than %u, ProcessSourceCodeFileIfNeeded skipped.\n", localSourceilePath,
            static_cast<ULONG>(LengthOfDefaultLifetimeManagerClsid));
        return S_OK;
    }

    char* fileBuffer = new char[fileSize];
    RETURN_IF_NULL_ALLOC_MSG(fileBuffer, "new %u", fileSize);

    auto releasefileBuffer = wil::scope_exit([fileBuffer]() {
        if (fileBuffer)
        {
            delete[] fileBuffer;
        }
    });

    DWORD readBytes = 0;
    RETURN_LAST_ERROR_IF_MSG(!ReadFile(fileHandle.get(), (PBYTE)fileBuffer, fileSize, &readBytes, nullptr),
        "ReadFile %ws %u", localSourceilePath, fileSize);

    RETURN_HR_IF_MSG(E_UNEXPECTED, readBytes != fileSize, "ReadFile %ws %u %u", localSourceilePath, readBytes, fileSize);

    VERBOSE(wprintf(L"INFO: fileBuffer:\n%S.\n", fileBuffer));

    char* resultPtr = strstr(fileBuffer, localClsidString);
    if (resultPtr == nullptr)
    {
        if (tempGuidRpcString.get() == nullptr)
        {
            VERBOSE(wprintf(L"The default CLSID %S was _not_ found in both the manifest and the source code files.\n",
                localClsidString));
            VERBOSE(wprintf(L"NO REPLACEMENT HAPPENED. That's great as long as the manifest and source code files are consistently\n"));
            VERBOSE(wprintf(L"using the same non-default CLSID.\n"));
        }
        else
        {
            wprintf(L"WARNING: The default CLSID %S was found in the manifest and replaced with %s.\n",
                localClsidString, reinterpret_cast<PCWSTR>(tempGuidRpcString.get()));
            wprintf(L"         However, the default CLSID was *not* found in source code files, that's UNEXPECTED.\n");
            wprintf(L"         Please ensure that the manifest and source code files are consistently using the same CLSID.\n");
        }
    }
    else
    {
        VERBOSE(wprintf(L"INFO: Found default CLSID at %S.\n", resultPtr));

        if (tempGuidRpcString.get() == nullptr)
        {
            wprintf(L"WARNING: The default CLSID %S was *not* found in the manifest but WAS found in the source code files.\n",
                localClsidString);
            wprintf(L"         That is UNEXPECTED, the default CLSID in the source code files was *not* replaced.\n");
            wprintf(L"         Please ensure that the manifest and source code files are consistently using the same CLSID.\n");
        }
        else if (WI_IsFlagSet(flags, FLAGS_NOCHANGES))
        {
            wprintf(L"NOTE: Target CLSID %S was FOUND in BOTH the manifest and the source file, but -n\n",
                localClsidString);
            wprintf(L"      was specified, hence NOT modifying the source file.\n");
        }
        else
        {
            char* clsidStringA = new char[LengthOfDefaultLifetimeManagerClsid + 1];
            RETURN_IF_NULL_ALLOC_MSG(clsidStringA, "new %u", LengthOfDefaultLifetimeManagerClsid + 1);

            auto releaseclsidStringA = wil::scope_exit([clsidStringA]() {
                if (clsidStringA)
                {
                    delete[] clsidStringA;
                }
            });

            const HRESULT hrPrint = StringCchPrintfA(clsidStringA, LengthOfDefaultLifetimeManagerClsid + 1, "%S",
                (STRSAFE_PCNZWCH)tempGuidRpcString.get());
            VERBOSE(wprintf(L"INFO: clsidStringA %S, 0x%0x.\n", clsidStringA, hrPrint));
            RETURN_IF_FAILED_MSG(hrPrint, "StringCchPrintfA %ws", tempGuidRpcString.get());

            UINT numOfReplacements = 0;
            do
            {
                char* clsidStringPrt = clsidStringA;
                for (UINT i = 0; i < LengthOfDefaultLifetimeManagerClsid; i++)
                {
                    if (*clsidStringPrt != '\0')
                    {
                        *resultPtr++ = *clsidStringPrt++;
                    }
                }
                numOfReplacements += 1;
                resultPtr = strstr(fileBuffer, localClsidString);
            } while (resultPtr != nullptr);

            VERBOSE(wprintf(L"INFO: fileBuffer after %u rounds of replacement:\n%S.\n", numOfReplacements, fileBuffer));

            LARGE_INTEGER seek = {};
            seek.QuadPart = 0;
            RETURN_IF_WIN32_BOOL_FALSE_MSG(SetFilePointerEx(fileHandle.get(), seek, nullptr, FILE_BEGIN),
                "SetFilePointerEx %ws", localSourceilePath);

            DWORD bytesWrite = fileSize;
            if (!::WriteFile(fileHandle.get(), fileBuffer, fileSize, &bytesWrite, nullptr))
            {
                const HRESULT hrWrite = HRESULT_FROM_WIN32(GetLastError());
                wprintf(L"ERROR: WriteFile %ls %u FAILED, 0x%0x.\n", localSourceilePath, fileSize, hrWrite);
                RETURN_HR_MSG(hrWrite, "WriteFile %ws %u", localSourceilePath, fileSize);
            }

            if (bytesWrite != fileSize)
            {
                wprintf(L"ERROR: %u bytes to write for %ls, %u bytes written.\n", fileSize, localSourceilePath, bytesWrite);
                RETURN_HR_MSG(E_FAIL, "WriteFile %ws %u %u", localSourceilePath, fileSize, bytesWrite);
            }
            wprintf(L"INFO: The default CLSID %S was found in both the manifest and the source code files.\n",
                localClsidString);
            wprintf(L"      It has been REPLACED by %s.\n", reinterpret_cast<PCWSTR>(tempGuidRpcString.get()));
        }
    }

    VERBOSE(wprintf(L"ProcessSourceCodeFileIfNeeded DONE.\n"));
    return S_OK;
}

HRESULT FindCategoryInNode(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _In_ PCWSTR categoryValue, _Inout_ bool& valueFound)
{
    valueFound = false;

    ComPtr<IXMLDOMElement> extensionElement;
    RETURN_IF_FAILED(extensionNode.As(&extensionElement));
    RETURN_IF_NULL_ALLOC(extensionElement.Get());

    wil::unique_bstr queryAttributeCategory = wil::make_bstr_nothrow(L"Category");
    RETURN_IF_NULL_ALLOC_MSG(queryAttributeCategory.get(), "make_bstr_nothrow %ws", categoryValue);

    wil::unique_variant categoryAttribute;
    const HRESULT hrGet = extensionElement->getAttribute(queryAttributeCategory.get(), &categoryAttribute);    
    RETURN_HR_IF_EXPECTED(S_OK,
        (hrGet == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) || (hrGet == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)));
    RETURN_IF_FAILED_MSG(hrGet, "getAttribute Category %ws", categoryValue);
    RETURN_HR_IF(E_INVALIDARG, categoryAttribute.vt != VT_BSTR);

    valueFound = CompareStringOrdinal(categoryAttribute.bstrVal, -1, categoryValue, -1, TRUE) == CSTR_EQUAL;
    VERBOSE(wprintf(L"INFO: FindCategoryInNode %ls %u.\n", categoryAttribute.bstrVal, static_cast<UINT>(valueFound)));
    return S_OK;
}

HRESULT GetNodesList(
    _In_ ComPtr<IXMLDOMNode> targetNode,
    _In_ PCWSTR xPathQuery,
    _Inout_ ComPtr<IXMLDOMNodeList>& nodesList)
{
    wil::unique_bstr nodesQuery = wil::make_bstr_nothrow(xPathQuery);
    RETURN_IF_NULL_ALLOC_MSG(nodesQuery.get(), "make_bstr_nothrow %ws", xPathQuery);

    RETURN_IF_FAILED_MSG(targetNode.Get()->selectNodes(nodesQuery.get(), &nodesList), "selectNodes %ws", xPathQuery);
    return S_OK;
}

HRESULT GetNonEmptyNodesList(
    _In_ ComPtr<IXMLDOMNode> targetNode,
    _In_ PCWSTR xPathQuery,
    _Inout_ ComPtr<IXMLDOMNodeList>& nodesList,
    _Inout_ long& nodesCount)
{
    const HRESULT hrSelect = GetNodesList(targetNode, xPathQuery, nodesList);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, nodesList.Get());

    VERBOSE(wprintf(L"INFO: Query %ls found results.\n", xPathQuery));
    RETURN_IF_FAILED_MSG(nodesList.Get()->get_length(&nodesCount), "get_length %ws", xPathQuery);

    VERBOSE(wprintf(L"INFO: Found %lu elements.\n", nodesCount));
    RETURN_HR_IF_EXPECTED(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), nodesCount == 0);

    return S_OK;
}

HRESULT UpdateAttributeValueIfNeeded(_Inout_ ComPtr<IXMLDOMNode> targetNode, _In_ PCWSTR attributeName)
{
    ComPtr<IXMLDOMElement> targetElement;
    RETURN_IF_FAILED(targetNode.As(&targetElement));

    wil::unique_bstr queryAttribute = wil::make_bstr_nothrow(attributeName);
    RETURN_IF_NULL_ALLOC_MSG(queryAttribute.get(), "make_bstr_nothrow %ws", attributeName);

    wil::unique_variant targetAttribute;
    const HRESULT hrGet = targetElement->getAttribute(queryAttribute.get(), &targetAttribute);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrGet, targetAttribute.bstrVal);
    RETURN_HR_IF_MSG(E_INVALIDARG, targetAttribute.vt != VT_BSTR, "getAttribute %ws", attributeName);

    VERBOSE(wprintf(L"INFO: Attribute %ls value %ls.\n", attributeName, targetAttribute.bstrVal));

    const size_t LengthOfDefaultLifetimeManagerClsid = strlen(DefaultLifetimeManagerClsid);
    WCHAR clsidBuffer[MAX_PATH];
    PWSTR localClsidString = const_cast<PWSTR>(alternateClsid);
    if (localClsidString == nullptr)
    {
        localClsidString = clsidBuffer;
        RETURN_IF_FAILED_MSG(StringCchPrintfW(clsidBuffer, ARRAYSIZE(clsidBuffer), L"%ls", DefaultLifetimeManagerClsid),
            "StringCchPrintf %S", DefaultLifetimeManagerClsid);
    }
    else
    {
        RETURN_HR_IF_MSG(E_INVALIDARG, wcslen(localClsidString) != LengthOfDefaultLifetimeManagerClsid,
            "wcslen(%ws) is %u", localClsidString, wcslen(localClsidString));
    }

    RETURN_HR_IF_EXPECTED(S_OK,
        CompareStringOrdinal(targetAttribute.bstrVal, -1, localClsidString, -1, TRUE) != CSTR_EQUAL);

    if (tempGuidRpcString.get() == nullptr)
    {
        UUID uniqueId;
        RETURN_IF_FAILED_MSG(CoCreateGuid(&uniqueId), "CoCreateGuid %ws", attributeName);
        RETURN_HR_IF_MSG(HRESULT_FROM_WIN32(E_UNEXPECTED), UuidToString(&uniqueId, &tempGuidRpcString) != RPC_S_OK,
            "UuidToString %ws", attributeName);

        VERBOSE(wprintf(L"INFO: CoCreateGuid %ls.\n", reinterpret_cast<PCWSTR>(tempGuidRpcString.get())));
    }

    wil::unique_variant variantValue;
    variantValue.vt = VT_BSTR;
    variantValue.bstrVal = wil::make_bstr_nothrow(reinterpret_cast<PCWSTR>(tempGuidRpcString.get())).release();
    RETURN_IF_NULL_ALLOC_MSG(variantValue.bstrVal, "make_bstr_nothrow %ws", tempGuidRpcString.get());

    const HRESULT hrSet = targetElement->setAttribute(queryAttribute.get(), variantValue);
    VERBOSE(wprintf(L"INFO: setAttribute %ls to %ls, 0x%0x.\n", attributeName,
        reinterpret_cast<PCWSTR>(tempGuidRpcString.get()), hrSet));
    RETURN_IF_FAILED_MSG(hrSet, "setAttribute %ws %ws", attributeName, tempGuidRpcString.get());

    clsidsReplaced += 1;
    return S_OK;
}

HRESULT ProcessInterfaceNode(_Inout_ ComPtr<IXMLDOMNode> interfaceNode)
{
    static const WCHAR typeLibNodesXPath[] = L"./*[local-name()='TypeLib']";
    ComPtr<IXMLDOMNodeList> typeLibNodesList;
    long nodesCount = 0;
    const HRESULT hrSelect = GetNonEmptyNodesList(interfaceNode, typeLibNodesXPath, typeLibNodesList, nodesCount);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, typeLibNodesList.Get());

    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> typeLibNode;
        RETURN_IF_FAILED_MSG(typeLibNodesList.Get()->get_item(index, &typeLibNode), "get_item TypeLib %u %u", index, nodesCount);
        RETURN_IF_FAILED_MSG(UpdateAttributeValueIfNeeded(typeLibNode, L"Id"),
            "UpdateAttributeValueIfNeeded TypeLib %u %u", index, nodesCount);
    }
    VERBOSE(wprintf(L"ProcessInterfaceNode DONE.\n"));
    return S_OK;
}

HRESULT ProcessExeServerNode(_Inout_ ComPtr<IXMLDOMNode> exeServerNode)
{
    static const WCHAR classNodesXPath[] = L"./*[local-name()='Class']";
    ComPtr<IXMLDOMNodeList> classNodesList;
    long nodesCount = 0;
    const HRESULT hrSelect = GetNonEmptyNodesList(exeServerNode, classNodesXPath, classNodesList, nodesCount);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, classNodesList.Get());

    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> classNode;
        RETURN_IF_FAILED_MSG(classNodesList.Get()->get_item(index, &classNode), "get_item Class %u %u", index, nodesCount);
        RETURN_IF_FAILED_MSG(UpdateAttributeValueIfNeeded(classNode, L"Id"),
            "UpdateAttributeValueIfNeeded Class %u %u", index, nodesCount);
    }
    VERBOSE(wprintf(L"ProcessExeServerNode DONE.\n"));
    return S_OK;
}

HRESULT ProcessComServerNode(_Inout_ ComPtr<IXMLDOMNode> comServerNode)
{
    static const WCHAR exeServerNodesXPath[] = L"./*[local-name()='ExeServer']";
    ComPtr<IXMLDOMNodeList> exeServerNodesList;
    long nodesCount = 0;
    HRESULT hrSelect = GetNonEmptyNodesList(comServerNode, exeServerNodesXPath, exeServerNodesList, nodesCount);
    if (SUCCEEDED(hrSelect) && (exeServerNodesList.Get() != nullptr))
    {
        for (int index = nodesCount - 1; index >= 0; index--)
        {
            ComPtr<IXMLDOMNode> exeServerNode;
            RETURN_IF_FAILED_MSG(exeServerNodesList.Get()->get_item(index, &exeServerNode),
                "get_item ExeServer %u %u", index, nodesCount);

            RETURN_IF_FAILED_MSG(ProcessExeServerNode(exeServerNode),
                "ProcessExeServerNode Class %u %u", index, nodesCount);
        }
    }

    RETURN_HR_IF_MSG(hrSelect, FAILED(hrSelect) && (hrSelect != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)),
        "GetNonEmptyNodesList ExeServer");

    static const WCHAR progIdNodesXPath[] = L"./*[local-name()='ProgId']";
    ComPtr<IXMLDOMNodeList> progIdNodesList;
    hrSelect = GetNonEmptyNodesList(comServerNode, progIdNodesXPath, progIdNodesList, nodesCount);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, progIdNodesList.Get());

    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> progIdNode;
        RETURN_IF_FAILED_MSG(progIdNodesList.Get()->get_item(index, &progIdNode),
            "get_item ProgId %u %u", index, nodesCount);
        RETURN_IF_FAILED_MSG(UpdateAttributeValueIfNeeded(progIdNode, L"Clsid"),
            "UpdateAttributeValueIfNeeded ProgId %u %u", index, nodesCount);
    }
    VERBOSE(wprintf(L"ProcessComServerNode DONE.\n"));
    return S_OK;
}

HRESULT ProcessComServerNodes(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _Inout_ bool& isComServe)
{
    isComServe = false;
    static const PCWSTR comServerAttributeValue = L"windows.comServer";
    RETURN_IF_FAILED(FindCategoryInNode(extensionNode, comServerAttributeValue, isComServe));
    RETURN_HR_IF_EXPECTED(S_OK, !isComServe);

    if (WI_IsFlagSet(flags, FLAGS_KEEPCLSID))
    {
        VERBOSE(wprintf(L"ProcessComServerNodes SKIPPED  due to the -k option.\n"));
    }
    else
    {
        static const WCHAR comServerNodesXPath[] = L"./*[local-name()='ComServer']";
        ComPtr<IXMLDOMNodeList> comServerNodesList;
        long nodesCount = 0;
        const HRESULT hrSelect = GetNonEmptyNodesList(extensionNode, comServerNodesXPath, comServerNodesList, nodesCount);
        RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, comServerNodesList.Get());

        for (int index = nodesCount - 1; index >= 0; index--)
        {
            ComPtr<IXMLDOMNode> comServerNode;
            RETURN_IF_FAILED_MSG(comServerNodesList.Get()->get_item(index, &comServerNode),
                "get_item ComServer %u %u", index, nodesCount);

            RETURN_IF_FAILED_MSG(ProcessComServerNode(comServerNode),
                "ProcessComServerNode %u %u", index, nodesCount);
        }

        VERBOSE(wprintf(L"ProcessComServerNodes DONE.\n"));
    }
    return S_OK;
}

HRESULT ProcessComInterfaceNode(_Inout_ ComPtr<IXMLDOMNode> comInterfaceNode)
{
    static const WCHAR interfaceNodesXPath[] = L"./*[local-name()='Interface']";
    ComPtr<IXMLDOMNodeList> interfaceNodesList;
    long nodesCount = 0;
    HRESULT hrSelect = GetNonEmptyNodesList(comInterfaceNode, interfaceNodesXPath, interfaceNodesList, nodesCount);
    if (SUCCEEDED(hrSelect) && (interfaceNodesList.Get() != nullptr))
    {
        for (int index = nodesCount - 1; index >= 0; index--)
        {
            ComPtr<IXMLDOMNode> interfaceNode;
            RETURN_IF_FAILED_MSG(interfaceNodesList.Get()->get_item(index, &interfaceNode),
                "get_item Interface %u %u", index, nodesCount);

            RETURN_IF_FAILED_MSG(ProcessInterfaceNode(interfaceNode),
                "ProcessInterfaceNode Interface %u %u", index, nodesCount);
        }
    }

    RETURN_HR_IF_MSG(hrSelect, FAILED(hrSelect) && (hrSelect != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)),
        "GetNonEmptyNodesList Interface");

    static const WCHAR typeLibNodesXPath[] = L"./*[local-name()='TypeLib']";
    ComPtr<IXMLDOMNodeList> typeLibNodesList;
    hrSelect = GetNonEmptyNodesList(comInterfaceNode, typeLibNodesXPath, typeLibNodesList, nodesCount);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, typeLibNodesList.Get());

    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> typeLibNode;
        RETURN_IF_FAILED_MSG(typeLibNodesList.Get()->get_item(index, &typeLibNode),
            "get_item TypeLib %u %u", index, nodesCount);

        RETURN_IF_FAILED_MSG(UpdateAttributeValueIfNeeded(typeLibNode, L"Id"),
            "UpdateAttributeValueIfNeeded TypeLib %u %u", index, nodesCount);
    }
    VERBOSE(wprintf(L"ProcessComInterfaceNode DONE.\n"));
    return S_OK;
}

HRESULT ProcessComInterfaceNodes(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _Inout_ bool& isComInterface)
{
    isComInterface = false;
    static const PCWSTR comInterfaceAttributeValue = L"windows.comInterface";
    RETURN_IF_FAILED_MSG(FindCategoryInNode(extensionNode, comInterfaceAttributeValue, isComInterface),
        "FindCategoryInNode %ws", comInterfaceAttributeValue);

    RETURN_HR_IF_EXPECTED(S_OK, !isComInterface);

    if (WI_IsFlagSet(flags, FLAGS_KEEPCLSID))
    {
        VERBOSE(wprintf(L"ProcessComInterfaceNodes SKIPPED  due to the -k option.\n"));
    }
    else
    {
        static const WCHAR comInterfaceNodesXPath[] = L"./*[local-name()='ComInterface']";
        ComPtr<IXMLDOMNodeList> comInterfaceNodesList;
        long nodesCount = 0;
        const HRESULT hrSelect = GetNonEmptyNodesList(extensionNode, comInterfaceNodesXPath, comInterfaceNodesList, nodesCount);
        RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, comInterfaceNodesList.Get());

        for (int index = nodesCount - 1; index >= 0; index--)
        {
            ComPtr<IXMLDOMNode> comInterfaceNode;
            RETURN_IF_FAILED_MSG(comInterfaceNodesList.Get()->get_item(index, &comInterfaceNode),
                "get_item ComInterface %u %u", index, nodesCount);

            RETURN_IF_FAILED_MSG(ProcessComInterfaceNode(comInterfaceNode),
                "ProcessComInterfaceNode %u %u", index, nodesCount);
        }

        VERBOSE(wprintf(L"ProcessComInterfaceNodes DONE.\n"));
    }
    return S_OK;
}

// This method does not enforce the schema, just perform search-and-replace.
// Returns S_OK if extension ndoe is successfully processed; appropriate error otherwise.
HRESULT ProcessComServerOrInterfaceCategory(_Inout_ ComPtr<IXMLDOMNode> extensionNode, _Inout_ bool& isComServerOrInterface)
{
    bool isElementFound = false;
    RETURN_IF_FAILED(ProcessComServerNodes(extensionNode, isElementFound));

    // If this was a COM Server element, then it wouldn't be a COM Interface element, we can return normally.
    isComServerOrInterface = isElementFound;
    RETURN_HR_IF_EXPECTED(S_OK, isComServerOrInterface);

    RETURN_IF_FAILED(ProcessComInterfaceNodes(extensionNode, isElementFound));

    isComServerOrInterface = isElementFound;
    VERBOSE(wprintf(L"ProcessComServerOrInterfaceCategory DONE %u.\n", static_cast<UINT>(isComServerOrInterface)));
    return S_OK;
}

HRESULT ParseExtensionNode(_In_ ComPtr<IXMLDOMNode> extensionNode)
{
    static const WCHAR propertiesElementXPath[] = L"./*[local-name()='Properties']";
    static const WCHAR propertyElementsXPath[] = L"./*";

    // If this is a COM Server or Interface element, process it and then we are done. Currently there is no
    // Properties element in such an element.
    bool isComServerOrInterface = false;
    RETURN_IF_FAILED(ProcessComServerOrInterfaceCategory(extensionNode, isComServerOrInterface));
    RETURN_HR_IF_EXPECTED(S_OK, isComServerOrInterface);

    wil::unique_bstr propertiesNodeQuery = wil::make_bstr_nothrow(propertiesElementXPath);
    RETURN_IF_NULL_ALLOC_MSG(propertiesNodeQuery.get(), "make_bstr_nothrow %ws", propertiesElementXPath);

    ComPtr<IXMLDOMNode> propertiesNode;
    HRESULT hrSelect = extensionNode.Get()->selectSingleNode(propertiesNodeQuery.get(), &propertiesNode);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, propertiesNode.Get());

    VERBOSE(wprintf(L"INFO: Query %ls found results.\n", propertiesElementXPath));    

    // Enumerate the property nodes under the properties node.
    wil::unique_bstr propertyNodesQuery = wil::make_bstr_nothrow(propertyElementsXPath);
    RETURN_IF_NULL_ALLOC_MSG(propertyNodesQuery.get(), "make_bstr_nothrow %ws", propertyElementsXPath);

    ComPtr<IXMLDOMNodeList> propertyNodesList;
    hrSelect = propertiesNode.Get()->selectNodes(propertyNodesQuery.get(), &propertyNodesList);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, propertyNodesList.Get());

    VERBOSE(wprintf(L"INFO: Query %ls found results.\n", propertyElementsXPath));    

    long nodesCount = 0;
    RETURN_IF_FAILED_MSG(propertyNodesList.Get()->get_length(&nodesCount), "get_length %ws", propertyElementsXPath);

    VERBOSE(wprintf(L"INFO: Found %lu property elements in manifest.\n", nodesCount));    

    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> propertyNode;
        RETURN_IF_FAILED_MSG(propertyNodesList.Get()->get_item(index, &propertyNode), "get_item %u %u", index, nodesCount);

        ComPtr<IXMLDOMNode> outNode;
        RETURN_IF_FAILED_MSG(propertiesNode.Get()->removeChild(propertyNode.Get(), &outNode),
            "removeChild %u %u", index, nodesCount);

        wil::unique_bstr nodeName;
        RETURN_IF_FAILED_MSG(outNode->get_nodeName(&nodeName), "get_nodeName %u %u", index, nodesCount);

        wil::unique_bstr nodeText;
        RETURN_IF_FAILED_MSG(outNode->get_text(&nodeText), "get_text %u %u", index, nodesCount);

        VERBOSE(wprintf(L"INFO: Removed property %ls with data %ls.\n", nodeName.get(), nodeText.get()));
        propertyNodesRemoved += 1;
    }

    ComPtr<IXMLDOMNode> outNode;
    RETURN_IF_FAILED_MSG(extensionNode.Get()->removeChild(propertiesNode.Get(), &outNode), "removeChild Properties");
    
    VERBOSE(wprintf(L"INFO: Removed properties node.\n"));
    propertiesNodesRemoved += 1;
    return S_OK;
}

HRESULT ProcessNodeAndGetGrandChildrenNotes(
    _In_ ComPtr<IXMLDOMNode> currentNode,
    _In_ PCWSTR childNodeXPath,
    _In_ PCWSTR grandChildrenNodesXPath,
    _Inout_ ComPtr<IXMLDOMNodeList>& nodesList)
{        
    // Check if the prescribe child node is present under the current node.
    wil::unique_bstr childNodeQuery = wil::make_bstr_nothrow(childNodeXPath);
    RETURN_IF_NULL_ALLOC_MSG(childNodeQuery.get(), "make_bstr_nothrow %ws", childNodeXPath);

    ComPtr<IXMLDOMNode> childNode;
    HRESULT hrSelect = currentNode.Get()->selectSingleNode(childNodeQuery.get(), &childNode);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, childNode.Get());

    VERBOSE(wprintf(L"INFO: Query %ls found results.\n", childNodeXPath));    

    // Enumerate the grand children nodes under the child node.
    wil::unique_bstr grandChildrenNodesQuery = wil::make_bstr_nothrow(grandChildrenNodesXPath);
    RETURN_IF_NULL_ALLOC_MSG(grandChildrenNodesQuery.get(), "make_bstr_nothrow %ws", grandChildrenNodesXPath);

    hrSelect = childNode.Get()->selectNodes(grandChildrenNodesQuery.get(), &nodesList);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, nodesList.Get());

    VERBOSE(wprintf(L"INFO: Query %ls found results.\n", grandChildrenNodesXPath));    
    return S_OK;
}

HRESULT ParseApplicationNode(_In_ ComPtr<IXMLDOMNode> applicationNode)
{
    static const WCHAR extensionsElementXPath[] = L"./*[local-name()='Extensions']";
    static const WCHAR extensionElementsXPath[] = L"./*[local-name()='Extension']";

    // Enumerate the Extension elements under the Extensions element, which is in turn under the application node.
    ComPtr<IXMLDOMNodeList> extensionNodesList;        
    const HRESULT hrSelect = ProcessNodeAndGetGrandChildrenNotes(applicationNode,
        extensionsElementXPath, 
        extensionElementsXPath, 
        extensionNodesList);
    RETURN_IF_NOT_FOUND_OR_ERROR(hrSelect, extensionNodesList.Get());

    long nodesCount = 0;
    RETURN_IF_FAILED_MSG(extensionNodesList.Get()->get_length(&nodesCount), "get_length %ws", extensionsElementXPath);

    VERBOSE(wprintf(L"Found %lu extension elements in manifest.\n", nodesCount));        

    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> extensionNode;
        RETURN_IF_FAILED_MSG(extensionNodesList.Get()->get_item(index, &extensionNode), "get_item %u %u", index, nodesCount);
        RETURN_IF_FAILED_MSG(ParseExtensionNode(extensionNode), "ParseExtensionNode %u %u", index, nodesCount);
    }
    return S_OK;
}

HRESULT ProcessDom(_Inout_ ComPtr<IXMLDOMDocument>& xmlDoc)
{
    static const WCHAR applicationElementXPath[] = 
        L"/*[local-name()='Package']/*[local-name()='Applications']/*[local-name()='Application']";

    // Expect to find at least one Aplication element.
    wil::unique_bstr applicationElementsQuery = wil::make_bstr_nothrow(applicationElementXPath);
    RETURN_IF_NULL_ALLOC_MSG(applicationElementsQuery.get(), "make_bstr_nothrow %ws", applicationElementXPath);

    // TODO: selectNodes currently returns E_FAIL.
    ComPtr<IXMLDOMNodeList> applicationNodesList;  
    RETURN_IF_FAILED_MSG(xmlDoc.Get()->selectNodes(applicationElementsQuery.get(), &applicationNodesList),
        "selectNodes %ws", applicationElementXPath);
    RETURN_IF_NULL_ALLOC(applicationNodesList.Get());

    long nodesCount = 0;
    RETURN_IF_FAILED_MSG(applicationNodesList.Get()->get_length(&nodesCount), "get_length %ws", applicationElementXPath);

    VERBOSE(wprintf(L"Found %lu apps in manifest.\n", nodesCount));
    RETURN_HR_IF_MSG(E_INVALIDARG, nodesCount < 1, "get_length %ws %d", applicationElementXPath, nodesCount);

    // Enumerate application elements.
    for (int index = nodesCount - 1; index >= 0; index--)
    {
        ComPtr<IXMLDOMNode> applicationNode;
        RETURN_IF_FAILED_MSG(applicationNodesList.Get()->get_item(index, &applicationNode),
            "get_item Application %d %d", index, nodesCount);

        RETURN_IF_FAILED_MSG(ParseApplicationNode(applicationNode), "ParseApplicationNode %d %d", index, nodesCount);
    }

    return S_OK;
}

HRESULT GetDomFromManifestPath(_In_ PCWSTR sourceManifest, _Inout_ ComPtr<IXMLDOMDocument> & xmlDoc)
{
    RETURN_IF_FAILED(CoCreateInstance(CLSID_DOMDocument, nullptr, CLSCTX_ALL, __uuidof(IXMLDOMDocument), (LPVOID*)&xmlDoc));
    RETURN_IF_NULL_ALLOC(xmlDoc.Get());

    RETURN_IF_FAILED_MSG(xmlDoc.Get()->put_async(VARIANT_FALSE), "put_async F");
    RETURN_IF_FAILED_MSG(xmlDoc.Get()->put_validateOnParse(VARIANT_FALSE), "put_validateOnParse F");
    RETURN_IF_FAILED_MSG(xmlDoc.Get()->put_resolveExternals(VARIANT_FALSE), "put_resolveExternals F");
    RETURN_IF_FAILED_MSG(xmlDoc.Get()->put_preserveWhiteSpace(VARIANT_FALSE), "put_preserveWhiteSpace F");

    wil::unique_variant filePath;
    filePath.vt = VT_BSTR;
    filePath.bstrVal = wil::make_bstr(sourceManifest).release();

    VARIANT_BOOL isSuccessful = VARIANT_FALSE;
    RETURN_IF_FAILED_MSG(xmlDoc.Get()->load(filePath, &isSuccessful), "load %ws", filePath.bstrVal);

    if (isSuccessful == VARIANT_TRUE)
    {
        // Return from here if XML loading was successful.
        wprintf(L"INFO: Loaded manifest %ls.\n", filePath.bstrVal);
        return S_OK;
    }

    ComPtr<IXMLDOMParseError> xmlParseError;
    xmlDoc.Get()->get_parseError(&xmlParseError);

    long xmlLine = 0;
    xmlParseError.Get()->get_line(&xmlLine);

    long xmlLinePos = 0;
    xmlParseError.Get()->get_linepos(&xmlLinePos);

    wil::unique_bstr errorReason;
    xmlParseError.Get()->get_reason(&errorReason);

    wprintf(L"ERROR: %ls, Line: %d, Col: %d, Reason: %ls.\n", sourceManifest, xmlLine, xmlLinePos, errorReason.get());
    RETURN_HR_MSG(E_FAIL, "load %ws, Line: %d, Col: %d, Reason: %ws", filePath.bstrVal, xmlLine, xmlLinePos, errorReason.get());

    return S_OK;
}

HRESULT GetFileTimeForExistingFile(_In_ PCWSTR filePath, _Inout_ FILETIME& ft)
{
    wil::unique_handle fileHandle(CreateFile(filePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL));
    if (fileHandle.get() == nullptr)
    {
        const HRESULT hrCreate = HRESULT_FROM_WIN32(GetLastError());
        wprintf(L"ERROR: CreateFile %ws FAILED, 0x%0x.\n", filePath, hrCreate);
        RETURN_HR_MSG(hrCreate, "CreateFileW %ws", filePath);
    }

	RETURN_IF_WIN32_BOOL_FALSE_MSG(GetFileTime(fileHandle.get(), NULL, NULL, &ft), "GetFileTime %ws", filePath);
    return S_OK;
}

HRESULT IsTargetNewerThanSource(_In_ PCWSTR sidecarManifestPath, _In_ PCWSTR targetManifestPath, _Inout_ bool& isNewer)
{
    isNewer = false;

    FILETIME sourceWriteTime = { 0 };
	RETURN_IF_FAILED_MSG(GetFileTimeForExistingFile(sidecarManifestPath, sourceWriteTime),
        "GetFileTimeForExistingFile %ws", sidecarManifestPath);

    FILETIME targetWriteTime = { 0 };
    RETURN_IF_FAILED_MSG(GetFileTimeForExistingFile(targetManifestPath, targetWriteTime),
        "GetFileTimeForExistingFile %ws", targetManifestPath);

    isNewer = (CompareFileTime(&sourceWriteTime, &targetWriteTime) < (LONG)0);
    return S_OK;
}

HRESULT DoWork(_In_ PCWSTR sourceFolder, _In_ PCWSTR sourceFile, _In_ PCWSTR targetFile)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        wprintf(L"ERROR: CoInitializeEx FAILED, 0x%0x.\n", hr);
        RETURN_HR_MSG(hr, "CoInitializeEx");
    }

    hr = ProcessManifest(sourceFolder, sourceFile, targetFile);
    if (SUCCEEDED(hr))
    {
        // During processing of the manifest, we might have replaced CLSIDs in the manifest, if so, we need to
        // accordingly replace CLSIDs in the source code file if so specificed.
        hr = ProcessSourceCodeFileIfNeeded(sourceFolder);
    }

    CoUninitialize();
    return hr;
}

HRESULT ProcessManifest(_In_ PCWSTR sourceFolder, _In_ PCWSTR sourceFile, _In_ PCWSTR targetFile)
{
    WCHAR sidecarManifestPath[MAX_PATH];
    RETURN_IF_FAILED_MSG(StringCchPrintf(sidecarManifestPath, ARRAYSIZE(sidecarManifestPath), L"%ls\\%ls", sourceFolder, sourceFile),
        "StringCchPrintf %ws %ws", sourceFolder, sourceFile);

    WCHAR targetManifestPath[MAX_PATH];
    RETURN_IF_FAILED_MSG(StringCchPrintf(targetManifestPath, ARRAYSIZE(targetManifestPath), L"%ls\\%ls", sourceFolder, targetFile),
        "StringCchPrintf %ws %ws", sourceFolder, targetFile);

    bool exists = false;
    HRESULT hr = FileExists(sidecarManifestPath, &exists);
    if (FAILED_LOG(hr) || !exists)
    {
        wprintf(L"ERROR: 0x%0x: Existence of %ls is %u.\n", hr, sidecarManifestPath, static_cast<UINT>(exists));
        RETURN_HR_MSG(hr, "FileExists %ws %u", sidecarManifestPath, static_cast<UINT>(exists));
    }

    hr = FileExists(targetManifestPath, &exists);
    if (SUCCEEDED_LOG(hr) && exists && !WI_IsFlagSet(flags, FLAGS_OVERWRITE))
    {
        bool isNewer = false;
        RETURN_IF_FAILED_MSG(IsTargetNewerThanSource(sidecarManifestPath, targetManifestPath, isNewer),
            "IsTargetNewerThanSource %ws", sidecarManifestPath);

        if (isNewer)
        {
            wprintf(L"WARNING: 0x%0x: %ls exists and is newer than %ls.\n", hr, targetManifestPath, sidecarManifestPath);
            RETURN_HR_MSG(hr, "IsTargetNewerThanSource");
        }
    }

    VERBOSE(wprintf(L"INFO: Input: %ls, Output: %ls (presence %u).\n", sidecarManifestPath, targetManifestPath,
        static_cast<UINT>(exists)));

    ComPtr<IXMLDOMDocument> xmlDoc;
    RETURN_IF_FAILED_MSG(GetDomFromManifestPath(sidecarManifestPath, xmlDoc), "sidecarManifestPath %ws", sidecarManifestPath);
    RETURN_IF_FAILED_MSG(ProcessDom(xmlDoc), "ProcessDom %ws", sidecarManifestPath);

    wprintf(L"INFO: %lu properties and %lu property nodes removed.\n", propertiesNodesRemoved, propertyNodesRemoved);
    wprintf(L"INFO: %lu CLSIDs replaced.\n", clsidsReplaced);

    // Even if nothing was changed by us explicitly, formatting of the doc might have changed, save it anyway.
    wil::unique_variant pathVariant;
    pathVariant.vt = VT_BSTR;
    pathVariant.bstrVal = wil::make_bstr(targetManifestPath).release();

    if (WI_IsFlagSet(flags, FLAGS_NOCHANGES))
    {
        VERBOSE(wprintf(L"NOTE: ** OUTPUT MANIFEST _NOT_ SAVED BECAUSE -n WAS SPECIFIED. **\n"));
    }
    else
    {
        const HRESULT hrSave = xmlDoc->save(pathVariant);
        if (FAILED_LOG(hrSave))
        {
            wprintf(L"ERROR: 0x%0x: saving %ls.\n", hrSave, targetManifestPath);
            RETURN_HR_MSG(hrSave, "save %ws", pathVariant.bstrVal);
        }

        VERBOSE(wprintf(L"INFO: Output: %ls saved.\n", targetManifestPath));
    }

    wprintf(L"ProcessManifest Done.\n");
    return S_OK;
}

// Argument processing, and then hand over to the main loop.
int __cdecl wmain(int argc, _In_reads_(argc) WCHAR * argv[])
{
    wprintf(L"PIPTool Copyright (C) Microsoft. All rights reserved.\n");
    if (argc < 3)
    {
        return ShowUsage();
    }

    PCWSTR sourceFolder = argv[1];
    PCWSTR sourceFile = argv[2];    
    PWSTR targetFile = nullptr;    
    for (int i = 3; i < argc; i++)
    {
        if (CompareStringOrdinal(argv[i], -1, L"-o", -1, TRUE) == CSTR_EQUAL)
        {
            flags |= FLAGS_OVERWRITE;
        }
        else if (CompareStringOrdinal(argv[i], -1, L"-v", -1, TRUE) == CSTR_EQUAL)
        {
            flags |= FLAGS_VERBOSE;
        }
        else if (CompareStringOrdinal(argv[i], -1, L"-n", -1, TRUE) == CSTR_EQUAL)
        {
            flags |= FLAGS_NOCHANGES;
        }
        else if (CompareStringOrdinal(argv[i], -1, L"-k", -1, TRUE) == CSTR_EQUAL)
        {
            flags |= FLAGS_KEEPCLSID;
        }
        else if (CompareStringOrdinal(argv[i], -1, L"-h", -1, TRUE) == CSTR_EQUAL)
        {
            return ShowUsage();
        }
        else if (CompareStringOrdinal(argv[i], -1, L"-fp", -1, TRUE) == CSTR_EQUAL)
        {
            if (i + 1 < argc)
            {
                i += 1;
                cSourceFileName = argv[i];
            }
            else
            {
                wprintf(L"ERROR: missing file name?\n");
                return ShowUsage();
            }
        }
        else if (CompareStringOrdinal(argv[i], -1, L"-c", -1, TRUE) == CSTR_EQUAL)
        {
            if (i + 1 < argc)
            {
                i += 1;
                alternateClsid = argv[i];
            }
            else
            {
                wprintf(L"ERROR: missing CLSID?\n");
                return ShowUsage();
            }
        }
        else if (targetFile == nullptr)
        {
            targetFile = argv[i];
        }
        else
        {
            return ShowUsage();
        }
    }

    targetFile = (targetFile == nullptr) ? const_cast<PWSTR>(DefaultOutputManifestFileName) : targetFile;
    const int doWorkResult = SUCCEEDED(DoWork(sourceFolder, sourceFile, targetFile)) ? 0 : 1;
    wprintf(L"PIPTool exiting.\n");
    return doWorkResult;
}