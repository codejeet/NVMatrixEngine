#pragma once
#include "gpu_resources.h"
#include <sstream>

void logLine(const std::string &);
namespace lab::gpu {
inline void enableDred() {
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    check(D3D12GetDebugInterface(IID_PPV_ARGS(&settings)), "DRED settings");
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
}
inline void reportDred(ID3D12Device *device) {
    if (!device || SUCCEEDED(device->GetDeviceRemovedReason()))
        return;
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
    auto name = [](const wchar_t *w, const char *a) {
        if (a)
            return std::string(a);
        std::string s;
        if (w)
            while (*w) {
                s += *w < 128 ? char(*w) : '?';
                ++w;
            }
        return s;
    };
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
        for (auto n = breadcrumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            const uint32_t done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done == n->BreadcrumbCount)
                continue;
            logLine("DRED list=" + name(n->pCommandListDebugNameW, n->pCommandListDebugNameA) +
                    " queue=" + name(n->pCommandQueueDebugNameW, n->pCommandQueueDebugNameA) +
                    " completed=" + std::to_string(done) + "/" + std::to_string(n->BreadcrumbCount));
            for (uint32_t i = done > 4 ? done - 4 : 0; i < std::min(done + 8, n->BreadcrumbCount); ++i)
                logLine("DRED op " + std::to_string(i) + " = " +
                        std::to_string(n->pCommandHistory[i % 65536]));
            for (uint32_t i = 0; i < n->BreadcrumbContextsCount; ++i)
                logLine("DRED context " + std::to_string(n->pBreadcrumbContexts[i].BreadcrumbIndex) + " " +
                        name(n->pBreadcrumbContexts[i].pContextString, nullptr));
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
        std::ostringstream message;
        message << "DRED fault VA 0x" << std::hex << fault.PageFaultVA;
        logLine(message.str());
        for (auto n = fault.pHeadExistingAllocationNode; n; n = n->pNext)
            logLine("DRED existing " + name(n->ObjectNameW, n->ObjectNameA));
        for (auto n = fault.pHeadRecentFreedAllocationNode; n; n = n->pNext)
            logLine("DRED freed " + name(n->ObjectNameW, n->ObjectNameA));
    }
}
} // namespace lab::gpu
