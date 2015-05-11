//
//   Copyright 2013 Pixar
//
//   Licensed under the Apache License, Version 2.0 (the "Apache License")
//   with the following modification; you may not use this file except in
//   compliance with the Apache License and the following modification to it:
//   Section 6. Trademarks. is deleted and replaced with:
//
//   6. Trademarks. This License does not grant permission to use the trade
//      names, trademarks, service marks, or product names of the Licensor
//      and its affiliates, except as required to comply with Section 4(c) of
//      the License and to reproduce the content of the NOTICE file.
//
//   You may obtain a copy of the Apache License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the Apache License with the above modification is
//   distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
//   KIND, either express or implied. See the Apache License for the specific
//   language governing permissions and limitations under the Apache License.
//

#include "../osd/d3d11DrawContext.h"

#include <D3D11.h>

#include <string.h>

namespace OpenSubdiv {
namespace OPENSUBDIV_VERSION {

namespace Osd {

D3D11DrawContext::D3D11DrawContext() :
    patchIndexBuffer(NULL),
    patchParamBuffer(NULL),
    patchParamBufferSRV(NULL),
    fvarDataBuffer(NULL),
    fvarDataBufferSRV(NULL),
    vertexBufferSRV(NULL),
    vertexValenceBuffer(NULL),
    vertexValenceBufferSRV(NULL),
    quadOffsetBuffer(NULL),
    quadOffsetBufferSRV(NULL)
{
}

D3D11DrawContext::~D3D11DrawContext()
{
    if (patchIndexBuffer) patchIndexBuffer->Release();
    if (patchParamBuffer) patchParamBuffer->Release();
    if (patchParamBufferSRV) patchParamBufferSRV->Release();
    if (fvarDataBuffer) fvarDataBuffer->Release();
    if (fvarDataBufferSRV) fvarDataBufferSRV->Release();
    if (vertexBufferSRV) vertexBufferSRV->Release();
    if (vertexValenceBuffer) vertexValenceBuffer->Release();
    if (vertexValenceBufferSRV) vertexValenceBufferSRV->Release();
    if (quadOffsetBuffer) quadOffsetBuffer->Release();
    if (quadOffsetBufferSRV) quadOffsetBufferSRV->Release();
};

D3D11DrawContext *
D3D11DrawContext::Create(Far::PatchTables const *patchTables,
                         int numVertexElements,
                         ID3D11DeviceContext *pd3d11DeviceContext)
{
    D3D11DrawContext * result = new D3D11DrawContext();
    if (result->create(*patchTables, numVertexElements, pd3d11DeviceContext))
        return result;

    delete result;
    return NULL;
}

bool
D3D11DrawContext::create(Far::PatchTables const &patchTables,
                         int numVertexElements,
                         ID3D11DeviceContext *pd3d11DeviceContext)
{
    // adaptive patches
    _isAdaptive = patchTables.IsFeatureAdaptive();

    ID3D11Device *pd3d11Device = NULL;
    pd3d11DeviceContext->GetDevice(&pd3d11Device);
    assert(pd3d11Device);

    // Process PTable
    Far::PatchTables::PatchVertsTable const & ptables = patchTables.GetPatchControlVerticesTable();

    D3D11_BUFFER_DESC bd;
    bd.ByteWidth = (int)ptables.size() * sizeof(int);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = 0;
    bd.StructureByteStride = sizeof(int);
    HRESULT hr = pd3d11Device->CreateBuffer(&bd, NULL, &patchIndexBuffer);
    if (FAILED(hr)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = pd3d11DeviceContext->Map(patchIndexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(hr)) {
        return false;
    }
    unsigned int * indexBuffer = (unsigned int *) mappedResource.pData;
    memcpy(indexBuffer, &ptables[0], ptables.size() * sizeof(unsigned int));

    pd3d11DeviceContext->Unmap(patchIndexBuffer, 0);

    DrawContext::ConvertPatchArrays(patchTables, _patchArrays,
        patchTables.GetMaxValence(), numVertexElements);

    // allocate and initialize additional buffer data

    // create patch param buffer
    Far::PatchParamTable const & patchParamTables =
        patchTables.GetPatchParamTable();

    if (not patchParamTables.empty()) {

        int numElements = (int)patchParamTables.size(),
            elementSize = sizeof(Far::PatchParam);
        unsigned int const * values =
            reinterpret_cast<unsigned int const *>(&patchParamTables[0]);

        std::vector<unsigned int> buffer;
        
        bool useSingleCrease = not patchTables.GetSharpnessIndexTable().empty();
        
        if (useSingleCrease) {
            // if indexed sharpnesses exists, flatten them and interleave into 3-component buffer
            packSharpnessValues(patchTables, buffer);
            elementSize += sizeof(float);
            values = &buffer[0];
        }

        bd.ByteWidth = numElements * elementSize;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = 0;
        bd.StructureByteStride = sizeof(unsigned int);
        hr = pd3d11Device->CreateBuffer(&bd, NULL, &patchParamBuffer);
        if (FAILED(hr)) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
        ZeroMemory(&srvd, sizeof(srvd));
        srvd.Format = useSingleCrease ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32_UINT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvd.Buffer.FirstElement = 0;
        srvd.Buffer.NumElements = numElements;
        hr = pd3d11Device->CreateShaderResourceView(
            patchParamBuffer, &srvd, &patchParamBufferSRV);
        if (FAILED(hr)) {
            return false;
        }
        hr = pd3d11DeviceContext->Map(patchParamBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        if (FAILED(hr)) {
            return false;
        }
        unsigned int *dst = (unsigned int *) mappedResource.pData;
        memcpy(dst, values, numElements * elementSize);
        pd3d11DeviceContext->Unmap(patchParamBuffer, 0);
    }

    // create vertex valence buffer and vertex texture
    Far::PatchTables::VertexValenceTable const &
        valenceTable = patchTables.GetVertexValenceTable();

    if (not valenceTable.empty()) {
        D3D11_BUFFER_DESC bd;
        bd.ByteWidth = UINT(valenceTable.size() * sizeof(unsigned int));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = 0;
        bd.MiscFlags = 0;
        bd.StructureByteStride = sizeof(unsigned int);
        D3D11_SUBRESOURCE_DATA initData;
        initData.pSysMem = &valenceTable[0];
        HRESULT hr = pd3d11Device->CreateBuffer(&bd, &initData, &vertexValenceBuffer);
        if (FAILED(hr)) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
        ZeroMemory(&srvd, sizeof(srvd));
        srvd.Format = DXGI_FORMAT_R32_SINT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvd.Buffer.FirstElement = 0;
        srvd.Buffer.NumElements = UINT(valenceTable.size());
        hr = pd3d11Device->CreateShaderResourceView(vertexValenceBuffer, &srvd, &vertexValenceBufferSRV);
        if (FAILED(hr)) {
            return false;
        }
    }

    Far::PatchTables::QuadOffsetsTable const &
        quadOffsetTable = patchTables.GetQuadOffsetsTable();

    if (not quadOffsetTable.empty()) {
        D3D11_BUFFER_DESC bd;
        bd.ByteWidth = UINT(quadOffsetTable.size() * sizeof(unsigned int));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = 0;
        bd.MiscFlags = 0;
        bd.StructureByteStride = sizeof(unsigned int);
        D3D11_SUBRESOURCE_DATA initData;
        initData.pSysMem = &quadOffsetTable[0];
        HRESULT hr = pd3d11Device->CreateBuffer(&bd, &initData, &quadOffsetBuffer);
        if (FAILED(hr)) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
        ZeroMemory(&srvd, sizeof(srvd));
        srvd.Format = DXGI_FORMAT_R32_SINT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvd.Buffer.FirstElement = 0;
        srvd.Buffer.NumElements = UINT(quadOffsetTable.size());
        hr = pd3d11Device->CreateShaderResourceView(quadOffsetBuffer, &srvd, &quadOffsetBufferSRV);
        if (FAILED(hr)) {
            return false;
        }
    }

    return true;
}

bool
D3D11DrawContext::SetFVarDataTexture(Far::PatchTables const & patchTables,
                                     int fvarWidth, FVarData const & fvarData,
                                     ID3D11DeviceContext *pd3d11DeviceContext) {

    if (not fvarData.empty()) {

        FVarData fvarDataTable;

        packFVarData(patchTables, fvarWidth, fvarData, fvarDataTable);

        ID3D11Device *pd3d11Device = NULL;
        pd3d11DeviceContext->GetDevice(&pd3d11Device);
        assert(pd3d11Device);

        D3D11_BUFFER_DESC bd;
        bd.ByteWidth = UINT(fvarDataTable.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = 0;
        bd.MiscFlags = 0;
        bd.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA initData;
        initData.pSysMem = &fvarDataTable[0];
        HRESULT hr = pd3d11Device->CreateBuffer(&bd, &initData, &fvarDataBuffer);
        if (FAILED(hr)) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
        ZeroMemory(&srvd, sizeof(srvd));
        srvd.Format = DXGI_FORMAT_R32_FLOAT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvd.Buffer.FirstElement = 0;
        srvd.Buffer.NumElements = UINT(fvarDataTable.size());
        hr = pd3d11Device->CreateShaderResourceView(fvarDataBuffer, &srvd, &fvarDataBufferSRV);
        if (FAILED(hr)) {
            return false;
        }
    }
    return true;
}

void
D3D11DrawContext::updateVertexTexture(ID3D11Buffer *vbo,
                                         ID3D11DeviceContext *pd3d11DeviceContext,
                                         int numVertices,
                                         int numVertexElements)
{
    ID3D11Device *pd3d11Device = NULL;
    pd3d11DeviceContext->GetDevice(&pd3d11Device);
    assert(pd3d11Device);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
    ZeroMemory(&srvd, sizeof(srvd));
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvd.Buffer.FirstElement = 0;
    srvd.Buffer.NumElements = numVertices * numVertexElements;
    HRESULT hr = pd3d11Device->CreateShaderResourceView(vbo, &srvd, &vertexBufferSRV);
    if (FAILED(hr)) {
        return;
    }
}

}  // end namespace Osd

}  // end namespace OPENSUBDIV_VERSION
} // end namespace OpenSubdiv
