#pragma once

#include "DmlRuntimeGraphFusionHelper.h"


namespace Dml
{
namespace DmlRuntimeGraphFusionHelper
{
    Microsoft::WRL::ComPtr<ID3D12Resource>
    CreateResource(
        const ExecutionProviderImpl* provider,
        const std::byte* tensorPtr,
        size_t tensorByteSize)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

        D3D12_HEAP_PROPERTIES heapProperties = {
            D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

        D3D12_RESOURCE_DESC resourceDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            static_cast<uint64_t>((tensorByteSize + 3) & ~3),
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

        Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
        ORT_THROW_IF_FAILED(provider->GetD3DDevice(d3dDevice.GetAddressOf()));

        ORT_THROW_IF_FAILED(d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(buffer.GetAddressOf())));

        ORT_THROW_IF_FAILED(provider->UploadToResource(buffer.Get(), tensorPtr, tensorByteSize));

        return buffer;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource>
    CreateCpuResource(
        const ExecutionProviderImpl* provider,
        const std::byte* tensorPtr,
        size_t tensorByteSize)
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;

        D3D12_HEAP_PROPERTIES heapProperties = {
            D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_MEMORY_POOL_L0, 0, 0};

        D3D12_RESOURCE_DESC resourceDesc = {D3D12_RESOURCE_DIMENSION_BUFFER,
                                            0,
                                            static_cast<uint64_t>((tensorByteSize + 3) & ~3),
                                            1,
                                            1,
                                            1,
                                            DXGI_FORMAT_UNKNOWN,
                                            {1, 0},
                                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};

        Microsoft::WRL::ComPtr<ID3D12Device> d3dDevice;
        ORT_THROW_IF_FAILED(provider->GetD3DDevice(d3dDevice.GetAddressOf()));

        ORT_THROW_IF_FAILED(d3dDevice->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_GRAPHICS_PPV_ARGS(buffer.GetAddressOf())));

        // Map the buffer and copy the data
        void* bufferData = nullptr;
        D3D12_RANGE range = {0, tensorByteSize};
        ORT_THROW_IF_FAILED(buffer->Map(0, &range, &bufferData));
        memcpy(bufferData, tensorPtr, tensorByteSize);
        buffer->Unmap(0, &range);

        return buffer;
    }

    void UnwrapTensor(
        Windows::AI::MachineLearning::Adapter::IWinmlExecutionProvider* winmlProvider,
        const onnxruntime::Tensor* tensor,
        ID3D12Resource** resource,
        uint64_t* allocId)
    {
        IUnknown* allocationUnk = static_cast<IUnknown*>(const_cast<void*>(tensor->DataRaw()));
        Microsoft::WRL::ComPtr<IUnknown> resourceUnk;
        winmlProvider->GetABIDataInterface(false, allocationUnk, &resourceUnk);

        *allocId = winmlProvider->TryGetPooledAllocationId(allocationUnk, 0);

        ORT_THROW_IF_FAILED(resourceUnk->QueryInterface(resource));
    }

    std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>>
    GetInitializerToPartitionMap(
        const onnxruntime::GraphViewer& graph,
        gsl::span<std::unique_ptr<GraphPartition>> partitions
    )
    {
        std::unordered_map<const onnx::TensorProto*, std::vector<uint32_t>> initializerPartitionMap;
        for (uint32_t partitionIndex = 0; partitionIndex < gsl::narrow_cast<uint32_t>(partitions.size()); ++partitionIndex)
        {
            auto& partition = partitions[partitionIndex];

            // Skip partitions which have been merged into other partitions
            if (partition->GetRootMergedPartition() != partition.get())
            {
                continue;
            }

            for (const std::string& input : partition->GetInputs())
            {
                const onnx::TensorProto* tensor = nullptr;
                if (graph.GetInitializedTensor(input, tensor))
                {
                    initializerPartitionMap[tensor].push_back(partitionIndex);
                }
            }
        }

        return initializerPartitionMap;
    }

    void ConvertGraphDesc(
        const Dml::GraphDescBuilder::GraphDesc& graphDesc,
        _Out_ DML_GRAPH_DESC& dmlGraphDesc,
        const uint32_t inputCount,
        const uint32_t outputCount,
        _Inout_ std::vector<DML_OPERATOR_GRAPH_NODE_DESC>& dmlOperatorGraphNodes,
        _Inout_ std::vector<DML_GRAPH_NODE_DESC>& dmlGraphNodes,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlInputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlOutputEdges,
        _Inout_ std::vector<DML_GRAPH_EDGE_DESC>& dmlIntermediateEdges)
    {
        for (size_t i = 0; i < graphDesc.nodes.size(); ++i)
        {
            auto& nodeInfo = graphDesc.nodes[i];
            dmlOperatorGraphNodes[i] = DML_OPERATOR_GRAPH_NODE_DESC{nodeInfo.op.Get(), nodeInfo.name.data()};
            dmlGraphNodes[i] = DML_GRAPH_NODE_DESC{DML_GRAPH_NODE_TYPE_OPERATOR, &dmlOperatorGraphNodes[i]};
        }

        for (size_t i = 0; i < graphDesc.inputEdges.size(); ++i)
        {
            dmlInputEdges[i] = DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INPUT, &graphDesc.inputEdges[i]};
        }

        for (size_t i = 0; i < graphDesc.outputEdges.size(); ++i)
        {
            dmlOutputEdges[i] = DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_OUTPUT, &graphDesc.outputEdges[i]};
        }

        for (size_t i = 0; i < graphDesc.intermediateEdges.size(); ++i)
        {
            dmlIntermediateEdges[i] =
                DML_GRAPH_EDGE_DESC{DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &graphDesc.intermediateEdges[i]};
        }

        dmlGraphDesc.InputCount = inputCount;
        dmlGraphDesc.OutputCount = outputCount;
        dmlGraphDesc.NodeCount = gsl::narrow_cast<uint32_t>(dmlGraphNodes.size());
        dmlGraphDesc.Nodes = dmlGraphNodes.data();
        dmlGraphDesc.InputEdgeCount = gsl::narrow_cast<uint32_t>(dmlInputEdges.size());
        dmlGraphDesc.InputEdges = dmlInputEdges.data();
        dmlGraphDesc.OutputEdgeCount = gsl::narrow_cast<uint32_t>(dmlOutputEdges.size());
        dmlGraphDesc.OutputEdges = dmlOutputEdges.data();
        dmlGraphDesc.IntermediateEdgeCount = gsl::narrow_cast<uint32_t>(dmlIntermediateEdges.size());
        dmlGraphDesc.IntermediateEdges = dmlIntermediateEdges.data();
    }

    onnxruntime::IndexedSubGraph CreateIndexedSubGraph(
        GraphPartition* partition,
        uint32_t partitionIndex,
        const std::string& partitionKernelPrefix)
    {
        assert(partition->IsDmlGraphPartition());

        onnxruntime::IndexedSubGraph indexedSubGraph;
        // Create a definition for the node.  The name must be unique.
        auto def = std::make_unique<onnxruntime::IndexedSubGraph::MetaDef>();
        def->name = DmlRuntimeGraphFusionTransformer::DML_GRAPH_FUSION_NODE_NAME_PREFIX + partitionKernelPrefix + std::to_string(partitionIndex);
        def->domain = DmlRuntimeGraphFusionTransformer::DML_GRAPH_FUSION_NODE_DOMAIN;
        def->since_version = 1;
        def->inputs.insert(def->inputs.begin(), partition->GetInputs().begin(), partition->GetInputs().end());
        def->outputs.insert(def->outputs.begin(), partition->GetOutputs().begin(), partition->GetOutputs().end());

        indexedSubGraph.SetMetaDef(std::move(def));
        indexedSubGraph.nodes = std::move(partition->GetNodeIndices());

        return indexedSubGraph;
    }

    std::unordered_map<std::string, GraphNodeProperties> CreatePartitionNodePropsMap(
        const onnxruntime::Graph& graph,
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties>&& graphNodePropertyMap)
    {
        // Populate properties which will be passed to OpKernel for this graph via the function below
        std::unordered_map<std::string, GraphNodeProperties> partitionNodePropsMap;
        for (auto nodeIndex : indexedSubGraph.nodes)
        {
            const onnxruntime::Node* node = graph.GetNode(nodeIndex);

#ifdef PRINT_PARTITON_INFO
            printf("Partition %u\t%s\n", partitionIndex, GraphDescBuilder::GetUniqueNodeName(*node).c_str());
#endif
            partitionNodePropsMap.insert(std::make_pair(
                GraphDescBuilder::GetUniqueNodeName(*node), std::move(graphNodePropertyMap[node])));
        }

#ifdef PRINT_PARTITON_INFO
        printf("\n");
#endif

        return partitionNodePropsMap;
    }

    Microsoft::WRL::ComPtr<IDMLCompiledOperator> TryCreateCompiledOperator(
        const GraphDescBuilder::GraphDesc& graphDesc,
        const onnxruntime::IndexedSubGraph& indexedSubGraph,
        const ExecutionProviderImpl* providerImpl)
    {
        const uint32_t fusedNodeInputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->inputs.size());
        const uint32_t fusedNodeOutputCount = gsl::narrow_cast<uint32_t>(indexedSubGraph.GetMetaDef()->outputs.size());

        // convert DML EP GraphDesc into DML_GRAPH_DESC and create IDMLCompiledOperator
        DML_GRAPH_DESC dmlGraphDesc = {};
        std::vector<DML_OPERATOR_GRAPH_NODE_DESC> dmlOperatorGraphNodes(graphDesc.nodes.size());
        std::vector<DML_GRAPH_NODE_DESC> dmlGraphNodes(graphDesc.nodes.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlInputEdges(graphDesc.inputEdges.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlOutputEdges(graphDesc.outputEdges.size());
        std::vector<DML_GRAPH_EDGE_DESC> dmlIntermediateEdges(graphDesc.intermediateEdges.size());
        ConvertGraphDesc(
            graphDesc,
            dmlGraphDesc,
            fusedNodeInputCount,
            fusedNodeOutputCount,
            dmlOperatorGraphNodes,
            dmlGraphNodes,
            dmlInputEdges,
            dmlOutputEdges,
            dmlIntermediateEdges);

        DML_EXECUTION_FLAGS executionFlags = DML_EXECUTION_FLAG_NONE;
        if (graphDesc.reuseCommandList)
        {
            executionFlags |= DML_EXECUTION_FLAG_DESCRIPTORS_VOLATILE;
        }

        // Query DML execution provider to see if metacommands is enabled
        if (!providerImpl->MetacommandsEnabled())
        {
            executionFlags |= DML_EXECUTION_FLAG_DISABLE_META_COMMANDS;
        }

        ComPtr<IDMLDevice> device;
        ORT_THROW_IF_FAILED(providerImpl->GetDmlDevice(device.GetAddressOf()));

        ComPtr<IDMLDevice1> device1;
        ORT_THROW_IF_FAILED(device.As(&device1));

        ComPtr<IDMLCompiledOperator> compiledExecutionPlanOperator;
        ORT_THROW_IF_FAILED(device1->CompileGraph(
            &dmlGraphDesc,
            executionFlags,
            IID_PPV_ARGS(&compiledExecutionPlanOperator)));

        // UINT32_MAX is currently the maximum number of bytes allowed by D3D12 for the offset of a view over a resource
        if (compiledExecutionPlanOperator->GetBindingProperties().PersistentResourceSize > UINT32_MAX)
        {
            return nullptr;
        }

        return compiledExecutionPlanOperator;
    }

    void RegisterKernel(
        onnxruntime::Graph& graph,
        onnxruntime::KernelRegistry* registryForPartitionKernels,
        const ExecutionProviderImpl* providerImpl,
        std::unordered_map<const onnxruntime::Node*, GraphNodeProperties> graphNodePropertyMap,
        std::shared_ptr<onnxruntime::IndexedSubGraph> indexedSubGraph)
    {
        auto partitionNodePropsMap = DmlRuntimeGraphFusionHelper::CreatePartitionNodePropsMap(
            graph,
            *indexedSubGraph,
            std::move(graphNodePropertyMap));

        // lamda captures for the kernel registration
        auto fused_kernel_func = [
            &graph,
            indexedSubGraph,
            partitionNodePropsMap = std::move(partitionNodePropsMap)] (onnxruntime::FuncManager& func_mgr, const onnxruntime::OpKernelInfo& info, std::unique_ptr<onnxruntime::OpKernel>& out) mutable ->onnxruntime::Status
        {
            out.reset(CreateRuntimeFusedGraphKernel(info, graph, *indexedSubGraph, std::move(partitionNodePropsMap)));
            return Status::OK();
        };

        // build the kernel definition on the fly, and register it to the fused_kernel_regisitry.
        onnxruntime::KernelDefBuilder builder;
        builder.SetName(indexedSubGraph->GetMetaDef()->name)
            .SetDomain(indexedSubGraph->GetMetaDef()->domain)
            .SinceVersion(indexedSubGraph->GetMetaDef()->since_version)
            .Provider(onnxruntime::kDmlExecutionProvider);
        ORT_THROW_IF_ERROR(registryForPartitionKernels->Register(builder, fused_kernel_func));

        auto& fusedNode = graph.BeginFuseSubGraph(*indexedSubGraph, indexedSubGraph->GetMetaDef()->name);
        fusedNode.SetExecutionProviderType(onnxruntime::kDmlExecutionProvider);

        graph.FinalizeFuseSubGraph(*indexedSubGraph, fusedNode);
    }
}
}
